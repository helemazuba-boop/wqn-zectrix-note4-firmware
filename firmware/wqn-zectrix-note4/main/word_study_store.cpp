#include "word_study_store.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "storage.h"  // GetDeckScopeGeneration (deck-scope session validation)
#include "esp_timer.h"
#include "runtime/sleep_coordinator.h"
#include "services/storage_service.h"

namespace {

constexpr char kTag[] = "word_store";
constexpr char kOutboxPath[] = "/storage/wout.v1";
constexpr char kOutboxTempPath[] = "/storage/wout.tmp";
constexpr char kOutboxBackupPath[] = "/storage/wout.bak";
constexpr char kRejectedOutboxPath[] = "/storage/wrej.v1";
constexpr char kRejectedOutboxTempPath[] = "/storage/wrej.tmp";
constexpr char kRejectedOutboxBackupPath[] = "/storage/wrej.bak";
constexpr uint32_t kSessionMagic = UINT32_C(0x53535157);  // WQSS
constexpr uint32_t kOutboxMagic = UINT32_C(0x424f5157);  // WQOB
// v3 appends deck_scope_generation after next_sequence: sessions are pinned to
// the default-deck scope they were built under, and a load whose stamp no
// longer matches the committed scope generation reports NOT_FOUND. Bumping the
// version discards v2 snapshots once at upgrade -- acceptable, they are only
// browse cursors (the observation outbox is a separate, versioned store).
// v4 appends start_index (the 顺序过词库 continuation point) for the same
// reason: it is a browse cursor, and a v3 file simply loses its label.
constexpr uint16_t kSessionSchemaVersion = 4;
constexpr uint16_t kOutboxSchemaVersion = 1;
constexpr size_t kMaxSessionPayloadBytes = 96U * 1024U;
constexpr size_t kMaxSessionCursorBytes = 256;
constexpr size_t kRuntimeCompactAckThreshold = 128;
constexpr size_t kRejectedOutboxCapacity = 256;
constexpr wqn::protocol::word_study_v1::Mode kPersistedSessionModes[] = {
    wqn::protocol::word_study_v1::Mode::kSequential,
    wqn::protocol::word_study_v1::Mode::kRandom,
    wqn::protocol::word_study_v1::Mode::kDictionary,
    wqn::protocol::word_study_v1::Mode::kReview,
    wqn::protocol::word_study_v1::Mode::kIntake,
    wqn::protocol::word_study_v1::Mode::kShuffle,
    wqn::protocol::word_study_v1::Mode::kMistakes,
};

#pragma pack(push, 1)
struct SessionHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t payload_size;
    uint32_t payload_crc;
};

enum class OutboxRecordKind : uint8_t {
    kObservation = 1,
    kAck = 2,
    // Parked record: pairs with a preceding kObservation by request_id and
    // removes it from the upload queue WITHOUT deleting the payload. The
    // suspend reason rides in OutboxRecord::reserved. Old firmware builds
    // treat this kind as an invalid response and fall back to the .bak
    // journal (documented downgrade caveat; upgrades parse it natively).
    kSuspend = 3,
};

struct OutboxRecord {
    uint32_t magic;
    uint16_t version;
    uint8_t kind;
    uint8_t action;
    uint8_t mode;
    uint8_t next_phase;
    uint16_t reserved;
    uint64_t sequence;
    uint32_t next_position;
    char request_id[65];
    char session_id[37];
    char item_id[37];
    char occurred_at[33];
    uint32_t crc;
};
#pragma pack(pop)

static_assert(sizeof(OutboxRecord) == 200);

struct OutboxScan {
    std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>> pending;
    // Preserve the observation paired with each ACK. ACK records only carry
    // request_id; their preceding observation carries the durable cursor.
    std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>> acknowledged;
    // Observations parked by a kSuspend marker: excluded from Peek/upload,
    // but still rewritten by compaction so the payload survives on device.
    std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>> suspended;
    // Parallel to `suspended`: preserves the marker reason across compaction.
    std::vector<uint16_t, wqn::WordStorePsramAllocator<uint16_t>> suspended_reasons;
    size_t total_records = 0;
    size_t ack_records = 0;
    size_t suspend_records = 0;
    size_t orphan_ack_records = 0;
    size_t orphan_suspend_records = 0;
    bool partial_tail = false;
    bool backup_source = false;
};

// Every public operation below runs on StorageService's sole owner task. Keep
// the parsed journal resident there so card actions do not re-read and CRC the
// complete SPIFFS file. The cache is rebuilt after boot and updated only after
// the corresponding append/compaction has become durable.
OutboxScan g_outbox_cache;
bool g_outbox_cache_loaded = false;
// Storage-owner-only. A failed write/partial clear also invalidates an older
// checkpoint proof: the disk may have changed before an error was returned.
uint64_t g_session_mutation_generation = 0;

struct SessionPaths {
    const char* primary;
    const char* temporary;
    const char* backup;
};

