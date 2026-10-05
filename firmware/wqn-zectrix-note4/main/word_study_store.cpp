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
#include "esp_spiffs.h"  // [measure] esp_spiffs_info for the bench capacity log
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"  // [measure] xTaskCreate / vTaskDelete for the bench
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
constexpr size_t kRuntimeCompactAckThreshold = 32;
constexpr size_t kRejectedOutboxCapacity = 256;
constexpr wqn::protocol::word_study_v1::Mode kPersistedSessionModes[] = {
    wqn::protocol::word_study_v1::Mode::kSequential,
    wqn::protocol::word_study_v1::Mode::kReview,
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

// [measure] Phase-1 write-path instrumentation for the storage rewrite
// (doc/1005-storage-rewrite-todo.md §四.1). Emits exactly one line per
// AtomicWrite call -- the destructor fires on every exit path, including the
// early returns that skip later operations (those fields then read 0).
// stat_ms is deliberately a sixth field: FileExists(primary) below is itself a
// SPIFFS metadata operation and is not one of the five §四.1 names an
// "operation", so leaving it out would silently credit its cost to whatever
// happens to be measured next.
// The point is the reconciliation, not the individual numbers: the six parts
// must be summed and compared against the enclosing transaction's elapsed_ms.
// If they do not add up to the whole, the cost is not in AtomicWrite and the
// rewrite has to look somewhere else. Delete this block with the rest of the
// [measure] tags once the split is recorded in §一.
class AtomicWriteProbe {
  public:
    AtomicWriteProbe(size_t bytes, bool preserve_backup)
        : bytes_(bytes), preserve_backup_(preserve_backup) {}

    ~AtomicWriteProbe() {
        ESP_LOGI(
            kTag,
            "atomic write: bytes=%u backup=%d fopen_ms=%lld write_ms=%lld "
            "stat_ms=%lld remove_ms=%lld rename_backup_ms=%lld "
            "rename_primary_ms=%lld total_ms=%lld",
            static_cast<unsigned>(bytes_),
            preserve_backup_ ? 1 : 0,
            static_cast<long long>(fopen_ms_),
            static_cast<long long>(write_ms_),
            static_cast<long long>(stat_ms_),
            static_cast<long long>(remove_ms_),
            static_cast<long long>(rename_backup_ms_),
            static_cast<long long>(rename_primary_ms_),
            static_cast<long long>((esp_timer_get_time() - entered_us_) / 1000));
    }

    void AfterFopen() { fopen_ms_ = Delta(); }
    void AfterWrite() { write_ms_ = Delta(); }
    void AfterStat() { stat_ms_ = Delta(); }
    void AfterRemove() { remove_ms_ = Delta(); }
    void AfterRenameBackup() { rename_backup_ms_ = Delta(); }
    void AfterRenamePrimary() { rename_primary_ms_ = Delta(); }

  private:
    int64_t Delta() {
        const int64_t now_us = esp_timer_get_time();
        const int64_t elapsed_ms = (now_us - mark_us_) / 1000;
        mark_us_ = now_us;
        return elapsed_ms;
    }

    size_t bytes_;
    bool preserve_backup_;
    int64_t entered_us_ = esp_timer_get_time();
    int64_t mark_us_ = entered_us_;
    int64_t fopen_ms_ = 0;
    int64_t write_ms_ = 0;
    int64_t stat_ms_ = 0;
    int64_t remove_ms_ = 0;
    int64_t rename_backup_ms_ = 0;
    int64_t rename_primary_ms_ = 0;
};

esp_err_t AtomicWrite(
    const char* primary,
    const char* temporary,
    const char* backup,
    const void* bytes,
    size_t size,
    bool preserve_backup = false)
{
    AtomicWriteProbe probe(size, preserve_backup);
    FILE* file = std::fopen(temporary, "wb");
    probe.AfterFopen();
    if (file == nullptr) return ESP_FAIL;
    const bool written = std::fwrite(bytes, 1, size, file) == size;
    const bool durable = written && std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    probe.AfterWrite();
    if (!durable || !closed) {
        std::remove(temporary);
        return ESP_FAIL;
    }
    const bool had_primary = FileExists(primary);
    probe.AfterStat();
    if (preserve_backup) {
        if (had_primary && std::remove(primary) != 0 && errno != ENOENT) {
            std::remove(temporary);
            return ESP_FAIL;
        }
        probe.AfterRemove();
        if (std::rename(temporary, primary) != 0) {
            std::remove(temporary);
            return ESP_FAIL;
        }
        probe.AfterRenamePrimary();
        return ESP_OK;
    }
    if (had_primary) {
        if (std::remove(backup) != 0 && errno != ENOENT) {
            std::remove(temporary);
            return ESP_FAIL;
        }
        probe.AfterRemove();
        if (std::rename(primary, backup) != 0) {
            std::remove(temporary);
            return ESP_FAIL;
        }
        probe.AfterRenameBackup();
    }
    if (std::rename(temporary, primary) != 0) {
        if (had_primary) std::rename(backup, primary);
        std::remove(temporary);
        return ESP_FAIL;
    }
    probe.AfterRenamePrimary();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// [measure] Phase-1 write bench (doc/1005-storage-rewrite-todo.md §四 2/3/4).
//
// WHY A BENCH AND NOT REAL TRAFFIC: every number in §一 that would decide the
// rewrite is n=1 (session-save, cursor) or one aggregated bucket (background).
// A single sample cannot carry a rewrite. This synthesizes repeatable writes so
// the three open questions get real n:
//   A2/A4  -- does cost grow with write count (GC pressure), and what does the
//             backup rotation actually cost (3 KB rotated vs 3 KB direct)?
//   A3     -- what share of a write is fsync (see the no-fsync shape)?
//   §四.1  -- the per-op split is already logged by AtomicWriteProbe above; the
//             bench is what gives that split a stable, repeated input.
//
// SHAPE CONSTRAINTS (hard, from the [watchdog] note in
// services/storage_service.cpp): SPIFFS lookups are CPU-bound directory scans
// that never block, so a long uninterrupted transaction starves IDLE0 past the
// 10 s task_wdt timeout and panics the device. Therefore ONE write per
// transaction, never a loop inside one, and the service's own vTaskDelay(1)
// between transactions plus a delay here keeps IDLE0 fed.
//
// TEMPORARY: gated by the constexpr below, not a Kconfig symbol (a new symbol
// silently evaluates to 0 until a reconfigure, which would burn a flash for
// nothing). Remove this entire block before the rewrite lands.
constexpr bool kStorageBenchEnabled = true;
constexpr int kBenchRounds = 4;
constexpr size_t kBenchMaxBytes = 3072;
constexpr TickType_t kBenchStartDelayTicks = pdMS_TO_TICKS(25000);
constexpr size_t kBenchReserveBytes = 64 * 1024;

struct BenchShape {
    const char* name;
    size_t bytes;
    bool preserve_backup;
    bool append;
};

// probe = a lookup with no write at all: fopen("rb") on a path that does not
// exist. That is the purest measurement of the scan cost the model blames.
// session3k vs session3k-direct is §四.4: same bytes, rotated vs written over
// the primary in place, so the difference is what the backup rotation costs.
//
// append exists to separate two explanations that the write shapes alone cannot
// tell apart. AppendOutboxRecordTo() does fopen("ab") + fwrite + fsync + fclose
// with no temp file, no remove and no rename, and its real cost is ~8 ms median
// (n=46 across 1005.4/1005.6/1005.7) against ~1875 ms for a 52 B AtomicWrite on
// the same partition in the same boot. Either the per-lookup scan dominates (in
// which case a smaller page count helps both) or object creation dominates (in
// which case it only helps the lookup tier). Appending to one fixed path
// isolates it: round 0 creates, later rounds only extend.
constexpr BenchShape kBenchShapes[] = {
    {"probe", 0, true, false},
    {"cur52", 52, true, false},
    {"journal400", 400, false, false},
    {"session3k", kBenchMaxBytes, false, false},
    {"session3k-direct", kBenchMaxBytes, true, false},
    {"append", 0, true, true},
};

struct BenchContext {
    const BenchShape* shape = nullptr;
    int round = 0;
    esp_err_t result = ESP_FAIL;
};

// Scratch paths, deliberately outside every real store's name space.
constexpr char kBenchPrimary[] = "/storage/bench.pri";
constexpr char kBenchTemp[] = "/storage/bench.tmp";
constexpr char kBenchBackup[] = "/storage/bench.bak";
constexpr char kBenchProbeMissing[] = "/storage/bench-does-not-exist";
constexpr char kBenchAppend[] = "/storage/bench.append";

// Runs ON the storage task. One AtomicWrite per invocation -- see the watchdog
// note above; a loop here would hold the task through the whole bench.
//
// This body contains ONLY the measured operation. The capacity log and the free
// space check live in BenchTask outside the timed window on purpose:
// esp_spiffs_info may itself walk pages, and counting that inside the
// transaction would inflate the very cost we are trying to measure.
esp_err_t BenchTransaction(void* opaque)
{
    BenchContext* ctx = static_cast<BenchContext*>(opaque);
    if (ctx == nullptr || ctx->shape == nullptr) return ESP_ERR_INVALID_ARG;
    const BenchShape& shape = *ctx->shape;

    if (shape.append) {
        // Mirrors AppendOutboxRecordTo(): append, flush, fsync, close. No temp
        // file, no remove, no rename. Reported on its own line rather than the
        // `atomic write:` line because it is not an AtomicWrite and folding it
        // in would corrupt the per-op reconciliation that line exists for.
        FILE* file = std::fopen(kBenchAppend, "ab");
        if (file == nullptr) {
            ctx->result = ESP_FAIL;
            return ctx->result;
        }
        const uint8_t record[52] = {};
        const bool written = std::fwrite(record, 1, sizeof(record), file) == sizeof(record);
        const bool durable =
            written && std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
        const bool closed = std::fclose(file) == 0;
        ctx->result = (durable && closed) ? ESP_OK : ESP_FAIL;
        ESP_LOGI(kTag, "storage bench append round=%d result=%s",
                 ctx->round, esp_err_to_name(ctx->result));
        return ctx->result;
    }

    if (shape.bytes == 0) {
        // Pure lookup probe: no write, so no AtomicWrite line is expected. This
        // is the cleanest read on the scan cost the model blames for the floor.
        FILE* missing = std::fopen(kBenchProbeMissing, "rb");
        if (missing != nullptr) std::fclose(missing);
        ctx->result = ESP_OK;
        return ESP_OK;
    }

    // Payload lives on the heap: the storage task stack is 20 KiB and a 3 KiB
    // buffer plus the AtomicWrite frame is not worth spending stack on.
    std::vector<uint8_t> payload(shape.bytes, 0xA5);
    ctx->result = AtomicWrite(
        kBenchPrimary, kBenchTemp, kBenchBackup,
        payload.data(), payload.size(), shape.preserve_backup);
    return ctx->result;
}

// Returns the free-space verdict and, in info_ms_out, how long
// esp_spiffs_info itself took. That cost is reported separately rather than
// folded into the write measurement: if it turns out to be hundreds of
// milliseconds it is a second, independent confirmation of the scan-cost model
// from a completely different code path -- worth having, but it must not be
// allowed to inflate the number the bench is actually about.
bool BenchFreeSpaceOk(const BenchShape& shape, int round, int64_t* info_ms_out)
{
    size_t total = 0;
    size_t used = 0;
    const int64_t info_started_us = esp_timer_get_time();
    const esp_err_t info_result = esp_spiffs_info("storage", &total, &used);
    if (info_ms_out != nullptr) {
        *info_ms_out = (esp_timer_get_time() - info_started_us) / 1000;
    }
    if (info_result != ESP_OK) {
        ESP_LOGW(kTag, "storage bench SKIP shape=%s round=%d: spiffs unavailable",
                 shape.name, round);
        return false;
    }
    if (total - used < shape.bytes + kBenchReserveBytes) {
        // Never let the bench fill the partition: the outboxes live here and a
        // full SPIFFS would cost real study records, not just this measurement.
        ESP_LOGW(kTag,
                 "storage bench SKIP shape=%s round=%d free=%u need=%u",
                 shape.name, round,
                 static_cast<unsigned>(total - used),
                 static_cast<unsigned>(shape.bytes + kBenchReserveBytes));
        return false;
    }
    return true;
}

void BenchTask(void*)
{
    // Boot runs a burst of real storage work; wait for it to drain so the
    // bench's numbers are not polluted by queue_wait from startup traffic.
    vTaskDelay(kBenchStartDelayTicks);

    // [sleep] The bench outlives the 60 s idle deadline: it starts ~25 s after
    // boot and runs for tens of seconds, so quiesce can otherwise begin mid-run
    // and truncate it (a quiet device on battery has no kUsbPower lease to
    // block sleep, and the bench's own transactions take no lease of their
    // own). Hold one kStorage lease across the whole run -- same reasoning as
    // §五.8: the lease must cover the work it protects. If quiesce already
    // started, TryAcquire fails and we skip rather than race it.
    wqn::runtime::SleepLease bench_lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kStorage, "storage-bench", __FILE__, __LINE__);
    if (!bench_lease) {
        ESP_LOGW(kTag, "storage bench ABORT: sleep quiesce already active");
        ESP_LOGI(kTag, "storage bench END total_ms=0");
        vTaskDelete(nullptr);
        return;
    }

    const int shape_count =
        static_cast<int>(sizeof(kBenchShapes) / sizeof(kBenchShapes[0]));
    int write_count = 0;
    for (const BenchShape& shape : kBenchShapes) {
        if (shape.bytes > 0) write_count += kBenchRounds;
    }
    // No predicted duration on purpose: any number here would be derived from
    // the very model this bench exists to test, and printing it would smuggle a
    // hypothesis in as a measurement. The real total is printed at END.
    ESP_LOGI(kTag, "storage bench BEGIN shapes=%d rounds=%d writes=%d",
             shape_count, kBenchRounds, write_count);

    // Mount precheck: if the partition is not up, skip the whole run rather
    // than let the bench write into an unmounted or unhealthy filesystem.
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK) {
        ESP_LOGW(kTag, "storage bench ABORT: storage partition not mounted");
        ESP_LOGI(kTag, "storage bench END total_ms=0");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "storage bench begin: total=%u used=%u free=%u",
             static_cast<unsigned>(total),
             static_cast<unsigned>(used),
             static_cast<unsigned>(total - used));

    const int64_t bench_started_us = esp_timer_get_time();

    for (const BenchShape& shape : kBenchShapes) {
        for (int round = 0; round < kBenchRounds; ++round) {
            // Checked here, outside the timed window -- see BenchTransaction.
            int64_t info_ms = 0;
            if (!BenchFreeSpaceOk(shape, round, &info_ms)) continue;
            BenchContext ctx;
            ctx.shape = &shape;
            ctx.round = round;
            const int64_t started_us = esp_timer_get_time();
            const esp_err_t dispatched =
                wqn::services::ExecuteStorageTransactionNamed(
                    BenchTransaction, &ctx, "storage-bench");
            // info_ms is appended, not inserted: it is adjacent context, not
            // part of wall_ms, and appending keeps the field order stable.
            ESP_LOGI(
                kTag,
                "storage bench round: shape=%s round=%d wall_ms=%lld result=%s "
                "info_ms=%lld",
                shape.name, round,
                static_cast<long long>((esp_timer_get_time() - started_us) / 1000),
                esp_err_to_name(dispatched),
                static_cast<long long>(info_ms));
            // Give IDLE0 a slot between transactions; the service also yields
            // one tick per transaction, this is belt-and-braces.
            vTaskDelay(2);
        }
    }

    std::remove(kBenchPrimary);
    std::remove(kBenchTemp);
    std::remove(kBenchBackup);
    std::remove(kBenchAppend);
    size_t end_total = 0;
    size_t end_used = 0;
    if (esp_spiffs_info("storage", &end_total, &end_used) == ESP_OK) {
        ESP_LOGI(kTag, "storage bench end: total=%u used=%u free=%u",
                 static_cast<unsigned>(end_total),
                 static_cast<unsigned>(end_used),
                 static_cast<unsigned>(end_total - end_used));
    }
    ESP_LOGI(kTag, "storage bench END total_ms=%lld",
             static_cast<long long>(
                 (esp_timer_get_time() - bench_started_us) / 1000));

    // [sleep] Release explicitly. vTaskDelete below ends the task WITHOUT
    // running destructors, so a stack-held SleepLease would otherwise never be
    // given back: ActiveSleepBlockerCount(kStorage) stays non-zero for the
    // life of the boot and deep sleep is blocked forever. 1005.12 caught this
    // as "long-held sleep lease: holder=storage-bench held_ms=209480" long
    // after the bench had finished.
    bench_lease.Reset();
    vTaskDelete(nullptr);
}