bool GetSessionPaths(
    wqn::protocol::word_study_v1::Mode mode,
    SessionPaths* paths)
{
    if (paths == nullptr) return false;
    switch (mode) {
        case wqn::protocol::word_study_v1::Mode::kSequential:
            *paths = {"/storage/wsq.v1", "/storage/wsq.tmp", "/storage/wsq.bak"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kRandom:
            *paths = {"/storage/wsr.v1", "/storage/wsr.tmp", "/storage/wsr.bak"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kDictionary:
            *paths = {"/storage/wsd.v1", "/storage/wsd.tmp", "/storage/wsd.bak"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kReview:
            *paths = {"/storage/wsv.v1", "/storage/wsv.tmp", "/storage/wsv.bak"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kIntake:
            *paths = {"/storage/wsi.v1", "/storage/wsi.tmp", "/storage/wsi.bak"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kShuffle:
            *paths = {"/storage/wsh.v1", "/storage/wsh.tmp", "/storage/wsh.bak"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kMistakes:
            *paths = {"/storage/wsm.v1", "/storage/wsm.tmp", "/storage/wsm.bak"};
            return true;
    }
    return false;
}

uint32_t Crc32(const void* bytes, size_t size)
{
    return esp_rom_crc32_le(
        UINT32_MAX,
        static_cast<const uint8_t*>(bytes),
        static_cast<uint32_t>(size)) ^ UINT32_MAX;
}

bool CopyFixedText(char* output, size_t output_size, const std::string& value)
{
    if (output == nullptr || output_size == 0 || value.size() >= output_size) {
        return false;
    }
    std::memset(output, 0, output_size);
    std::memcpy(output, value.data(), value.size());
    return true;
}

bool FileExists(const char* path)
{
    struct stat status = {};
    return path != nullptr && stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

template <typename T>
void AppendScalar(std::vector<uint8_t>* output, T value)
{
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    output->insert(output->end(), bytes, bytes + sizeof(value));
}

bool AppendString(std::vector<uint8_t>* output, const std::string& value, size_t maximum)
{
    if (output == nullptr || value.size() > maximum || value.size() > UINT16_MAX) {
        return false;
    }
    AppendScalar<uint16_t>(output, static_cast<uint16_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
    return output->size() <= kMaxSessionPayloadBytes;
}

class PayloadReader {
public:
    PayloadReader(const uint8_t* bytes, size_t size) : bytes_(bytes), size_(size) {}

    template <typename T>
    bool Scalar(T* value)
    {
        if (value == nullptr || offset_ + sizeof(T) > size_) return false;
        std::memcpy(value, bytes_ + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    bool String(std::string* value, size_t maximum)
    {
        uint16_t length = 0;
        if (value == nullptr || !Scalar(&length) || length > maximum ||
            offset_ + length > size_) {
            return false;
        }
        value->assign(reinterpret_cast<const char*>(bytes_ + offset_), length);
        offset_ += length;
        return true;
    }

    bool finished() const { return offset_ == size_; }

private:
    const uint8_t* bytes_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
};

bool ValidMode(uint8_t value)
{
    return value <= static_cast<uint8_t>(wqn::protocol::word_study_v1::Mode::kMistakes);
}

bool ValidPurpose(uint8_t value)
{
    return value <= static_cast<uint8_t>(wqn::protocol::word_study_v1::Purpose::kLookup);
}

bool ValidOrdering(uint8_t value)
{
    return value <=
        static_cast<uint8_t>(wqn::protocol::word_study_v1::Ordering::kMistakeWordsV1);
}

bool EncodeSession(
    const wqn::PersistedWordSession& session,
    std::vector<uint8_t>* payload)
{
    using namespace wqn::protocol::word_study_v1;
    if (payload == nullptr || session.remote.snapshot.size() > kMaxDecks ||
        session.remote.deck_ids.size() > kMaxDecks ||
        session.remote.items.size() > kMaxSessionItems ||
        session.position > session.remote.items.size() ||
        session.remote.optional_count < 0 || session.remote.optional_count > 500) {
        return false;
    }
    payload->clear();
    payload->reserve(4096);
    AppendScalar<uint8_t>(payload, session.active ? 1 : 0);
    AppendScalar<uint8_t>(payload, session.paused ? 1 : 0);
    AppendScalar<uint8_t>(payload, static_cast<uint8_t>(session.phase));
    AppendScalar<uint8_t>(payload, static_cast<uint8_t>(session.remote.mode));
    AppendScalar<uint8_t>(payload, static_cast<uint8_t>(session.remote.purpose));
    AppendScalar<uint8_t>(payload, static_cast<uint8_t>(session.remote.ordering));
    AppendScalar<uint8_t>(payload, session.remote.include_mastered ? 1 : 0);
    AppendScalar<uint8_t>(payload, session.remote.has_more ? 1 : 0);
    AppendScalar<uint32_t>(payload, session.position);
    AppendScalar<uint32_t>(payload, session.start_index);
    AppendScalar<uint32_t>(payload, static_cast<uint32_t>(session.remote.optional_count));
    AppendScalar<uint64_t>(payload, session.remote.next_sequence);
    // [deck-scope] The session's OWN scope stamp (assigned when the session
    // was created, inherited by every advanced copy). Deliberately NOT the
    // current global generation: a stale runner-side save must fail the
    // SaveSessionRaw validation below, not get re-stamped as current.
    AppendScalar<uint32_t>(payload, session.deck_scope_generation);
    if (!AppendString(payload, session.remote.session_id, 36) ||
        !AppendString(payload, session.remote.seed, 64) ||
        !AppendString(payload, session.remote.cursor, kMaxSessionCursorBytes)) {
        return false;
    }
    AppendScalar<uint16_t>(payload, static_cast<uint16_t>(session.remote.deck_ids.size()));
    for (const wqn::StoredWordDeckId& deck_id : session.remote.deck_ids) {
        if (!AppendString(payload, deck_id.value, 36)) return false;
    }
    AppendScalar<uint16_t>(payload, static_cast<uint16_t>(session.remote.snapshot.size()));
    for (const wqn::StoredWordPackSnapshot& snapshot : session.remote.snapshot) {
        if (!AppendString(payload, snapshot.deck_id, 36)) return false;
        AppendScalar<uint64_t>(payload, snapshot.content_revision);
        AppendScalar<uint64_t>(payload, snapshot.pack_revision);
        if (!AppendString(payload, snapshot.sha256, 64)) return false;
    }
    AppendScalar<uint16_t>(payload, static_cast<uint16_t>(session.remote.items.size()));
    for (const wqn::StoredWordSessionItem& item : session.remote.items) {
        if (!AppendString(payload, item.item_id, 36) ||
            !AppendString(payload, item.deck_id, 36)) {
            return false;
        }
        AppendScalar<uint64_t>(payload, item.ordinal);
    }
    AppendScalar<uint64_t>(payload, session.remote.progress_revision);
    return payload->size() <= kMaxSessionPayloadBytes;
}

bool DecodeSession(
    const std::vector<uint8_t>& payload,
    wqn::PersistedWordSession* session)
{
    using namespace wqn::protocol::word_study_v1;
    if (session == nullptr) return false;
    PayloadReader reader(payload.data(), payload.size());
    uint8_t active = 0;
    uint8_t paused = 0;
    uint8_t phase = 0;
    uint8_t mode = 0;
    uint8_t purpose = 0;
    uint8_t ordering = 0;
    uint8_t include_mastered = 0;
    uint8_t has_more = 0;
    uint32_t optional_count = 0;
    wqn::PersistedWordSession parsed;
    if (!reader.Scalar(&active) || !reader.Scalar(&paused) ||
        !reader.Scalar(&phase) || !reader.Scalar(&mode) ||
        !reader.Scalar(&purpose) || !reader.Scalar(&ordering) ||
        !reader.Scalar(&include_mastered) || !reader.Scalar(&has_more) ||
        !reader.Scalar(&parsed.position) ||
        !reader.Scalar(&parsed.start_index) ||
        !reader.Scalar(&optional_count) ||
        !reader.Scalar(&parsed.remote.next_sequence) ||
        !reader.Scalar(&parsed.deck_scope_generation) ||
        !reader.String(&parsed.remote.session_id, 36) ||
        !reader.String(&parsed.remote.seed, 64) ||
        !reader.String(&parsed.remote.cursor, kMaxSessionCursorBytes) || phase > 1 ||
        !ValidMode(mode) || !ValidPurpose(purpose) || !ValidOrdering(ordering) ||
        optional_count > 500) {
        return false;
    }
    parsed.active = active == 1;
    parsed.paused = paused == 1;
    parsed.phase = static_cast<wqn::WordPresentationPhase>(phase);
    parsed.remote.mode = static_cast<Mode>(mode);
    parsed.remote.purpose = static_cast<Purpose>(purpose);
    parsed.remote.ordering = static_cast<Ordering>(ordering);
    if (active > 1 || paused > 1 || include_mastered > 1 || has_more > 1) {
        return false;
    }
    parsed.remote.include_mastered = include_mastered == 1;
    parsed.remote.has_more = has_more == 1;
    parsed.remote.optional_count = static_cast<int>(optional_count);

    uint16_t count = 0;
    if (!reader.Scalar(&count) || count > kMaxDecks) return false;
    parsed.remote.deck_ids.reserve(count);
    for (uint16_t index = 0; index < count; ++index) {
        std::string value;
        if (!reader.String(&value, 36)) return false;
        wqn::StoredWordDeckId deck;
        if (!CopyFixedText(deck.value, sizeof(deck.value), value)) return false;
        parsed.remote.deck_ids.push_back(deck);
    }
    if (!reader.Scalar(&count) || count > kMaxDecks) return false;
    parsed.remote.snapshot.reserve(count);
    for (uint16_t index = 0; index < count; ++index) {
        wqn::StoredWordPackSnapshot snapshot;
        std::string deck_id;
        std::string sha256;
        if (!reader.String(&deck_id, 36) ||
            !reader.Scalar(&snapshot.content_revision) ||
            !reader.Scalar(&snapshot.pack_revision) ||
            !reader.String(&sha256, 64) ||
            !CopyFixedText(snapshot.deck_id, sizeof(snapshot.deck_id), deck_id) ||
            !CopyFixedText(snapshot.sha256, sizeof(snapshot.sha256), sha256)) {
            return false;
        }
        parsed.remote.snapshot.push_back(std::move(snapshot));
    }
    if (!reader.Scalar(&count) || count > kMaxSessionItems) return false;
    parsed.remote.items.reserve(count);
    for (uint16_t index = 0; index < count; ++index) {
        wqn::StoredWordSessionItem item;
        std::string item_id;
        std::string deck_id;
        if (!reader.String(&item_id, 36) ||
            !reader.String(&deck_id, 36) ||
            !CopyFixedText(item.item_id, sizeof(item.item_id), item_id) ||
            !CopyFixedText(item.deck_id, sizeof(item.deck_id), deck_id) ||
            !reader.Scalar(&item.ordinal)) {
            return false;
        }
        parsed.remote.items.push_back(std::move(item));
    }
    if (!reader.Scalar(&parsed.remote.progress_revision)) {
        return false;
    }
    if (!reader.finished() || parsed.position > parsed.remote.items.size()) return false;
    *session = std::move(parsed);
    return true;
}

esp_err_t AtomicWrite(
    const char* primary,
    const char* temporary,
    const char* backup,
    const void* bytes,
    size_t size,
    bool preserve_backup = false)
{
    FILE* file = std::fopen(temporary, "wb");
    if (file == nullptr) return ESP_FAIL;
    const bool written = std::fwrite(bytes, 1, size, file) == size;
    const bool durable = written && std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!durable || !closed) {
        std::remove(temporary);
        return ESP_FAIL;
    }
    const bool had_primary = FileExists(primary);
    if (preserve_backup) {
        if (had_primary && std::remove(primary) != 0 && errno != ENOENT) {
            std::remove(temporary);
            return ESP_FAIL;
        }
        if (std::rename(temporary, primary) != 0) {
            std::remove(temporary);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (had_primary) {
        if (std::remove(backup) != 0 && errno != ENOENT) {
            std::remove(temporary);
            return ESP_FAIL;
        }
        if (std::rename(primary, backup) != 0) {
            std::remove(temporary);
            return ESP_FAIL;
        }
    }
    if (std::rename(temporary, primary) != 0) {
        if (had_primary) std::rename(backup, primary);
        std::remove(temporary);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t SaveSessionRaw(
    const wqn::PersistedWordSession& session,
    bool preserve_backup = false)
{
    SessionPaths paths = {};
    if (!GetSessionPaths(session.remote.mode, &paths)) {
        return ESP_ERR_INVALID_ARG;
    }
    // [deck-scope] Refuse to persist a session built under a different scope.
    // A deck switch can land between a session request being queued and the
    // runner-side save executing; without this gate the stale session would
    // re-materialize on disk right after the switch wiped it.
    if (session.deck_scope_generation != wqn::GetDeckScopeGeneration()) {
        ESP_LOGW(kTag,
                 "word session save rejected: scope generation %u != committed %u",
                 static_cast<unsigned>(session.deck_scope_generation),
                 static_cast<unsigned>(wqn::GetDeckScopeGeneration()));
        return ESP_ERR_INVALID_STATE;
    }
    std::vector<uint8_t> payload;
    if (!EncodeSession(session, &payload)) return ESP_ERR_INVALID_ARG;
    SessionHeader header = {};
    header.magic = kSessionMagic;
    header.version = kSessionSchemaVersion;
    header.payload_size = static_cast<uint32_t>(payload.size());
    header.payload_crc = Crc32(payload.data(), payload.size());
    std::vector<uint8_t> file_bytes(sizeof(header) + payload.size());
    std::memcpy(file_bytes.data(), &header, sizeof(header));
    std::memcpy(file_bytes.data() + sizeof(header), payload.data(), payload.size());
    ++g_session_mutation_generation;
    return AtomicWrite(
        paths.primary,
        paths.temporary,
        paths.backup,
        file_bytes.data(),
        file_bytes.size(),
        preserve_backup);
}

// Small mutable companion to the per-mode session snapshot. Pause/resume only
// flip PersistedWordSession::paused, but rewriting the whole candidate snapshot
// (up to 500 items) to persist that one bit cost ~2-3 s on the UI thread. The
// cursor records just the flag; LoadSessionTransaction overlays it after outbox
// reconciliation, so pause/resume no longer rewrite the snapshot.
constexpr uint32_t kSessionCursorMagic = 0x57435552;  // 'WCUR'
constexpr uint16_t kSessionCursorVersion = 1;

struct SessionCursorRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t paused;
    char session_id[37];
    char reserved[3];
    uint32_t crc;
};
static_assert(sizeof(SessionCursorRecord) == 52, "cursor record must be tightly packed");

// [word-session-cursor-nvs] NVS keys for the same seven modes. NVS keys are
// capped at NVS_KEY_NAME_MAX_SIZE (15 usable characters) and must be stable
// across firmware versions, because a rename orphans the stored value and the
// cursor silently falls back to the legacy file for every mode.
const char* GetSessionCursorNvsKey(wqn::protocol::word_study_v1::Mode mode)
{
    switch (mode) {
        case wqn::protocol::word_study_v1::Mode::kSequential:
            return "cur_seq";
        case wqn::protocol::word_study_v1::Mode::kRandom:
            return "cur_rnd";
        case wqn::protocol::word_study_v1::Mode::kDictionary:
            return "cur_dic";
        case wqn::protocol::word_study_v1::Mode::kReview:
            return "cur_rev";
        case wqn::protocol::word_study_v1::Mode::kIntake:
            return "cur_int";
        case wqn::protocol::word_study_v1::Mode::kShuffle:
            return "cur_shf";
        case wqn::protocol::word_study_v1::Mode::kMistakes:
            return "cur_mis";
    }
    return nullptr;
}

bool GetSessionCursorPaths(
    wqn::protocol::word_study_v1::Mode mode,
    SessionPaths* paths)
{
    if (paths == nullptr) return false;
    switch (mode) {
        case wqn::protocol::word_study_v1::Mode::kSequential:
            *paths = {"/storage/wsq.cur", "/storage/wsq.ctp", "/storage/wsq.cbk"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kRandom:
            *paths = {"/storage/wsr.cur", "/storage/wsr.ctp", "/storage/wsr.cbk"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kDictionary:
            *paths = {"/storage/wsd.cur", "/storage/wsd.ctp", "/storage/wsd.cbk"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kReview:
            *paths = {"/storage/wsv.cur", "/storage/wsv.ctp", "/storage/wsv.cbk"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kIntake:
            *paths = {"/storage/wsi.cur", "/storage/wsi.ctp", "/storage/wsi.cbk"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kShuffle:
            *paths = {"/storage/wsh.cur", "/storage/wsh.ctp", "/storage/wsh.cbk"};
            return true;
        case wqn::protocol::word_study_v1::Mode::kMistakes:
            *paths = {"/storage/wsm.cur", "/storage/wsm.ctp", "/storage/wsm.cbk"};
            return true;
    }
    return false;
}

esp_err_t WriteSessionCursor(const wqn::PersistedWordSession& session)
{
    // [word-session-cursor-nvs] The .cur/.ctp/.cbk paths are no longer written;
    // they are only read, and only as the one-time migration fallback below.
    // GetSessionCursorNvsKey covers the same seven modes, so it is also the
    // invalid-mode check that GetSessionCursorPaths used to be.
    const char* nvs_key = GetSessionCursorNvsKey(session.remote.mode);
    if (nvs_key == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // [deck-scope] Same guard as SaveSessionRaw: a stale pause/resume cursor
    // for an old-scope session must not be written after a deck switch.
    if (session.deck_scope_generation != wqn::GetDeckScopeGeneration()) {
        ESP_LOGW(kTag,
                 "word session cursor rejected: scope generation %u != committed %u",
                 static_cast<unsigned>(session.deck_scope_generation),
                 static_cast<unsigned>(wqn::GetDeckScopeGeneration()));
        return ESP_ERR_INVALID_STATE;
    }
    SessionCursorRecord record = {};
    record.magic = kSessionCursorMagic;
    record.version = kSessionCursorVersion;
    record.paused = session.paused ? 1 : 0;
    std::snprintf(
        record.session_id,
        sizeof(record.session_id),
        "%s",
        session.remote.session_id.c_str());
    record.crc = Crc32(&record, sizeof(record) - sizeof(record.crc));
    // [word-session-cursor-nvs] One NVS blob instead of an AtomicWrite on
    // SPIFFS. NVS uses its own entry/GC recovery protocol, not an AtomicWrite
    // backup rotation. Do not claim stronger power-loss semantics without HIL.
    // Keep the CRC for size/schema/corruption checks across firmware versions.
    return wqn::SaveWordSessionCursorNvs(nvs_key, &record, sizeof(record));
}

bool ReadSessionCursorPaused(
    wqn::protocol::word_study_v1::Mode mode,
    const std::string& session_id,
    bool* paused)
{
    SessionPaths paths = {};
    if (paused == nullptr || !GetSessionCursorPaths(mode, &paths)) return false;
    const char* nvs_key = GetSessionCursorNvsKey(mode);
    if (nvs_key == nullptr) return false;

    // [word-session-cursor-nvs] NVS first. The legacy .cur file is still read
    // because devices that ran the SPIFFS cursor have one on disk and nothing
    // else: without this fallback the first cold boot after this change would
    // resume every paused session as unpaused. That is a one-time migration,
    // not a permanent second source -- the writer no longer touches SPIFFS, so
    // after the first successful read NVS is the only copy.
    SessionCursorRecord record = {};
    bool have_nvs_cursor = false;
    if (wqn::LoadWordSessionCursorNvs(nvs_key, &record, sizeof(record), &have_nvs_cursor) !=
        ESP_OK) {
        return false;
    }
    if (!have_nvs_cursor) {
        FILE* file = std::fopen(paths.primary, "rb");
        if (file == nullptr) return false;
        const bool read_ok =
            std::fread(&record, 1, sizeof(record), file) == sizeof(record);
        const int trailing = std::fgetc(file);
        const bool file_ok = read_ok && trailing == EOF && !std::ferror(file);
        std::fclose(file);
        if (!file_ok) return false;
    }
    if (record.magic != kSessionCursorMagic ||
        record.version != kSessionCursorVersion ||
        record.paused > 1 ||
        record.session_id[sizeof(record.session_id) - 1] != '\0' ||
        Crc32(&record, sizeof(record) - sizeof(record.crc)) != record.crc) {
        return false;
    }
    // Reject a leftover cursor from a previous session in the same mode slot.
    if (session_id != record.session_id) return false;
    // [word-session-cursor-nvs] Validate ALL fields and the session binding
    // before migration. Otherwise a corrupt/old legacy record gets installed
    // as NVS's preferred source even though this load subsequently rejects it.
    // A failed migration may still use the valid legacy value in memory; it
    // leaves the file intact for a later retry, not a claimed durable upgrade.
    if (!have_nvs_cursor &&
        wqn::SaveWordSessionCursorNvs(nvs_key, &record, sizeof(record)) != ESP_OK) {
        ESP_LOGW(kTag, "word session cursor migration to NVS deferred");
    }
    *paused = record.paused != 0;
    return true;
}

esp_err_t LoadSessionFile(const char* path, wqn::PersistedWordSession* session)
{
    if (session == nullptr) return ESP_ERR_INVALID_ARG;
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    SessionHeader header = {};
    const bool header_ok = std::fread(&header, 1, sizeof(header), file) == sizeof(header);
    if (!header_ok || header.magic != kSessionMagic ||
        header.version != kSessionSchemaVersion ||
        header.payload_size > kMaxSessionPayloadBytes) {
        std::fclose(file);
        return ESP_ERR_INVALID_VERSION;
    }
    std::vector<uint8_t> payload(header.payload_size);
    const bool payload_ok = payload.empty() ||
        std::fread(payload.data(), 1, payload.size(), file) == payload.size();
    const int trailing = std::fgetc(file);
    std::fclose(file);
    if (!payload_ok || trailing != EOF || Crc32(payload.data(), payload.size()) != header.payload_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    if (!DecodeSession(payload, session)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    // [deck-scope] Second line of defense behind the marker protocol: a
    // snapshot stamped under an older scope generation must not resume after
    // a deck switch (e.g. a switch replayed by boot recovery while this file
    // survived a torn clear). NOT_FOUND matches the "unused mode" contract.
    if (session->deck_scope_generation != wqn::GetDeckScopeGeneration()) {
        ESP_LOGW(kTag,
                 "word session rejected: scope generation %u != committed %u",
                 static_cast<unsigned>(session->deck_scope_generation),
                 static_cast<unsigned>(wqn::GetDeckScopeGeneration()));
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

// [load-repair] Writes: when the primary session file is torn (a power cut
// mid-commit) and the backup is intact, this promotes the backup back to primary
// so the next read is served by one file. That is the healing of the read it
// just performed, and it has to run on the storage task that owns the file,
// which is why it lives inside the load instead of in a separate repair pass.
esp_err_t LoadSessionSlotRaw(
    wqn::protocol::word_study_v1::Mode mode,
    wqn::PersistedWordSession* session)
{
    SessionPaths paths = {};
    if (session == nullptr || !GetSessionPaths(mode, &paths)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = LoadSessionFile(paths.primary, session);
    if (result == ESP_OK) {
        return session->remote.mode == mode ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    if (!FileExists(paths.backup)) return result;
    const esp_err_t backup_result = LoadSessionFile(paths.backup, session);
    if (backup_result == ESP_OK) {
        if (session->remote.mode != mode) return ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(kTag, "recovered word session from backup");
        return SaveSessionRaw(*session, true);
    }
    return result;
}

esp_err_t LoadSessionRaw(
    wqn::protocol::word_study_v1::Mode mode,
    wqn::PersistedWordSession* session)
{
    return LoadSessionSlotRaw(mode, session);
}

uint32_t RecordCrc(const OutboxRecord& record)
{
    return Crc32(&record, offsetof(OutboxRecord, crc));
}

bool CopyField(char* output, size_t output_size, const std::string& value)
{
    if (value.empty()) {
        return false;
    }
    return CopyFixedText(output, output_size, value);
}

wqn::DurableWordObservation ObservationFromRecord(const OutboxRecord& record)
{
    wqn::DurableWordObservation observation;
    observation.request_id = record.request_id;
    observation.session_id = record.session_id;
    observation.sequence = record.sequence;
    observation.item_id = record.item_id;
    observation.action = static_cast<wqn::protocol::word_study_v1::ObservationAction>(record.action);
    observation.mode = static_cast<wqn::protocol::word_study_v1::Mode>(record.mode);
    observation.occurred_at = record.occurred_at;
    observation.next_position = record.next_position;
    observation.next_phase = static_cast<wqn::WordPresentationPhase>(record.next_phase);
    return observation;
}

bool SameObservation(
    const wqn::DurableWordObservation& left,
    const wqn::DurableWordObservation& right)
{
    return left.request_id == right.request_id &&
        left.session_id == right.session_id && left.sequence == right.sequence &&
        left.item_id == right.item_id && left.action == right.action &&
        left.mode == right.mode && left.occurred_at == right.occurred_at &&
        left.next_position == right.next_position && left.next_phase == right.next_phase;
}

esp_err_t BuildObservationRecord(
    const wqn::DurableWordObservation& observation,
    OutboxRecordKind kind,
    OutboxRecord* record)
{
    using wqn::protocol::word_study_v1::Mode;
    using wqn::protocol::word_study_v1::ObservationAction;
    if (record == nullptr || observation.sequence > wqn::protocol::v3::kMaxSafeJsonInteger ||
        static_cast<uint8_t>(observation.action) > static_cast<uint8_t>(ObservationAction::kLookedUp) ||
        static_cast<uint8_t>(observation.mode) > static_cast<uint8_t>(Mode::kMistakes) ||
        static_cast<uint8_t>(observation.next_phase) > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    *record = {};
    record->magic = kOutboxMagic;
    record->version = kOutboxSchemaVersion;
    record->kind = static_cast<uint8_t>(kind);
    record->action = static_cast<uint8_t>(observation.action);
    record->mode = static_cast<uint8_t>(observation.mode);
    record->next_phase = static_cast<uint8_t>(observation.next_phase);
    record->sequence = observation.sequence;
    record->next_position = observation.next_position;
    if (!CopyField(record->request_id, sizeof(record->request_id), observation.request_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (kind == OutboxRecordKind::kObservation &&
        (!CopyField(record->session_id, sizeof(record->session_id), observation.session_id) ||
         !CopyField(record->item_id, sizeof(record->item_id), observation.item_id) ||
         !CopyField(record->occurred_at, sizeof(record->occurred_at), observation.occurred_at))) {
        return ESP_ERR_INVALID_ARG;
    }
    record->crc = RecordCrc(*record);
    return ESP_OK;
}

// Encodes a park marker for `request_id`. Only the identity and the reason
// are stored; the paired kObservation record keeps the full payload.
esp_err_t BuildSuspendRecord(
    const std::string& request_id,
    wqn::OutboxSuspendReason reason,
    OutboxRecord* record)
{
    if (record == nullptr || request_id.empty() ||
        request_id.size() > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    *record = {};
    record->magic = kOutboxMagic;
    record->version = kOutboxSchemaVersion;
    record->kind = static_cast<uint8_t>(OutboxRecordKind::kSuspend);
    record->reserved =
        static_cast<uint16_t>(static_cast<uint8_t>(reason));
    if (!CopyField(record->request_id, sizeof(record->request_id), request_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    record->crc = RecordCrc(*record);
    return ESP_OK;
}

esp_err_t ScanOutboxFile(const char* path, OutboxScan* scan)
{
    if (path == nullptr || scan == nullptr) return ESP_ERR_INVALID_ARG;
    *scan = {};
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    while (true) {
        OutboxRecord record = {};
        const size_t read = std::fread(&record, 1, sizeof(record), file);
        if (std::ferror(file)) {
            std::fclose(file);
            return ESP_FAIL;
        }
        if (read == 0 && std::feof(file)) break;
        if (read != sizeof(record)) {
            scan->partial_tail = true;
            break;
        }
        if (record.magic != kOutboxMagic ||
            record.version != kOutboxSchemaVersion ||
            record.crc != RecordCrc(record) || record.request_id[64] != '\0') {
            std::fclose(file);
            return ESP_ERR_INVALID_CRC;
        }
        ++scan->total_records;
        const std::string request_id(record.request_id);
        if (record.kind == static_cast<uint8_t>(OutboxRecordKind::kObservation)) {
            if (record.session_id[36] != '\0' || record.item_id[36] != '\0' ||
                record.occurred_at[32] != '\0' || record.next_phase > 1 ||
                !ValidMode(record.mode) ||
                record.action > static_cast<uint8_t>(
                    wqn::protocol::word_study_v1::ObservationAction::kLookedUp)) {
                std::fclose(file);
                return ESP_ERR_INVALID_RESPONSE;
            }
            const auto existing = std::find_if(
                scan->pending.begin(), scan->pending.end(),
                [&](const auto& value) { return request_id == value.request_id; });
            if (existing == scan->pending.end()) {
                scan->pending.push_back(record);
            } else if (std::memcmp(&*existing, &record, sizeof(record)) != 0) {
                std::fclose(file);
                return ESP_ERR_INVALID_STATE;
            }
        } else if (record.kind == static_cast<uint8_t>(OutboxRecordKind::kAck)) {
            ++scan->ack_records;
            const auto pending = std::find_if(
                scan->pending.begin(),
                scan->pending.end(),
                [&](const auto& value) { return request_id == value.request_id; });
            const auto acknowledged = std::find_if(
                scan->acknowledged.begin(),
                scan->acknowledged.end(),
                [&](const auto& value) { return request_id == value.request_id; });
            if (pending != scan->pending.end()) {
                if (acknowledged == scan->acknowledged.end()) {
                    scan->acknowledged.push_back(*pending);
                }
                scan->pending.erase(pending);
            } else if (acknowledged == scan->acknowledged.end()) {
                // ACKs are idempotent tombstones. A crash between journal
                // replacement steps can leave a valid ACK whose observation
                // was already removed by an earlier compaction. There is no
                // state left to reconcile, so retain the diagnostic and
                // ignore it instead of treating the complete journal as
                // corrupt and entering a repair loop.
                ++scan->orphan_ack_records;
                ESP_LOGW(
                    kTag,
                    "ignoring orphan word outbox ACK: request=%s",
                    request_id.c_str());
            }
        } else if (record.kind == static_cast<uint8_t>(OutboxRecordKind::kSuspend)) {
            // Suspend markers park their paired observation without
            // deleting it. Like ACKs they are idempotent: an orphaned
            // marker (observation already compacted away) is retained as a
            // diagnostic rather than poisoning the whole journal.
            ++scan->suspend_records;
            const auto pending = std::find_if(
                scan->pending.begin(),
                scan->pending.end(),
                [&](const auto& value) { return request_id == value.request_id; });
            const auto suspended = std::find_if(
                scan->suspended.begin(),
                scan->suspended.end(),
                [&](const auto& value) { return request_id == value.request_id; });
            if (pending != scan->pending.end()) {
                if (suspended == scan->suspended.end()) {
                    scan->suspended.push_back(*pending);
                    scan->suspended_reasons.push_back(record.reserved);
                }
                scan->pending.erase(pending);
            } else if (suspended == scan->suspended.end()) {
                ++scan->orphan_suspend_records;
                ESP_LOGW(
                    kTag,
                    "ignoring orphan word outbox suspend marker: request=%s",
                    request_id.c_str());
            }
        } else {
            std::fclose(file);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (std::fclose(file) != 0) return ESP_FAIL;
    return scan->pending.size() + scan->suspended.size() <=
            wqn::kWordObservationOutboxCapacity
        ? ESP_OK
        : ESP_ERR_INVALID_SIZE;
}

esp_err_t ScanOutbox(OutboxScan* scan)
{
    if (scan == nullptr) return ESP_ERR_INVALID_ARG;
    esp_err_t primary_result = ScanOutboxFile(kOutboxPath, scan);
    if (primary_result == ESP_OK) return ESP_OK;
    if (!FileExists(kOutboxBackupPath)) {
        if (primary_result == ESP_ERR_NOT_FOUND) {
            *scan = {};
            return ESP_OK;
        }
        return primary_result;
    }

    OutboxScan backup;
    const esp_err_t backup_result = ScanOutboxFile(kOutboxBackupPath, &backup);
    if (backup_result != ESP_OK) {
        return primary_result == ESP_ERR_NOT_FOUND ? backup_result : primary_result;
    }
    backup.backup_source = true;
    *scan = std::move(backup);
    ESP_LOGW(
        kTag,
        "recovered word outbox from backup: primary_error=%s pending=%u",
        esp_err_to_name(primary_result),
        static_cast<unsigned>(scan->pending.size()));
    return ESP_OK;
}

esp_err_t EnsureOutboxCache(OutboxScan** scan)
{
    if (scan == nullptr) return ESP_ERR_INVALID_ARG;
    if (!g_outbox_cache_loaded) {
        OutboxScan loaded;
        ESP_RETURN_ON_ERROR(
            ScanOutbox(&loaded), kTag, "load word outbox cache");
        g_outbox_cache = std::move(loaded);
        g_outbox_cache_loaded = true;
        ESP_LOGI(
            kTag,
            "word outbox cache loaded: total=%u pending=%u ack=%u orphan_ack=%u",
            static_cast<unsigned>(g_outbox_cache.total_records),
            static_cast<unsigned>(g_outbox_cache.pending.size()),
            static_cast<unsigned>(g_outbox_cache.ack_records),
            static_cast<unsigned>(g_outbox_cache.orphan_ack_records));
    }
    *scan = &g_outbox_cache;
    return ESP_OK;
}

// Preserve the existing 1000 live-observation capacity. A parked observation
// needs two records; retain the EXISTING 64-record maintenance slack, independent
// of maintenance frequency. Reserve one terminal slot for EVERY pending event.
constexpr size_t kOutboxMaxRecords =
    2 * wqn::kWordObservationOutboxCapacity + 64;
constexpr size_t kOutboxMaxBytes = kOutboxMaxRecords * sizeof(OutboxRecord);

esp_err_t CheckOutboxAppendBudget(const OutboxScan& scan, bool observation)
{
    // Subtraction form avoids overflow on a legacy/corrupt counter. Terminal
    // records consume a reserved slot, so total + pending stays unchanged.
    if (scan.total_records > kOutboxMaxRecords ||
        scan.pending.size() > kOutboxMaxRecords - scan.total_records ||
        (observation && kOutboxMaxRecords - scan.total_records - scan.pending.size() < 2) ||
        (!observation && (scan.pending.empty() || scan.total_records == kOutboxMaxRecords))) {
        ESP_LOGW(kTag, "word outbox quota reached: records=%u pending=%u max_bytes=%u",
                 static_cast<unsigned>(scan.total_records),
                 static_cast<unsigned>(scan.pending.size()),
                 static_cast<unsigned>(kOutboxMaxBytes));
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

// [word-batch] One open/write/flush/sync/close for a bounded contiguous batch.
// Every record keeps the existing v1 CRC and identity. A failed call may leave
// a complete prefix plus a torn tail: callers must rescan before any retry.
constexpr size_t kOutboxAppendBatchCapacity = 10;

esp_err_t AppendOutboxRecordsTo(
    const char* path,
    const OutboxRecord* records,
    size_t count,
    size_t* bytes = nullptr,
    size_t max_bytes = 0)
{
    if (bytes != nullptr) *bytes = 0;
    if (path == nullptr || records == nullptr || count == 0 ||
        count > kOutboxAppendBatchCapacity) return ESP_ERR_INVALID_ARG;
    const size_t requested_bytes = count * sizeof(OutboxRecord);
    FILE* file = std::fopen(path, "ab");
    if (file == nullptr) return ESP_FAIL;
    if (max_bytes != 0) {
        struct stat info = {};
        if (::fstat(fileno(file), &info) != 0 || info.st_size < 0) {
            std::fclose(file);
            return ESP_FAIL;
        }
        if (max_bytes < requested_bytes ||
            static_cast<uint64_t>(info.st_size) > max_bytes - requested_bytes ||
            static_cast<uint64_t>(info.st_size) % sizeof(OutboxRecord) != 0) {
            ESP_LOGW(kTag, "word outbox physical bound: bytes=%llu max_bytes=%u",
                     static_cast<unsigned long long>(info.st_size),
                     static_cast<unsigned>(max_bytes));
            return std::fclose(file) == 0 ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
        }
    }
    const size_t written_bytes = std::fwrite(records, 1, requested_bytes, file);
    const bool written = written_bytes == requested_bytes;
    const bool durable = written && std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (bytes != nullptr) *bytes = written_bytes;
    return durable && closed ? ESP_OK : ESP_FAIL;
}

esp_err_t AppendOutboxRecordTo(
    const char* path,
    const OutboxRecord& record,
    size_t* bytes = nullptr,
    size_t max_bytes = 0)
{
    return AppendOutboxRecordsTo(path, &record, 1, bytes, max_bytes);
}

// The rejected journal is forensic data, not another upload queue. Keep it as
// a bounded rolling file so a long run of terminal server errors cannot make
// quarantine itself the reason the durable upload head stops progressing.
esp_err_t AppendRejectedOutboxRecord(const OutboxRecord& record)
{
    size_t complete_records = 0;
    bool discarded_partial_tail = false;
    struct stat existing = {};
    if (stat(kRejectedOutboxPath, &existing) == 0 &&
        S_ISREG(existing.st_mode) && existing.st_size >= 0) {
        const size_t bytes = static_cast<size_t>(existing.st_size);
        complete_records = bytes / sizeof(OutboxRecord);
        discarded_partial_tail = bytes % sizeof(OutboxRecord) != 0;
    }
    const size_t keep_existing = std::min(
        complete_records, kRejectedOutboxCapacity - static_cast<size_t>(1));
    const size_t skip_records = complete_records - keep_existing;

    FILE* source = nullptr;
    if (keep_existing > 0) {
        source = std::fopen(kRejectedOutboxPath, "rb");
        if (source == nullptr ||
            std::fseek(
                source,
                static_cast<long>(skip_records * sizeof(OutboxRecord)),
                SEEK_SET) != 0) {
            if (source != nullptr) std::fclose(source);
            return ESP_FAIL;
        }
    }
    FILE* output = std::fopen(kRejectedOutboxTempPath, "wb");
    if (output == nullptr) {
        if (source != nullptr) std::fclose(source);
        return ESP_FAIL;
    }
    bool ok = true;
    OutboxRecord copied = {};
    for (size_t i = 0; ok && i < keep_existing; ++i) {
        ok = std::fread(&copied, 1, sizeof(copied), source) == sizeof(copied) &&
             std::fwrite(&copied, 1, sizeof(copied), output) == sizeof(copied);
    }
    if (source != nullptr) std::fclose(source);
    ok = ok && std::fwrite(&record, 1, sizeof(record), output) == sizeof(record);
    const bool durable =
        ok && std::fflush(output) == 0 && ::fsync(fileno(output)) == 0;
    const bool closed = std::fclose(output) == 0;
    if (!durable || !closed) {
        std::remove(kRejectedOutboxTempPath);
        return ESP_FAIL;
    }

    const bool had_existing = FileExists(kRejectedOutboxPath);
    if (had_existing) {
        std::remove(kRejectedOutboxBackupPath);
        if (std::rename(kRejectedOutboxPath, kRejectedOutboxBackupPath) != 0) {
            std::remove(kRejectedOutboxTempPath);
            return ESP_FAIL;
        }
    }
    if (std::rename(kRejectedOutboxTempPath, kRejectedOutboxPath) != 0) {
        if (had_existing) std::rename(kRejectedOutboxBackupPath, kRejectedOutboxPath);
        std::remove(kRejectedOutboxTempPath);
        return ESP_FAIL;
    }
    if (had_existing) std::remove(kRejectedOutboxBackupPath);
    if (skip_records > 0 || discarded_partial_tail) {
        ESP_LOGW(
            kTag,
            "word observation quarantine rolled over: discarded=%u partial_tail=%d capacity=%u",
            static_cast<unsigned>(skip_records),
            discarded_partial_tail ? 1 : 0,
            static_cast<unsigned>(kRejectedOutboxCapacity));
    }
    return ESP_OK;
}

esp_err_t AppendOutboxRecords(
    const OutboxRecord* records, size_t count,
    size_t* bytes = nullptr)
{
    const esp_err_t result = AppendOutboxRecordsTo(
        kOutboxPath, records, count, bytes, kOutboxMaxBytes);
    // A failed sync/close can still have written a whole record or a bad tail.
    // Never use pre-write counters for the next budget or idempotence check.
    if (result != ESP_OK) g_outbox_cache_loaded = false;
    return result;
}

esp_err_t AppendOutboxRecord(
    const OutboxRecord& record, size_t* bytes = nullptr)
{
    const esp_err_t result = AppendOutboxRecordTo(
        kOutboxPath, record, bytes, kOutboxMaxBytes);
    if (result != ESP_OK) g_outbox_cache_loaded = false;
    return result;
}

esp_err_t CompactOutbox(
    const std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>>& pending,
    bool preserve_backup = false)
{
    if (pending.size() > kOutboxMaxRecords) return ESP_ERR_INVALID_SIZE;
    FILE* file = std::fopen(kOutboxTempPath, "wb");
    if (file == nullptr) return ESP_FAIL;
    bool ok = true;
    for (const OutboxRecord& record : pending) {
        if (std::fwrite(&record, 1, sizeof(record), file) != sizeof(record)) {
            ok = false;
            break;
        }
    }
    const bool durable = ok && std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!durable || !closed) {
        std::remove(kOutboxTempPath);
        return ESP_FAIL;
    }
    const bool had_primary = FileExists(kOutboxPath);
    if (preserve_backup) {
        // The scan came from the last known-good backup. Never replace that
        // backup with the corrupt primary during recovery.
        if (had_primary && std::remove(kOutboxPath) != 0 && errno != ENOENT) {
            std::remove(kOutboxTempPath);
            return ESP_FAIL;
        }
        if (std::rename(kOutboxTempPath, kOutboxPath) != 0) {
            std::remove(kOutboxTempPath);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (had_primary) {
        if (std::remove(kOutboxBackupPath) != 0 && errno != ENOENT) {
            std::remove(kOutboxTempPath);
            return ESP_FAIL;
        }
        if (std::rename(kOutboxPath, kOutboxBackupPath) != 0) {
            std::remove(kOutboxTempPath);
            return ESP_FAIL;
        }
    }
    if (std::rename(kOutboxTempPath, kOutboxPath) != 0) {
        if (had_primary) std::rename(kOutboxBackupPath, kOutboxPath);
        std::remove(kOutboxTempPath);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t CompactCachedOutbox(OutboxScan* scan)
{
    if (scan == nullptr) return ESP_ERR_INVALID_ARG;
    // Suspended records are part of the durable rewrite set: compaction
    // replaces the whole journal, so dropping them here would silently
    // destroy parked payloads that still await intervention.
    std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>>
        rewrite;
    if (scan->suspended.size() != scan->suspended_reasons.size()) {
        return ESP_ERR_INVALID_STATE;
    }
    rewrite.reserve(scan->pending.size() + 2 * scan->suspended.size());
    rewrite.insert(
        rewrite.end(), scan->pending.begin(), scan->pending.end());
    for (size_t i = 0; i < scan->suspended.size(); ++i) {
        rewrite.push_back(scan->suspended[i]);
        OutboxRecord marker = {};
        ESP_RETURN_ON_ERROR(
            BuildSuspendRecord(
                scan->suspended[i].request_id,
                static_cast<wqn::OutboxSuspendReason>(
                    static_cast<uint8_t>(scan->suspended_reasons[i])),
                &marker),
            kTag,
            "rebuild word suspend marker");
        rewrite.push_back(marker);
    }
    const esp_err_t compacted = CompactOutbox(
        rewrite, scan->backup_source || !scan->suspended.empty());
    if (compacted != ESP_OK) {
        g_outbox_cache_loaded = false;
        return compacted;
    }
    scan->acknowledged.clear();
    scan->total_records = rewrite.size();
    scan->ack_records = 0;
    scan->suspend_records = scan->suspended.size();
    scan->orphan_ack_records = 0;
    scan->orphan_suspend_records = 0;
    scan->partial_tail = false;
    scan->backup_source = false;
    return ESP_OK;
}

bool SetSessionCursorOrdinal(
    wqn::PersistedWordSession* session,
    uint32_t ordinal)
{
    if (session == nullptr) return false;
    const auto& items = session->remote.items;
    const auto match = std::find_if(
        items.begin(), items.end(),
        [&](const auto& item) { return item.ordinal == ordinal; });
    if (match != items.end()) {
        session->position = static_cast<uint32_t>(match - items.begin());
        return true;
    }
    if (items.empty()) {
        if (ordinal != 0) return false;
        session->position = 0;
        return true;
    }
    if (ordinal == items.back().ordinal + 1) {
        session->position = static_cast<uint32_t>(items.size());
        return true;
    }
    return false;
}

bool SessionCursorOrdinal(
    const wqn::PersistedWordSession& session,
    uint32_t* ordinal)
{
    if (ordinal == nullptr) return false;
    if (session.position < session.remote.items.size()) {
        const uint64_t value = session.remote.items[session.position].ordinal;
        if (value > UINT32_MAX) return false;
        *ordinal = static_cast<uint32_t>(value);
        return true;
    }
    if (session.position == session.remote.items.size()) {
        const uint64_t value = session.remote.items.empty()
            ? 0
            : session.remote.items.back().ordinal + 1;
        if (value > UINT32_MAX) return false;
        *ordinal = static_cast<uint32_t>(value);
        return true;
    }
    return false;
}

void ReconcileSession(
    const std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>>& records,
    wqn::PersistedWordSession* session,
    bool* changed)
{
    if (session == nullptr || changed == nullptr) return;
    for (const OutboxRecord& observation : records) {
        if (static_cast<uint8_t>(session->remote.mode) != observation.mode ||
            session->remote.session_id != observation.session_id ||
            observation.sequence < session->remote.next_sequence) {
            continue;
        }
        if (!SetSessionCursorOrdinal(session, observation.next_position)) {
            continue;
        }
        session->phase = static_cast<wqn::WordPresentationPhase>(observation.next_phase);
        session->remote.next_sequence = observation.sequence + 1;
        *changed = true;
    }
}

bool SessionCoversRecords(
    const std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>>& records,
    const wqn::PersistedWordSession& session)
{
    return std::none_of(records.begin(), records.end(), [&](const OutboxRecord& record) {
        return record.mode == static_cast<uint8_t>(session.remote.mode) &&
            session.remote.session_id == record.session_id &&
            record.sequence >= session.remote.next_sequence;
    });
}

esp_err_t CheckpointSessionFromOutbox(
    const OutboxScan& scan,
    wqn::protocol::word_study_v1::Mode mode)
{
    // All seven slots can be persisted, but avoid opening absent modes.
    const auto contains_mode = [mode](const auto& records) {
        return std::any_of(records.begin(), records.end(), [&](const OutboxRecord& record) {
            return record.mode == static_cast<uint8_t>(mode);
        });
    };
    if (!contains_mode(scan.acknowledged) && !contains_mode(scan.pending) &&
        !contains_mode(scan.suspended)) {
        return ESP_OK;
    }
    wqn::PersistedWordSession session;
    const esp_err_t load_result = LoadSessionRaw(mode, &session);
    if (load_result == ESP_ERR_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(load_result, kTag, "load session for outbox checkpoint");
    bool changed = false;
    ReconcileSession(scan.acknowledged, &session, &changed);
    ReconcileSession(scan.pending, &session, &changed);
    ReconcileSession(scan.suspended, &session, &changed);
    // A cursor missing from this candidate window is not a checkpoint proof.
    if (!SessionCoversRecords(scan.acknowledged, session)) {
        return ESP_ERR_INVALID_STATE;
    }
    return changed ? SaveSessionRaw(session) : ESP_OK;
}

esp_err_t CheckpointSessionsFromOutbox(const OutboxScan& scan)
{
    for (const auto mode : kPersistedSessionModes) {
        ESP_RETURN_ON_ERROR(
            CheckpointSessionFromOutbox(scan, mode), kTag,
            "checkpoint session before outbox compaction");
    }
    return ESP_OK;
}

esp_err_t MaybeCompactCachedOutbox(OutboxScan* scan)
{
    // Active suspend markers must survive every compaction, so they are not
    // reclaimable records and must not continuously retrigger maintenance.
    if (scan == nullptr || (scan->ack_records < kRuntimeCompactAckThreshold &&
                           CheckOutboxAppendBudget(*scan, true) == ESP_OK)) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        CheckpointSessionsFromOutbox(*scan),
        kTag,
        "checkpoint runtime word outbox");
    ESP_RETURN_ON_ERROR(
        CompactCachedOutbox(scan),
        kTag,
        "compact runtime word outbox");
    ESP_LOGI(
        kTag,
        "word outbox runtime compaction complete: pending=%u",
        static_cast<unsigned>(scan->pending.size()));
    return ESP_OK;
}

struct OutboxMaintenanceContext {
    wqn::WordOutboxMaintenanceGate maintenance_gate;
    // This caller-owned payload is touched only while its synchronous owner
    // transaction is running. No cache pointer escapes the owner between steps.
    std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>> acknowledged;
    size_t ack_records = 0;
    size_t next_mode = 0;
    uint64_t session_generation = 0;
    uint32_t scope_generation = 0;
    int64_t deadline_us = 0;
    bool for_sleep = false;
    bool force_compact = false;
    bool started = false;
    bool done = false;
    bool deferred = false;
    bool partial_tail = false;
    bool backup_source = false;
};

esp_err_t OutboxMaintenanceStepTransaction(void* opaque)
{
    auto* context = static_cast<OutboxMaintenanceContext*>(opaque);
    if (context == nullptr) return ESP_ERR_INVALID_ARG;
    if (context->done) return ESP_OK;
    if (context->deadline_us > 0 && esp_timer_get_time() >= context->deadline_us) {
        context->done = context->deferred = true;
        ESP_LOGI(kTag, "word outbox maintenance deferred: reason=deadline step=%u",
                 static_cast<unsigned>(context->next_mode));
        return ESP_OK;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load outbox maintenance step");
    if (!context->started) {
        if (!context->force_compact && (context->for_sleep
                ? scan->ack_records < kRuntimeCompactAckThreshold && !scan->partial_tail && !scan->backup_source &&
                      CheckOutboxAppendBudget(*scan, true) == ESP_OK
                : scan->ack_records < kRuntimeCompactAckThreshold &&
                      CheckOutboxAppendBudget(*scan, true) == ESP_OK)) {
            context->done = true;
            return ESP_OK;
        }
        context->acknowledged = scan->acknowledged;
        context->ack_records = scan->ack_records;
        context->session_generation = g_session_mutation_generation;
        context->scope_generation = wqn::GetDeckScopeGeneration();
        context->partial_tail = scan->partial_tail;
        context->backup_source = scan->backup_source;
        context->started = true;
    } else if (context->session_generation != g_session_mutation_generation ||
               context->scope_generation != wqn::GetDeckScopeGeneration() ||
               context->ack_records != scan->ack_records ||
               context->partial_tail != scan->partial_tail ||
               context->backup_source != scan->backup_source ||
               context->acknowledged.size() != scan->acknowledged.size() ||
               !std::equal(context->acknowledged.begin(), context->acknowledged.end(),
                           scan->acknowledged.begin(), [](const auto& left, const auto& right) {
                               return std::memcmp(&left, &right, sizeof(OutboxRecord)) == 0;
                           })) {
        // New ACKs, a changed slot/scope or another compaction invalidate the
        // proof. Keep the complete journal; the next call starts a fresh pass.
        context->done = context->deferred = true;
        ESP_LOGI(kTag, "word outbox maintenance deferred: reason=changed step=%u",
                 static_cast<unsigned>(context->next_mode));
        return ESP_OK;
    }
    if (!context->force_compact && !context->for_sleep &&
        !scan->partial_tail && !scan->backup_source &&
        CheckOutboxAppendBudget(*scan, true) == ESP_OK &&
        context->maintenance_gate.should_defer != nullptr &&
        context->maintenance_gate.should_defer(context->maintenance_gate.context)) {
        // [batch-maintenance] ACK durability precedes maintenance admission.
        // Check on the owner AFTER queue wait, before each checkpoint/reclaim;
        // new interaction must not start the remaining long VFS steps. The
        // retained journal still proves any progress already checkpointed.
        context->done = context->deferred = true;
        ESP_LOGI(kTag, "word outbox maintenance deferred: reason=interaction step=%u",
                 static_cast<unsigned>(context->next_mode));
        return ESP_OK;
    }
    constexpr size_t mode_count = sizeof(kPersistedSessionModes) / sizeof(kPersistedSessionModes[0]);
    if (context->next_mode < mode_count) {
        const auto mode = kPersistedSessionModes[context->next_mode];
        ESP_RETURN_ON_ERROR(
            CheckpointSessionFromOutbox(*scan, mode), kTag, "checkpoint outbox maintenance step");
        context->session_generation = g_session_mutation_generation;
        ++context->next_mode;
        ESP_LOGI(kTag, "word outbox maintenance checkpoint: mode=%u step=%u",
                 static_cast<unsigned>(mode), static_cast<unsigned>(context->next_mode));
        return ESP_OK;
    }
    // Foreground may have appended NEW pending/parked data between steps.
    // Rewrite the live cache, not a stale copy captured at the beginning.
    ESP_RETURN_ON_ERROR(CompactCachedOutbox(scan), kTag, "reclaim outbox maintenance step");
    context->done = true;
    ESP_LOGI(kTag, "word outbox maintenance complete: sleep=%u pending=%u suspended=%u",
             static_cast<unsigned>(context->for_sleep),
             static_cast<unsigned>(scan->pending.size()),
             static_cast<unsigned>(scan->suspended.size()));
    return ESP_OK;
}

struct LoadSessionContext {
    wqn::protocol::word_study_v1::Mode mode;
    wqn::PersistedWordSession* session;
};

// [load-repair] Ordinary replay derives the returned cursor in RAM from the durable journal;
// it need not rewrite the complete candidate snapshot. Keeping both on-disk
// inputs intact lets the next boot repeat the same reconciliation. This is not
// a promise of a write-free load: LoadSessionRaw can promote a backup, the
// paused-cursor reader can migrate legacy data, and a damaged journal still
// needs repair below. Before repair discards any acknowledged observations,
// checkpoint their progress, then compact, within the same owner transaction.
esp_err_t LoadSessionTransaction(void* opaque)
{
    auto* context = static_cast<LoadSessionContext*>(opaque);
    if (context == nullptr || context->session == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* session = context->session;
    const esp_err_t session_result = LoadSessionRaw(context->mode, session);
    if (session_result != ESP_OK) {
        // An unused mode has no file by design. Let the caller distinguish
        // NOT_FOUND without emitting an error-level log on every index load.
        return session_result;
    }
    const uint64_t snapshot_sequence = session->remote.next_sequence;
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load word outbox");
    bool changed = false;
    ReconcileSession(scan->acknowledged, session, &changed);
    ReconcileSession(scan->pending, session, &changed);
    ReconcileSession(scan->suspended, session, &changed);
    // Do not report an older card as successfully restored when newer durable
    // progress exists but cannot be represented in this candidate window.
    if (!SessionCoversRecords(scan->acknowledged, *session) ||
        !SessionCoversRecords(scan->pending, *session) ||
        !SessionCoversRecords(scan->suspended, *session)) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool needs_repair = scan->partial_tail || scan->backup_source;
    if (needs_repair) {
        // Preserve this already-loaded mode first; checkpointing the remaining
        // represented slots below then verifies ACK coverage before reclaim.
        // Ordinary replay above still stays in RAM.
        if (changed) {
            ESP_RETURN_ON_ERROR(
                SaveSessionRaw(*session), kTag, "checkpoint loaded mode before repair");
        }
        ESP_RETURN_ON_ERROR(
            CheckpointSessionsFromOutbox(*scan),
            kTag,
            "checkpoint before word outbox repair");
        ESP_RETURN_ON_ERROR(
            CompactCachedOutbox(scan),
            kTag,
            "repair word outbox tail");
    }
    if (changed) {
        ESP_LOGI(
            kTag,
            "word session replay: mode=%u next_sequence=%llu checkpoint_deferred=%u",
            static_cast<unsigned>(context->mode),
            static_cast<unsigned long long>(session->remote.next_sequence),
            static_cast<unsigned>(!needs_repair));
    }
    // Overlay the cheap cursor. Pause/resume persist only this flag, so the
    // snapshot on disk may still read unpaused; position/phase/sequence were
    // already reconciled from the outbox above.
    bool cursor_paused = false;
    if (ReadSessionCursorPaused(
            context->mode, session->remote.session_id, &cursor_paused)) {
        session->paused = cursor_paused;
    }
    ESP_LOGI(
        kTag,
        "word session loaded: mode=%u next_sequence=%llu snapshot_sequence=%llu "
        "scope=%u paused=%u session=%s",
        static_cast<unsigned>(context->mode),
        static_cast<unsigned long long>(session->remote.next_sequence),
        static_cast<unsigned long long>(snapshot_sequence),
        static_cast<unsigned>(session->deck_scope_generation),
        static_cast<unsigned>(session->paused), session->remote.session_id.c_str());
    return ESP_OK;
}

bool SameEncodedSessionSnapshot(const wqn::PersistedWordSession& left,
                               const wqn::PersistedWordSession& right)
{
    // Compare the complete existing codec, not just SID/sequence or a hash.
    // A new candidate window, pinned revision or paused flag is a real change.
    std::vector<uint8_t> left_bytes, right_bytes;
    return EncodeSession(left, &left_bytes) && EncodeSession(right, &right_bytes) &&
        left_bytes == right_bytes;
}

esp_err_t SaveSessionProgressProtected(
    const wqn::PersistedWordSession& incoming,
    bool refresh_cursor)
{
    // The runner may have captured this candidate-page snapshot before newer
    // observations were committed and checkpointed. A late save or old retry
    // must not overwrite their only durable progress after ACK reclamation.
    if (incoming.deck_scope_generation != wqn::GetDeckScopeGeneration()) {
        return ESP_ERR_INVALID_STATE;
    }
    wqn::PersistedWordSession session = incoming;
    wqn::PersistedWordSession current;
    const esp_err_t loaded = LoadSessionRaw(session.remote.mode, &current);
    if (loaded != ESP_OK && loaded != ESP_ERR_NOT_FOUND) return loaded;
    if (loaded == ESP_OK && current.remote.session_id == session.remote.session_id &&
        current.remote.next_sequence > session.remote.next_sequence) {
        uint32_t ordinal = 0;
        if (!SessionCursorOrdinal(current, &ordinal) ||
            !SetSessionCursorOrdinal(&session, ordinal)) {
            return ESP_ERR_INVALID_STATE;
        }
        session.remote.next_sequence = current.remote.next_sequence;
        session.phase = current.phase;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load journal before word snapshot save");
    bool changed = false;
    ReconcileSession(scan->acknowledged, &session, &changed);
    ReconcileSession(scan->pending, &session, &changed);
    ReconcileSession(scan->suspended, &session, &changed);
    if (!SessionCoversRecords(scan->acknowledged, session) ||
        !SessionCoversRecords(scan->pending, session) ||
        !SessionCoversRecords(scan->suspended, session)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (loaded == ESP_OK && SameEncodedSessionSnapshot(session, current)) {
        ESP_LOGI(kTag, "word snapshot coalesced: mode=%u sequence=%llu session=%s",
            static_cast<unsigned>(session.remote.mode),
            static_cast<unsigned long long>(session.remote.next_sequence), session.remote.session_id.c_str());
    } else {
        ESP_RETURN_ON_ERROR(SaveSessionRaw(session), kTag, "save word session snapshot");
    }
    if (session.remote.next_sequence > incoming.remote.next_sequence) {
        ESP_LOGI(
            kTag,
            "word session progress merge: mode=%u incoming_sequence=%llu saved_sequence=%llu session=%s",
            static_cast<unsigned>(session.remote.mode),
            static_cast<unsigned long long>(incoming.remote.next_sequence),
            static_cast<unsigned long long>(session.remote.next_sequence),
            session.remote.session_id.c_str());
    }
    // Refresh the cursor so it always agrees with the snapshot's paused flag,
    // preventing a stale cursor from a prior session in this slot.
    return refresh_cursor ? WriteSessionCursor(session) : ESP_OK;
}

esp_err_t SaveSessionTransaction(void* context)
{
    if (context == nullptr) return ESP_ERR_INVALID_ARG;
    return SaveSessionProgressProtected(
        *static_cast<const wqn::PersistedWordSession*>(context), true);
}

esp_err_t SaveCursorTransaction(void* context)
{
    return WriteSessionCursor(
        *static_cast<const wqn::PersistedWordSession*>(context));
}

struct ClearSessionContext {
    wqn::protocol::word_study_v1::Mode mode;
};

esp_err_t ClearSessionTransaction(void* opaque)
{
    auto* context = static_cast<ClearSessionContext*>(opaque);
    SessionPaths paths = {};
    if (context == nullptr || !GetSessionPaths(context->mode, &paths)) {
        return ESP_ERR_INVALID_ARG;
    }
    ++g_session_mutation_generation;
    if (std::remove(paths.primary) != 0 && errno != ENOENT) return ESP_FAIL;
    if (std::remove(paths.temporary) != 0 && errno != ENOENT) return ESP_FAIL;
    if (std::remove(paths.backup) != 0 && errno != ENOENT) return ESP_FAIL;
    SessionPaths cursor_paths = {};
    if (GetSessionCursorPaths(context->mode, &cursor_paths)) {
        // Legacy cursors remain a read fallback during migration. A failed
        // unlink is not "already absent" and must not be reported as a fully
        // successful clear, nor proceed to erasing the NVS cursor.
        for (const char* path : {cursor_paths.primary, cursor_paths.temporary,
                                 cursor_paths.backup}) {
            if (std::remove(path) != 0 && errno != ENOENT) return ESP_FAIL;
        }
    }
    // [word-session-cursor-nvs] The cursor is an NVS blob now, so removing the
    // three legacy files above is no longer what clears it. Erase the key too,
    // or every cleared session retains 4 of NVS's 504 entries (52 B blob:
    // one chunk header + two data entries + one BLOB_IDX). The
    // legacy removals stay for the one-time migration window.
    const char* cursor_key = GetSessionCursorNvsKey(context->mode);
    if (cursor_key != nullptr) {
        const esp_err_t cleared = wqn::ClearWordSessionCursorNvs(cursor_key);
        if (cleared != ESP_OK) {
            ESP_LOGW(
                kTag, "word session cursor erase failed: %s", esp_err_to_name(cleared));
            return cleared;
        }
    }
    return ESP_OK;
}

struct CommitContext {
    const wqn::DurableWordObservation* observation;
    const wqn::PersistedWordSession* advanced_session;
};

esp_err_t CommitObservationTransaction(void* opaque)
{
    auto* context = static_cast<CommitContext*>(opaque);
    if (context == nullptr || context->observation == nullptr ||
        context->advanced_session == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto& observation = *context->observation;
    const auto& session = *context->advanced_session;
    uint32_t session_ordinal = 0;
    if (session.remote.session_id != observation.session_id ||
        session.remote.mode != observation.mode ||
        session.remote.next_sequence != observation.sequence + 1 ||
        !SessionCursorOrdinal(session, &session_ordinal) ||
        session_ordinal != observation.next_position ||
        session.phase != observation.next_phase) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session.deck_scope_generation != wqn::GetDeckScopeGeneration()) {
        return ESP_ERR_INVALID_STATE;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load outbox before word observation");
    const auto existing = std::find_if(
        scan->pending.begin(), scan->pending.end(),
        [&](const auto& value) { return observation.request_id == value.request_id; });
    if (existing != scan->pending.end()) {
        if (!SameObservation(ObservationFromRecord(*existing), observation)) {
            return ESP_ERR_INVALID_STATE;
        }
        return SaveSessionProgressProtected(session, false);
    }
    const auto acknowledged = std::find_if(
        scan->acknowledged.begin(), scan->acknowledged.end(),
        [&](const auto& value) { return observation.request_id == value.request_id; });
    if (acknowledged != scan->acknowledged.end()) {
        if (!SameObservation(ObservationFromRecord(*acknowledged), observation)) {
            return ESP_ERR_INVALID_STATE;
        }
        return SaveSessionProgressProtected(session, false);
    }
    const auto suspended = std::find_if(
        scan->suspended.begin(), scan->suspended.end(),
        [&](const auto& value) { return observation.request_id == value.request_id; });
    if (suspended != scan->suspended.end()) {
        return SameObservation(ObservationFromRecord(*suspended), observation)
            ? SaveSessionProgressProtected(session, false)
            : ESP_ERR_INVALID_STATE;
    }
    if (scan->pending.size() + scan->suspended.size() >=
        wqn::kWordObservationOutboxCapacity) {
        return ESP_ERR_NO_MEM;
    }
    if (scan->partial_tail || scan->backup_source) {
        ESP_RETURN_ON_ERROR(
            CheckpointSessionsFromOutbox(*scan),
            kTag,
            "checkpoint before append repair");
        ESP_RETURN_ON_ERROR(
            CompactCachedOutbox(scan),
            kTag,
            "repair before append");
    }
    OutboxRecord record = {};
    ESP_RETURN_ON_ERROR(
        BuildObservationRecord(observation, OutboxRecordKind::kObservation, &record),
        kTag,
        "encode word observation");
    ESP_RETURN_ON_ERROR(CheckOutboxAppendBudget(*scan, true), kTag, "word observation quota");
    ESP_RETURN_ON_ERROR(
        AppendOutboxRecord(record), kTag,
        "append word observation");
    scan->pending.push_back(record);
    ++scan->total_records;
    ESP_LOGD(kTag, "word observation durable: sequence=%llu mode=%u session=%s",
        static_cast<unsigned long long>(observation.sequence),
        static_cast<unsigned>(observation.mode), observation.session_id.c_str());
    // The durable record includes next_position, next_phase, and sequence.
    // LoadSessionTransaction and sleep preparation reconcile an older session
    // file from these records. Rewriting and fsyncing the complete session
    // here added ~2 seconds to every card action without increasing power-loss
    // safety.
    return ESP_OK;
}

struct CommitBatchContext {
    const std::vector<wqn::DurableWordObservation>* observations;
    const wqn::PersistedWordSession* advanced_session;
    bool maintenance_required = false;
};

esp_err_t CommitObservationBatchTransaction(void* opaque)
{
    auto* context = static_cast<CommitBatchContext*>(opaque);
    if (context == nullptr || context->observations == nullptr ||
        context->advanced_session == nullptr || context->observations->empty() ||
        context->observations->size() > kOutboxAppendBatchCapacity) return ESP_ERR_INVALID_ARG;
    const auto& observations = *context->observations;
    const auto& session = *context->advanced_session;
    context->maintenance_required = false;
    uint32_t ordinal = 0;
    if (session.deck_scope_generation != wqn::GetDeckScopeGeneration()) return ESP_ERR_INVALID_STATE;
    if (!SessionCursorOrdinal(session, &ordinal)) return ESP_ERR_INVALID_ARG;
    const auto& last = observations.back();
    if (last.sequence == UINT64_MAX || session.remote.next_sequence != last.sequence + 1 ||
        ordinal != last.next_position || session.phase != last.next_phase) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < observations.size(); ++i) {
        const auto& value = observations[i];
        if (value.session_id != session.remote.session_id || value.mode != session.remote.mode ||
            (i != 0 && (observations[i - 1].sequence == UINT64_MAX ||
                        value.sequence != observations[i - 1].sequence + 1))) return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; ++j) {
            if (observations[j].request_id == value.request_id) return ESP_ERR_INVALID_ARG;
        }
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load outbox before batch");
    std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>> records;
    records.reserve(observations.size());
    // Preflight the WHOLE batch before repair or append. Idempotence covers a
    // complete prefix left by a short/failed earlier flush, including ACK/park.
    for (const auto& value : observations) {
        bool exists = false;
        for (const auto* values : {&scan->pending, &scan->acknowledged, &scan->suspended}) {
            const auto found = std::find_if(values->begin(), values->end(),
                [&](const auto& record) { return value.request_id == record.request_id; });
            if (found == values->end()) continue;
            if (!SameObservation(ObservationFromRecord(*found), value)) return ESP_ERR_INVALID_STATE;
            exists = true;
        }
        OutboxRecord record = {};
        ESP_RETURN_ON_ERROR(BuildObservationRecord(value, OutboxRecordKind::kObservation, &record),
                            kTag, "encode batch observation");
        if (!exists) records.push_back(record);
    }
    if (records.empty()) return SaveSessionProgressProtected(session, false);
    if (scan->pending.size() + scan->suspended.size() > wqn::kWordObservationOutboxCapacity ||
        records.size() > wqn::kWordObservationOutboxCapacity -
                         scan->pending.size() - scan->suspended.size()) return ESP_ERR_NO_MEM;
    if (scan->partial_tail || scan->backup_source) {
        ESP_RETURN_ON_ERROR(CheckpointSessionsFromOutbox(*scan), kTag, "checkpoint batch repair");
        ESP_RETURN_ON_ERROR(CompactCachedOutbox(scan), kTag, "repair before batch");
    }
    if (CheckOutboxAppendBudget(*scan, true) != ESP_OK) {
        context->maintenance_required = true;
        return ESP_ERR_INVALID_SIZE;
    }
    if (records.size() > (kOutboxMaxRecords - scan->total_records - scan->pending.size()) / 2) {
        context->maintenance_required = true;
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t appended = AppendOutboxRecords(records.data(), records.size());
    if (appended != ESP_OK) {
        context->maintenance_required = appended == ESP_ERR_INVALID_SIZE;
        return appended;
    }
    scan->pending.insert(scan->pending.end(), records.begin(), records.end());
    scan->total_records += records.size();
    ESP_LOGD(kTag, "word observation batch durable: count=%u appended=%u "
                  "first_sequence=%llu last_sequence=%llu mode=%u session=%s",
        static_cast<unsigned>(observations.size()), static_cast<unsigned>(records.size()),
        static_cast<unsigned long long>(observations.front().sequence),
        static_cast<unsigned long long>(last.sequence), static_cast<unsigned>(last.mode),
        last.session_id.c_str());
    return ESP_OK;
}

struct PeekBatchContext {
    const std::vector<std::string>* excluded;
    wqn::DurableWordObservation* observation;
};

esp_err_t PeekObservationExcludingTransaction(void* opaque)
{
    auto* context = static_cast<PeekBatchContext*>(opaque);
    if (context == nullptr || context->observation == nullptr ||
        (context->excluded != nullptr && context->excluded->size() > kOutboxAppendBatchCapacity))
        return ESP_ERR_INVALID_ARG;
    auto* observation = context->observation;
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load word outbox");
    if (scan->partial_tail || scan->backup_source) {
        ESP_RETURN_ON_ERROR(
            CheckpointSessionsFromOutbox(*scan),
            kTag,
            "checkpoint before outbox peek repair");
        ESP_RETURN_ON_ERROR(
            CompactCachedOutbox(scan),
            kTag,
            "repair word outbox tail");
    }
    const auto pending = std::find_if(
        scan->pending.begin(), scan->pending.end(),
        [&](const OutboxRecord& candidate) {
            if (context->excluded != nullptr && std::find(
                    context->excluded->begin(), context->excluded->end(),
                    candidate.request_id) != context->excluded->end()) return false;
            return std::none_of(
                scan->suspended.begin(), scan->suspended.end(),
                [&](const OutboxRecord& parked) {
                    return std::strcmp(candidate.session_id, parked.session_id) == 0;
                });
        });
    if (pending == scan->pending.end()) return ESP_ERR_NOT_FOUND;
    *observation = ObservationFromRecord(*pending);
    return ESP_OK;
}

esp_err_t PeekObservationTransaction(void* context)
{
    PeekBatchContext batch{nullptr, static_cast<wqn::DurableWordObservation*>(context)};
    return PeekObservationExcludingTransaction(&batch);
}

struct AckBatchContext {
    const std::vector<std::string>* request_ids;
    bool maintenance_required = false;
};

esp_err_t AckObservationBatchTransaction(void* opaque)
{
    auto* context = static_cast<AckBatchContext*>(opaque);
    if (context == nullptr || context->request_ids == nullptr || context->request_ids->empty() ||
        context->request_ids->size() > kOutboxAppendBatchCapacity) return ESP_ERR_INVALID_ARG;
    context->maintenance_required = false;
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load outbox before ACK batch");
    std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>> records, acknowledged;
    for (const auto& request_id : *context->request_ids) {
        if (request_id.empty()) return ESP_ERR_INVALID_ARG;
        if (std::any_of(acknowledged.begin(), acknowledged.end(),
                [&](const auto& value) { return request_id == value.request_id; })) continue;
        const auto found = std::find_if(scan->pending.begin(), scan->pending.end(),
            [&](const auto& value) { return request_id == value.request_id; });
        if (found == scan->pending.end()) {
            if (std::none_of(scan->acknowledged.begin(), scan->acknowledged.end(),
                    [&](const auto& value) { return request_id == value.request_id; }))
                return ESP_ERR_NOT_FOUND;
            continue;
        }
        OutboxRecord record = {};
        ESP_RETURN_ON_ERROR(BuildObservationRecord(ObservationFromRecord(*found),
            OutboxRecordKind::kAck, &record), kTag, "encode batch ACK");
        records.push_back(record);
        acknowledged.push_back(*found);
    }
    if (records.empty()) return ESP_OK;
    if (scan->partial_tail || scan->backup_source ||
        CheckOutboxAppendBudget(*scan, false) != ESP_OK ||
        records.size() > kOutboxMaxRecords - scan->total_records) {
        context->maintenance_required = true;
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t result = AppendOutboxRecords(records.data(), records.size());
    if (result != ESP_OK) {
        context->maintenance_required = result == ESP_ERR_INVALID_SIZE;
        return result;
    }
    for (const auto& value : acknowledged) {
        scan->pending.erase(std::remove_if(scan->pending.begin(), scan->pending.end(),
            [&](const auto& pending) { return std::strcmp(pending.request_id, value.request_id) == 0; }),
            scan->pending.end());
        scan->acknowledged.push_back(value);
    }
    scan->ack_records += records.size();
    scan->total_records += records.size();
    ESP_LOGD(kTag, "word ACK batch durable: count=%u", static_cast<unsigned>(records.size()));
    return ESP_OK;
}

struct QuarantineContext {
    const std::string* request_id;
    bool maintenance_required = false;
};

esp_err_t QuarantineObservationTransaction(void* opaque)
{
    auto* context = static_cast<QuarantineContext*>(opaque);
    if (context == nullptr || context->request_id == nullptr ||
        context->request_id->empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    context->maintenance_required = false;
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load outbox before quarantine");
    const auto pending = std::find_if(
        scan->pending.begin(), scan->pending.end(),
        [&](const auto& value) {
            return *context->request_id == value.request_id;
        });
    if (pending == scan->pending.end()) {
        return std::find_if(
                   scan->acknowledged.begin(),
                   scan->acknowledged.end(),
                   [&](const auto& value) {
                       return *context->request_id == value.request_id;
                   }) != scan->acknowledged.end()
            ? ESP_OK
            : ESP_ERR_NOT_FOUND;
    }

    if (scan->partial_tail || scan->backup_source || CheckOutboxAppendBudget(*scan, false) != ESP_OK) {
        context->maintenance_required = true;
        return ESP_ERR_INVALID_SIZE;
    }
    // Preserve the complete observation in a separate durable journal before
    // removing it from the upload head. If the ACK append fails, a retry may
    // duplicate this forensic record, but it can never lose the observation.
    const OutboxRecord rejected_record = *pending;
    ESP_RETURN_ON_ERROR(
        AppendRejectedOutboxRecord(rejected_record),
        kTag,
        "append rejected word observation");
    OutboxRecord ack_record = {};
    ESP_RETURN_ON_ERROR(
        BuildObservationRecord(
            ObservationFromRecord(rejected_record),
            OutboxRecordKind::kAck,
            &ack_record),
        kTag,
        "encode quarantined word ack");
    const esp_err_t appended = AppendOutboxRecord(ack_record);
    if (appended != ESP_OK) {
        context->maintenance_required = appended == ESP_ERR_INVALID_SIZE;
        return appended;
    }

    scan->acknowledged.push_back(rejected_record);
    scan->pending.erase(pending);
    ++scan->ack_records;
    ++scan->total_records;
    ESP_LOGW(
        kTag,
        "word observation quarantined: request=%s",
        context->request_id->c_str());
    return ESP_OK;
}

struct SuspendContext {
    const std::string* request_id;
    wqn::OutboxSuspendReason reason;
};

esp_err_t SuspendObservationTransaction(void* opaque)
{
    auto* context = static_cast<SuspendContext*>(opaque);
    if (context == nullptr || context->request_id == nullptr ||
        context->request_id->empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load outbox before suspend");
    auto pending = std::find_if(
        scan->pending.begin(),
        scan->pending.end(),
        [&](const auto& value) {
            return *context->request_id == value.request_id;
        });
    if (pending == scan->pending.end()) {
        // Idempotent: already parked (or already gone) is success, matching
        // the quarantine transaction's replay tolerance.
        return std::find_if(
                   scan->suspended.begin(),
                   scan->suspended.end(),
                   [&](const auto& value) {
                       return *context->request_id == value.request_id;
                   }) != scan->suspended.end()
                ? ESP_OK
                : ESP_ERR_NOT_FOUND;
    }

    const bool first_suspend = scan->suspended.empty();
    if (first_suspend || scan->partial_tail || scan->backup_source ||
        CheckOutboxAppendBudget(*scan, false) != ESP_OK) {
        // First park establishes a marker-free fallback with two rewrites.
        // Later parks need only one healing/reclaim pass if the source or
        // budget is invalid. Existing parked payloads keep their backup.
        ESP_RETURN_ON_ERROR(
            CheckpointSessionsFromOutbox(*scan),
            kTag,
            "checkpoint before first word suspend");
        ESP_RETURN_ON_ERROR(
            CompactCachedOutbox(scan),
            kTag,
            "prepare word suspend fallback");
        if (first_suspend) {
            ESP_RETURN_ON_ERROR(
                CompactCachedOutbox(scan),
                kTag,
                "refresh word suspend fallback");
        }
        pending = std::find_if(
            scan->pending.begin(), scan->pending.end(),
            [&](const auto& value) {
                return *context->request_id == value.request_id;
            });
        if (pending == scan->pending.end()) return ESP_ERR_NOT_FOUND;
    }

    // Park the head durably first: the marker append is fsync'd before the
    // cache mutates, so a crash mid-transaction replays the marker against
    // the still-present observation on the next scan.
    OutboxRecord suspend_record = {};
    ESP_RETURN_ON_ERROR(CheckOutboxAppendBudget(*scan, false), kTag, "word suspend quota");
    ESP_RETURN_ON_ERROR(
        BuildSuspendRecord(*context->request_id, context->reason, &suspend_record),
        kTag,
        "encode word suspend record");
    ESP_RETURN_ON_ERROR(
        AppendOutboxRecord(suspend_record),
        kTag,
        "append word suspend record");

    scan->suspended.push_back(*pending);
    scan->suspended_reasons.push_back(
        static_cast<uint16_t>(static_cast<uint8_t>(context->reason)));
    scan->pending.erase(pending);
    ++scan->total_records;
    ++scan->suspend_records;
    ESP_LOGE(
        kTag,
        "word observation parked (%s): request=%s",
        wqn::OutboxSuspendReasonName(context->reason),
        context->request_id->c_str());
    return MaybeCompactCachedOutbox(scan);
}

esp_err_t SnapshotTransaction(void* context)
{
    auto* snapshot = static_cast<wqn::WordOutboxSnapshot*>(context);
    if (snapshot == nullptr) return ESP_ERR_INVALID_ARG;
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load word outbox snapshot");
    snapshot->pending_count = scan->pending.size();
    snapshot->suspended_count = scan->suspended.size();
    snapshot->blocked_count = static_cast<size_t>(std::count_if(
        scan->pending.begin(), scan->pending.end(),
        [&](const OutboxRecord& candidate) {
            return std::any_of(
                scan->suspended.begin(), scan->suspended.end(),
                [&](const OutboxRecord& parked) {
                    return std::strcmp(candidate.session_id, parked.session_id) == 0;
                });
        }));
    snapshot->capacity = wqn::kWordObservationOutboxCapacity;
    return ESP_OK;
}

template <typename Transaction>
esp_err_t ExecuteWithStorageLease(
    const char* holder,
    Transaction transaction,
    void* context,
    bool foreground = false)
{
    wqn::runtime::SleepLease lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kStorage, holder, __FILE__, __LINE__);
    if (!lease) return ESP_ERR_INVALID_STATE;
    return foreground
        ? wqn::services::ExecuteForegroundStorageTransaction(
              transaction, context, holder)
        : wqn::services::ExecuteStorageTransactionNamed(
              transaction, context, holder);
}

esp_err_t RunOutboxMaintenance(OutboxMaintenanceContext* context, const char* owner)
{
    if (context == nullptr) return ESP_ERR_INVALID_ARG;
    // For an off-owner caller, each dispatch ends an owner transaction and
    // lets foreground run at the boundary. Existing owner-task passthrough
    // stays inline; it cannot yield, nor can a running VFS call be preempted.
    constexpr size_t step_count = sizeof(kPersistedSessionModes) / sizeof(kPersistedSessionModes[0]) + 1;
    for (size_t step = 0; step < step_count && !context->done; ++step) {
        ESP_RETURN_ON_ERROR(
            wqn::services::ExecuteStorageTransactionNamed(
                OutboxMaintenanceStepTransaction, context, owner),
            kTag, "execute outbox maintenance step");
    }
    return context->done ? ESP_OK : ESP_ERR_INVALID_STATE;
}

template <typename Transaction, typename Context>
esp_err_t ExecuteWithOutboxMaintenance(
    const char* holder, Transaction transaction, Context* context,
    bool foreground = false, bool maintain_after_commit = true,
    const wqn::WordOutboxMaintenanceGate& maintenance_gate = {})
{
    if (context == nullptr) return ESP_ERR_INVALID_ARG;
    auto lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kStorage, holder, __FILE__, __LINE__);
    if (!lease) return ESP_ERR_INVALID_STATE;
    const auto execute = [&]() {
        return foreground
            ? wqn::services::ExecuteForegroundStorageTransaction(transaction, context, holder)
            : wqn::services::ExecuteStorageTransactionNamed(transaction, context, holder);
    };
    esp_err_t result = execute();
    if (result != ESP_OK && context->maintenance_required) {
        // A legacy oversized journal or a damaged source must be healed
        // before appending. Retry ONCE after a fenced, stepped compaction;
        // proof deferral leaves the original error and all records intact.
        OutboxMaintenanceContext repair;
        repair.force_compact = true;
        ESP_RETURN_ON_ERROR(RunOutboxMaintenance(&repair, "word-outbox-space-reclaim"),
                            kTag, "reclaim before word outbox mutation");
        if (repair.deferred) return result;
        result = execute();
    }
    ESP_RETURN_ON_ERROR(result, kTag, "commit word outbox mutation");
    if (!maintain_after_commit) return ESP_OK;
    OutboxMaintenanceContext maintenance;
    maintenance.maintenance_gate = maintenance_gate;
    // Keep the same lease across all queue waits. A changed proof/deadline
    // defers maintenance without changing the already-durable mutation result;
    // actual maintenance I/O failures still propagate to the caller.
    return RunOutboxMaintenance(&maintenance, "word-outbox-maintenance");
}


}  // namespace

namespace wqn {


esp_err_t CompactWordSessionData(
    const protocol::word_study_v1::SessionData& source,
    StoredWordSessionData* destination)
{
    if (destination == nullptr || source.scope.deck_ids.size() >
            protocol::word_study_v1::kMaxDecks ||
        source.snapshot.size() > protocol::word_study_v1::kMaxDecks ||
        source.items.size() > protocol::word_study_v1::kMaxSessionItems) {
        return ESP_ERR_INVALID_ARG;
    }
    StoredWordSessionData compact;
    compact.session_id = source.session_id;
    compact.mode = source.mode;
    compact.purpose = source.purpose;
    compact.ordering = source.ordering;
    compact.seed = source.seed;
    compact.include_mastered = source.scope.include_mastered;
    compact.optional_count = source.optional_count;
    compact.next_sequence = source.next_sequence;
    compact.progress_revision = source.progress_revision;
    compact.cursor = source.cursor;
    compact.has_more = source.has_more;
    compact.deck_ids.reserve(source.scope.deck_ids.size());
    for (const std::string& source_id : source.scope.deck_ids) {
        StoredWordDeckId deck;
        if (!CopyFixedText(deck.value, sizeof(deck.value), source_id)) {
            return ESP_ERR_INVALID_ARG;
        }
        compact.deck_ids.push_back(deck);
    }
    compact.snapshot.reserve(source.snapshot.size());
    for (const auto& source_snapshot : source.snapshot) {
        StoredWordPackSnapshot snapshot;
        if (!CopyFixedText(
                snapshot.deck_id, sizeof(snapshot.deck_id), source_snapshot.deck_id) ||
            !CopyFixedText(
                snapshot.sha256, sizeof(snapshot.sha256), source_snapshot.sha256)) {
            return ESP_ERR_INVALID_ARG;
        }
        snapshot.content_revision = source_snapshot.content_revision;
        snapshot.pack_revision = source_snapshot.pack_revision;
        compact.snapshot.push_back(snapshot);
    }
    compact.items.reserve(source.items.size());
    for (const auto& source_item : source.items) {
        StoredWordSessionItem item;
        if (!CopyFixedText(item.item_id, sizeof(item.item_id), source_item.item_id) ||
            !CopyFixedText(item.deck_id, sizeof(item.deck_id), source_item.deck_id)) {
            return ESP_ERR_INVALID_ARG;
        }
        item.ordinal = source_item.ordinal;
        compact.items.push_back(item);
    }
    *destination = std::move(compact);
    return ESP_OK;
}

bool WordSessionSnapshotMatches(
    const StoredWordSessionData& session,
    const protocol::word_study_v1::CandidatePageData& page)
{
    if (session.snapshot.size() != page.snapshot.size()) return false;
    for (size_t index = 0; index < session.snapshot.size(); ++index) {
        const auto& stored = session.snapshot[index];
        const auto& remote = page.snapshot[index];
        if (remote.deck_id != stored.deck_id ||
            remote.content_revision != stored.content_revision ||
            remote.pack_revision != stored.pack_revision ||
            remote.sha256 != stored.sha256) {
            return false;
        }
    }
    return true;
}

esp_err_t ExtendPersistedWordSessionWithPage(
    const PersistedWordSession& persisted,
    const protocol::word_study_v1::CandidatePageData& page,
    PersistedWordSession* updated)
{
    // [ui-gates] Validates a candidate page against the session snapshot it was
    // fetched for and produces the extended snapshot: the answered prefix is
    // trimmed, the page's items are appended in ordinal order, and the cursor is
    // advanced. Pure -- no I/O -- so both the cloud runner (which also persists
    // the result) and the UI's stale-merge path can call it.
    //
    // Error codes are the transport for the UI's messages; see
    // WordPageExtendMessage in word_app.cpp.
    if (updated == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto& remote = persisted.remote;
    if (page.session_id != remote.session_id || page.ordering != remote.ordering ||
        page.candidate_policy_version !=
            protocol::word_study_v1::CandidatePolicyVersionName(remote.ordering) ||
        page.seed != remote.seed || page.progress_revision != remote.progress_revision ||
        page.cursor != remote.cursor ||
        !WordSessionSnapshotMatches(remote, page)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (persisted.position > remote.items.size()) {
        return ESP_ERR_INVALID_STATE;
    }
    PersistedWordSession candidate = persisted;
    auto& candidate_remote = candidate.remote;
    if (candidate.position > 0) {
        candidate_remote.items.erase(
            candidate_remote.items.begin(),
            candidate_remote.items.begin() + candidate.position);
        candidate.position = 0;
    }
    if (candidate_remote.items.size() + page.items.size() >
        protocol::word_study_v1::kCandidateWindowSize) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t expected_ordinal = candidate_remote.items.empty()
        ? (page.items.empty() ? 0 : page.items.front().ordinal)
        : candidate_remote.items.back().ordinal + 1;
    for (const auto& source : page.items) {
        if (source.ordinal != expected_ordinal || source.item_id.size() != 36 ||
            source.deck_id.size() != 36) {
            return ESP_ERR_INVALID_ARG;
        }
        StoredWordSessionItem item;
        std::snprintf(item.item_id, sizeof(item.item_id), "%s", source.item_id.c_str());
        std::snprintf(item.deck_id, sizeof(item.deck_id), "%s", source.deck_id.c_str());
        item.ordinal = source.ordinal;
        candidate_remote.items.push_back(item);
        ++expected_ordinal;
    }
    candidate_remote.cursor = page.next_cursor;
    candidate_remote.has_more = page.has_more;
    *updated = std::move(candidate);
    return ESP_OK;
}

esp_err_t LoadPersistedWordSession(
    protocol::word_study_v1::Mode mode,
    PersistedWordSession* session)
{
    if (session == nullptr) return ESP_ERR_INVALID_ARG;
    *session = {};
    LoadSessionContext context{mode, session};
    return ExecuteWithStorageLease(
        "word-session-load", LoadSessionTransaction, &context);
}

esp_err_t SavePersistedWordSession(const PersistedWordSession& session)
{
    return ExecuteWithStorageLease(
        "word-session-save",
        SaveSessionTransaction,
        const_cast<PersistedWordSession*>(&session));
}

esp_err_t SaveWordSessionCursor(const PersistedWordSession& session)
{
    // Pause/resume flip only PersistedWordSession::paused. Persist that single
    // flag (~52 bytes) instead of rewriting the whole candidate snapshot, which
    // blocked the UI thread for 2-3 s. Foreground so the tiny write is not
    // queued behind background storage work.
    return ExecuteWithStorageLease(
        "word-session-cursor",
        SaveCursorTransaction,
        const_cast<PersistedWordSession*>(&session),
        true);
}

esp_err_t ClearPersistedWordSession(protocol::word_study_v1::Mode mode)
{
    ClearSessionContext context{mode};
    return ExecuteWithStorageLease(
        "word-session-clear", ClearSessionTransaction, &context);
}

esp_err_t CommitWordObservation(
    const DurableWordObservation& observation,
    const PersistedWordSession& advanced_session)
{
    CommitContext context{&observation, &advanced_session};
    return ExecuteWithStorageLease(
        "word-observation-commit",
        CommitObservationTransaction,
        &context,
        true);
}

esp_err_t CommitWordObservations(
    const std::vector<DurableWordObservation>& observations,
    const PersistedWordSession& advanced_session)
{
    CommitBatchContext context{&observations, &advanced_session};
    return ExecuteWithOutboxMaintenance(
        "word-observation-batch", CommitObservationBatchTransaction, &context, true, false);
}

esp_err_t PeekPendingWordObservation(DurableWordObservation* observation)
{
    if (observation == nullptr) return ESP_ERR_INVALID_ARG;
    *observation = {};
    return ExecuteWithStorageLease(
        "word-outbox-peek", PeekObservationTransaction, observation);
}

esp_err_t PeekPendingWordObservationExcluding(
    const std::vector<std::string>& request_ids, DurableWordObservation* observation)
{
    if (observation == nullptr) return ESP_ERR_INVALID_ARG;
    *observation = {};
    PeekBatchContext context{&request_ids, observation};
    return ExecuteWithStorageLease("word-outbox-peek-batch", PeekObservationExcludingTransaction, &context);
}

esp_err_t AcknowledgeWordObservations(
    const std::vector<std::string>& request_ids,
    const WordOutboxMaintenanceGate& maintenance_gate)
{
    AckBatchContext context{&request_ids};
    return ExecuteWithOutboxMaintenance("word-outbox-ack-batch", AckObservationBatchTransaction,
                                       &context, false, true, maintenance_gate);
}

esp_err_t MaintainWordObservationOutbox(
    const WordOutboxMaintenanceGate& maintenance_gate, bool* deferred)
{
    if (deferred != nullptr) *deferred = false;
    auto lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kStorage, "word-outbox-maintenance", __FILE__, __LINE__);
    if (!lease) return ESP_ERR_INVALID_STATE;
    OutboxMaintenanceContext context;
    context.maintenance_gate = maintenance_gate;
    const esp_err_t result = RunOutboxMaintenance(&context, "word-outbox-maintenance");
    if (deferred != nullptr) *deferred = context.deferred;
    return result;
}

esp_err_t QuarantinePendingWordObservation(const std::string& request_id)
{
    QuarantineContext context{&request_id};
    return ExecuteWithOutboxMaintenance(
        "word-outbox-quarantine",
        QuarantineObservationTransaction,
        &context);
}

esp_err_t SuspendPendingWordObservation(
    const std::string& request_id,
    OutboxSuspendReason reason)
{
    SuspendContext context{&request_id, reason};
    return ExecuteWithStorageLease(
        "word-outbox-suspend",
        SuspendObservationTransaction,
        &context);
}

esp_err_t ReadWordOutboxSnapshot(WordOutboxSnapshot* snapshot)
{
    if (snapshot == nullptr) return ESP_ERR_INVALID_ARG;
    *snapshot = {};
    return ExecuteWithStorageLease("word-outbox-snapshot", SnapshotTransaction, snapshot);
}

esp_err_t PrepareWordObservationOutboxForSleep(int64_t deadline_us)
{
    OutboxMaintenanceContext context;
    context.for_sleep = true;
    context.deadline_us = deadline_us;
    // Sleep quiescing rejects new SleepLeases. PowerCoordinator calls this
    // only after existing storage blockers are drained, so submit directly to
    // the sole storage owner.
    return RunOutboxMaintenance(&context, "word-outbox-sleep-compact");
}

}  // namespace wqn