// BenchTask stays in this file-local namespace so it can reach AtomicWrite and
// the scratch paths above. The public starter lives at the end of the file,
// inside namespace wqn, once this anonymous namespace has closed.
// ---------------------------------------------------------------------------

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
    SessionPaths paths = {};
    if (!GetSessionCursorPaths(session.remote.mode, &paths)) {
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
    // A torn cursor write is detected by the CRC on load and ignored, so the
    // snapshot's own paused flag stands. preserve_backup keeps this to a single
    // tiny file instead of a rotation.
    return AtomicWrite(
        paths.primary, paths.temporary, paths.backup, &record, sizeof(record), true);
}

bool ReadSessionCursorPaused(
    wqn::protocol::word_study_v1::Mode mode,
    const std::string& session_id,
    bool* paused)
{
    SessionPaths paths = {};
    if (paused == nullptr || !GetSessionCursorPaths(mode, &paths)) return false;
    FILE* file = std::fopen(paths.primary, "rb");
    if (file == nullptr) return false;
    SessionCursorRecord record = {};
    const bool read_ok =
        std::fread(&record, 1, sizeof(record), file) == sizeof(record);
    std::fclose(file);
    if (!read_ok || record.magic != kSessionCursorMagic ||
        record.version != kSessionCursorVersion ||
        Crc32(&record, sizeof(record) - sizeof(record.crc)) != record.crc) {
        return false;
    }
    // Reject a leftover cursor from a previous session in the same mode slot.
    record.session_id[sizeof(record.session_id) - 1] = '\0';
    if (session_id != record.session_id) return false;
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
    std::fclose(file);
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

esp_err_t AppendOutboxRecordTo(const char* path, const OutboxRecord& record)
{
    if (path == nullptr) return ESP_ERR_INVALID_ARG;
    FILE* file = std::fopen(path, "ab");
    if (file == nullptr) return ESP_FAIL;
    const bool written = std::fwrite(&record, 1, sizeof(record), file) == sizeof(record);
    const bool durable = written && std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    return durable && closed ? ESP_OK : ESP_FAIL;
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

esp_err_t AppendOutboxRecord(const OutboxRecord& record)
{
    return AppendOutboxRecordTo(kOutboxPath, record);
}

esp_err_t CompactOutbox(
    const std::vector<OutboxRecord, wqn::WordStorePsramAllocator<OutboxRecord>>& pending,
    bool preserve_backup = false)
{
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
    ESP_RETURN_ON_ERROR(
        CompactOutbox(
            rewrite, scan->backup_source || !scan->suspended.empty()),
        kTag,
        "compact cached word outbox");
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
        if (session->remote.session_id != observation.session_id ||
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

esp_err_t CheckpointSessionsFromOutbox(const OutboxScan& scan)
{
    for (const auto mode : kPersistedSessionModes) {
        wqn::PersistedWordSession session;
        const esp_err_t load_result = LoadSessionRaw(mode, &session);
        if (load_result == ESP_ERR_NOT_FOUND) {
            continue;
        }
        ESP_RETURN_ON_ERROR(
            load_result,
            kTag,
            "load session for outbox checkpoint");
        bool changed = false;
        ReconcileSession(scan.acknowledged, &session, &changed);
        ReconcileSession(scan.pending, &session, &changed);
        ReconcileSession(scan.suspended, &session, &changed);
        if (changed) {
            ESP_RETURN_ON_ERROR(
                SaveSessionRaw(session),
                kTag,
                "checkpoint session before outbox compaction");
        }
    }
    return ESP_OK;
}

esp_err_t MaybeCompactCachedOutbox(OutboxScan* scan)
{
    // Active suspend markers must survive every compaction, so they are not
    // reclaimable records and must not continuously retrigger maintenance.
    if (scan == nullptr || scan->ack_records < kRuntimeCompactAckThreshold) {
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

struct LoadSessionContext {
    wqn::protocol::word_study_v1::Mode mode;
    wqn::PersistedWordSession* session;
};

// [load-repair] Writes three things, all of them the reconciliation of the
// session it just read against the durable outbox: re-derive the cursor when the
// outbox shows more acks than the snapshot recorded, checkpoint the sessions
// from the outbox before repairing its tail, and compact the outbox when it was
// read from a backup or ended mid-record. Doing this inside the read is what
// makes the on-disk snapshot agree with the outbox before any UI decision is
// made from it; splitting it into a separate pass would let a caller act on an
// unreconciled snapshot.
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
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load word outbox");
    bool changed = false;
    ReconcileSession(scan->acknowledged, session, &changed);
    ReconcileSession(scan->pending, session, &changed);
    ReconcileSession(scan->suspended, session, &changed);
    if (changed) {
        ESP_RETURN_ON_ERROR(SaveSessionRaw(*session), kTag, "repair word session cursor");
    }
    if (scan->partial_tail || scan->backup_source) {
        ESP_RETURN_ON_ERROR(
            CheckpointSessionsFromOutbox(*scan),
            kTag,
            "checkpoint before word outbox repair");
        ESP_RETURN_ON_ERROR(
            CompactCachedOutbox(scan),
            kTag,
            "repair word outbox tail");
    }
    // Overlay the cheap cursor. Pause/resume persist only this flag, so the
    // snapshot on disk may still read unpaused; position/phase/sequence were
    // already reconciled from the outbox above.
    bool cursor_paused = false;
    if (ReadSessionCursorPaused(
            context->mode, session->remote.session_id, &cursor_paused)) {
        session->paused = cursor_paused;
    }
    return ESP_OK;
}

esp_err_t SaveSessionTransaction(void* context)
{
    const auto& session = *static_cast<const wqn::PersistedWordSession*>(context);
    ESP_RETURN_ON_ERROR(SaveSessionRaw(session), kTag, "save word session snapshot");
    // Refresh the cursor so it always agrees with the snapshot's paused flag,
    // preventing a stale cursor from a prior session in this slot.
    return WriteSessionCursor(session);
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
    if (std::remove(paths.primary) != 0 && errno != ENOENT) return ESP_FAIL;
    if (std::remove(paths.temporary) != 0 && errno != ENOENT) return ESP_FAIL;
    if (std::remove(paths.backup) != 0 && errno != ENOENT) return ESP_FAIL;
    SessionPaths cursor_paths = {};
    if (GetSessionCursorPaths(context->mode, &cursor_paths)) {
        std::remove(cursor_paths.primary);
        std::remove(cursor_paths.temporary);
        std::remove(cursor_paths.backup);
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
        session.remote.next_sequence != observation.sequence + 1 ||
        !SessionCursorOrdinal(session, &session_ordinal) ||
        session_ordinal != observation.next_position ||
        session.phase != observation.next_phase) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t started_us = esp_timer_get_time();
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load outbox before word observation");
    const int64_t scanned_us = esp_timer_get_time();
    const auto existing = std::find_if(
        scan->pending.begin(), scan->pending.end(),
        [&](const auto& value) { return observation.request_id == value.request_id; });
    if (existing != scan->pending.end()) {
        if (!SameObservation(ObservationFromRecord(*existing), observation)) {
            return ESP_ERR_INVALID_STATE;
        }
        return SaveSessionRaw(session);
    }
    if (std::find_if(
            scan->acknowledged.begin(),
            scan->acknowledged.end(),
            [&](const auto& value) {
                return observation.request_id == value.request_id;
            }) != scan->acknowledged.end()) {
        return SaveSessionRaw(session);
    }
    const auto suspended = std::find_if(
        scan->suspended.begin(), scan->suspended.end(),
        [&](const auto& value) { return observation.request_id == value.request_id; });
    if (suspended != scan->suspended.end()) {
        return SameObservation(ObservationFromRecord(*suspended), observation)
            ? SaveSessionRaw(session)
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
    ESP_RETURN_ON_ERROR(AppendOutboxRecord(record), kTag, "append word observation");
    scan->pending.push_back(record);
    ++scan->total_records;
    const int64_t appended_us = esp_timer_get_time();
    ESP_LOGI(
        kTag,
        "word observation durable: sequence=%llu lookup_ms=%lld append_ms=%lld total_ms=%lld",
        static_cast<unsigned long long>(observation.sequence),
        static_cast<long long>((scanned_us - started_us) / 1000),
        static_cast<long long>((appended_us - scanned_us) / 1000),
        static_cast<long long>((appended_us - started_us) / 1000));
    // The durable record includes next_position, next_phase, and sequence.
    // LoadSessionTransaction and sleep preparation reconcile an older session
    // file from these records. Rewriting and fsyncing the complete session
    // here added ~2 seconds to every card action without increasing power-loss
    // safety.
    return ESP_OK;
}

esp_err_t PeekObservationTransaction(void* context)
{
    auto* observation = static_cast<wqn::DurableWordObservation*>(context);
    if (observation == nullptr) return ESP_ERR_INVALID_ARG;
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

struct AckContext {
    const std::string* request_id;
};

esp_err_t AckObservationTransaction(void* opaque)
{
    auto* context = static_cast<AckContext*>(opaque);
    if (context == nullptr || context->request_id == nullptr || context->request_id->empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load outbox before word ack");
    const auto pending = std::find_if(
        scan->pending.begin(), scan->pending.end(),
        [&](const auto& value) { return *context->request_id == value.request_id; });
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

    OutboxRecord record = {};
    wqn::DurableWordObservation ack = ObservationFromRecord(*pending);
    const OutboxRecord acknowledged = *pending;
    ESP_RETURN_ON_ERROR(
        BuildObservationRecord(ack, OutboxRecordKind::kAck, &record),
        kTag,
        "encode word ack");
    ESP_RETURN_ON_ERROR(AppendOutboxRecord(record), kTag, "append word ack");
    scan->acknowledged.push_back(acknowledged);
    scan->pending.erase(pending);
    ++scan->ack_records;
    ++scan->total_records;
    // Keep normal ACK latency bounded, but compact periodically even while USB
    // or settings prevent deep sleep. This bounds both journal size and the
    // one boot-time scan without putting compaction on every card action.
    return MaybeCompactCachedOutbox(scan);
}

struct QuarantineContext {
    const std::string* request_id;
};

esp_err_t QuarantineObservationTransaction(void* opaque)
{
    auto* context = static_cast<QuarantineContext*>(opaque);
    if (context == nullptr || context->request_id == nullptr ||
        context->request_id->empty()) {
        return ESP_ERR_INVALID_ARG;
    }
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
    ESP_RETURN_ON_ERROR(
        AppendOutboxRecord(ack_record),
        kTag,
        "append quarantined word ack");

    scan->acknowledged.push_back(rejected_record);
    scan->pending.erase(pending);
    ++scan->ack_records;
    ++scan->total_records;
    ESP_LOGW(
        kTag,
        "word observation quarantined: request=%s",
        context->request_id->c_str());
    return MaybeCompactCachedOutbox(scan);
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

    if (scan->suspended.empty()) {
        // Establish a marker-free fallback generation before introducing the
        // first kind=3 record. Two rewrites also replace a stale backup that
        // may contain an orphan marker from an interrupted earlier lifecycle.
        ESP_RETURN_ON_ERROR(
            CheckpointSessionsFromOutbox(*scan),
            kTag,
            "checkpoint before first word suspend");
        ESP_RETURN_ON_ERROR(
            CompactCachedOutbox(scan),
            kTag,
            "prepare word suspend fallback");
        ESP_RETURN_ON_ERROR(
            CompactCachedOutbox(scan),
            kTag,
            "refresh word suspend fallback");
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

struct PrepareOutboxContext {
    int64_t deadline_us;
};

esp_err_t PrepareOutboxForSleepTransaction(void* opaque)
{
    auto* context = static_cast<PrepareOutboxContext*>(opaque);
    if (context == nullptr) return ESP_ERR_INVALID_ARG;
    if (context->deadline_us > 0 && esp_timer_get_time() >= context->deadline_us) {
        // Journal entries are already fsync'd. Compaction is maintenance, not
        // a durability prerequisite, so a missed maintenance window must not
        // veto deep sleep and create a retry/power-drain loop.
        ESP_LOGW(kTag, "word outbox sleep maintenance deferred: deadline reached");
        return ESP_OK;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(
        EnsureOutboxCache(&scan), kTag, "load outbox before sleep");
    if (scan->ack_records == 0 && !scan->partial_tail &&
        !scan->backup_source) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        CheckpointSessionsFromOutbox(*scan),
        kTag,
        "checkpoint outbox before sleep");
    if (context->deadline_us > 0 && esp_timer_get_time() >= context->deadline_us) {
        ESP_LOGW(
            kTag,
            "word outbox compaction deferred after checkpoint: pending=%u ack=%u",
            static_cast<unsigned>(scan->pending.size()),
            static_cast<unsigned>(scan->ack_records));
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        CompactCachedOutbox(scan),
        kTag,
        "compact outbox before sleep");
    ESP_LOGI(
        kTag,
        "word outbox prepared for sleep: pending=%u",
        static_cast<unsigned>(scan->pending.size()));
    return ESP_OK;
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

esp_err_t PeekPendingWordObservation(DurableWordObservation* observation)
{
    if (observation == nullptr) return ESP_ERR_INVALID_ARG;
    *observation = {};
    return ExecuteWithStorageLease(
        "word-outbox-peek", PeekObservationTransaction, observation);
}

esp_err_t AcknowledgeWordObservation(const std::string& request_id)
{
    AckContext context{&request_id};
    return ExecuteWithStorageLease("word-outbox-ack", AckObservationTransaction, &context);
}

esp_err_t QuarantinePendingWordObservation(const std::string& request_id)
{
    QuarantineContext context{&request_id};
    return ExecuteWithStorageLease(
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
    PrepareOutboxContext context{deadline_us};
    // Sleep quiescing rejects new SleepLeases. PowerCoordinator calls this
    // only after existing storage blockers are drained, so submit directly to
    // the sole storage owner.
    return services::ExecuteStorageTransactionNamed(
        PrepareOutboxForSleepTransaction,
        &context,
        "word-outbox-sleep-compact");
}

}  // namespace wqn

// [measure] Temporary bench starter. Defined here, after the file-local
// namespace has closed, so it can reach BenchTask while still exposing a wqn::
// symbol for the boot hook. Remove with the rest of the [measure] block.
namespace wqn {

void StartStorageWriteBench()
{
    if (!kStorageBenchEnabled) return;
    // Small stack: this task only enqueues and waits on the completion
    // semaphore; the writes themselves run on the storage task's 20 KiB stack.
    if (xTaskCreate(BenchTask, "wqn_bench", 4096, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW(kTag, "storage bench task create failed");
    }
}

}  // namespace wqn
