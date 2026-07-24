#include "note_store.h"

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
#include "esp_timer.h"
#include "runtime/sleep_coordinator.h"
#include "services/storage_service.h"

namespace {

constexpr char kTag[] = "note_store";
// One active note session at a time, so a single fixed slot (no per-mode files).
constexpr char kSessionPath[] = "/storage/nses.v1";
constexpr char kSessionTempPath[] = "/storage/nses.tmp";
constexpr char kSessionBackupPath[] = "/storage/nses.bak";
constexpr char kOutboxPath[] = "/storage/nout.v1";
constexpr char kOutboxTempPath[] = "/storage/nout.tmp";
constexpr char kOutboxBackupPath[] = "/storage/nout.bak";
constexpr char kRejectedOutboxPath[] = "/storage/nrej.v1";
constexpr char kRejectedOutboxTempPath[] = "/storage/nrej.tmp";
constexpr char kRejectedOutboxBackupPath[] = "/storage/nrej.bak";
constexpr uint32_t kSessionMagic = UINT32_C(0x53534E51);  // "QNSS"
constexpr uint32_t kOutboxMagic = UINT32_C(0x424F4E51);   // "QNOB"
constexpr uint16_t kSessionSchemaVersion = 1;
constexpr uint16_t kOutboxSchemaVersion = 1;
constexpr size_t kMaxSessionPayloadBytes = 128U * 1024U;
constexpr size_t kMaxSessionCursorBytes = 256;
constexpr size_t kRuntimeCompactAckThreshold = 32;
constexpr size_t kRejectedOutboxCapacity = 256;
constexpr size_t kMaxSnapshot = wqn::protocol::note_study_v1::kMaxNotebooks;
constexpr size_t kMaxSessionItems = wqn::protocol::note_study_v1::kMaxSessionItems;

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
};

struct OutboxRecord {
    uint32_t magic;
    uint16_t version;
    uint8_t kind;
    uint8_t action;
    uint8_t mode;
    uint8_t reserved;
    uint64_t sequence;
    uint64_t next_sequence;
    uint32_t next_position;
    char request_id[65];
    char session_id[37];
    char item_id[37];
    char occurred_at[33];
    uint32_t crc;
};
#pragma pack(pop)

static_assert(sizeof(OutboxRecord) == 206, "unexpected note OutboxRecord layout");

struct OutboxScan {
    std::vector<OutboxRecord, wqn::NoteStorePsramAllocator<OutboxRecord>> pending;
    std::vector<OutboxRecord, wqn::NoteStorePsramAllocator<OutboxRecord>> acknowledged;
    size_t total_records = 0;
    size_t ack_records = 0;
    size_t orphan_ack_records = 0;
    bool partial_tail = false;
    bool backup_source = false;
};

// Parsed journal cached on StorageService's sole owner task so opening a note
// does not re-read and CRC the whole SPIFFS file.
OutboxScan g_outbox_cache;
bool g_outbox_cache_loaded = false;

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
    return value <= static_cast<uint8_t>(wqn::protocol::note_study_v1::Mode::kRecent);
}

bool ValidOrdering(uint8_t value)
{
    return value <=
        static_cast<uint8_t>(wqn::protocol::note_study_v1::Ordering::kLeastRecentlyViewedV1);
}

bool ValidAction(uint8_t value)
{
    return value <=
        static_cast<uint8_t>(wqn::protocol::note_study_v1::ObservationAction::kSessionPaused);
}

bool EncodeSession(
    const wqn::PersistedNoteSession& session,
    std::vector<uint8_t>* payload)
{
    if (payload == nullptr || session.remote.snapshot.size() > kMaxSnapshot ||
        session.remote.items.size() > kMaxSessionItems ||
        session.position > session.remote.items.size() ||
        session.remote.optional_count < 0 || session.remote.optional_count > 500) {
        return false;
    }
    payload->clear();
    payload->reserve(4096);
    AppendScalar<uint8_t>(payload, session.active ? 1 : 0);
    AppendScalar<uint8_t>(payload, session.paused ? 1 : 0);
    AppendScalar<uint8_t>(payload, static_cast<uint8_t>(session.remote.mode));
    AppendScalar<uint8_t>(payload, static_cast<uint8_t>(session.remote.ordering));
    AppendScalar<uint8_t>(payload, session.remote.include_archived ? 1 : 0);
    AppendScalar<uint8_t>(payload, session.remote.has_more ? 1 : 0);
    AppendScalar<uint32_t>(payload, session.position);
    AppendScalar<uint32_t>(payload, static_cast<uint32_t>(session.remote.optional_count));
    AppendScalar<uint64_t>(payload, session.remote.next_sequence);
    AppendScalar<uint64_t>(payload, session.remote.progress_revision);
    if (!AppendString(payload, session.remote.session_id, 36) ||
        !AppendString(payload, session.remote.seed, 64) ||
        !AppendString(payload, session.remote.notebook_id, 36) ||
        !AppendString(payload, session.remote.cursor, kMaxSessionCursorBytes)) {
        return false;
    }
    AppendScalar<uint16_t>(payload, static_cast<uint16_t>(session.remote.snapshot.size()));
    for (const wqn::StoredNotePackSnapshot& snapshot : session.remote.snapshot) {
        if (!AppendString(payload, snapshot.notebook_id, 36)) return false;
        AppendScalar<uint64_t>(payload, snapshot.content_revision);
        AppendScalar<uint64_t>(payload, snapshot.pack_revision);
        if (!AppendString(payload, snapshot.sha256, 64)) return false;
    }
    AppendScalar<uint16_t>(payload, static_cast<uint16_t>(session.remote.items.size()));
    for (const wqn::StoredNoteSessionItem& item : session.remote.items) {
        if (!AppendString(payload, item.item_id, 36) ||
            !AppendString(payload, item.notebook_id, 36)) {
            return false;
        }
        AppendScalar<uint64_t>(payload, item.ordinal);
        if (!AppendString(payload, item.last_opened_at, 32)) return false;
    }
    return payload->size() <= kMaxSessionPayloadBytes;
}

bool DecodeSession(
    const std::vector<uint8_t>& payload,
    wqn::PersistedNoteSession* session)
{
    using namespace wqn::protocol::note_study_v1;
    if (session == nullptr) return false;
    PayloadReader reader(payload.data(), payload.size());
    uint8_t active = 0;
    uint8_t paused = 0;
    uint8_t mode = 0;
    uint8_t ordering = 0;
    uint8_t include_archived = 0;
    uint8_t has_more = 0;
    uint32_t optional_count = 0;
    wqn::PersistedNoteSession parsed;
    if (!reader.Scalar(&active) || !reader.Scalar(&paused) ||
        !reader.Scalar(&mode) || !reader.Scalar(&ordering) ||
        !reader.Scalar(&include_archived) || !reader.Scalar(&has_more) ||
        !reader.Scalar(&parsed.position) || !reader.Scalar(&optional_count) ||
        !reader.Scalar(&parsed.remote.next_sequence) ||
        !reader.Scalar(&parsed.remote.progress_revision) ||
        !reader.String(&parsed.remote.session_id, 36) ||
        !reader.String(&parsed.remote.seed, 64) ||
        !reader.String(&parsed.remote.notebook_id, 36) ||
        !reader.String(&parsed.remote.cursor, kMaxSessionCursorBytes) ||
        !ValidMode(mode) || !ValidOrdering(ordering) || optional_count > 500 ||
        active > 1 || paused > 1 || include_archived > 1 || has_more > 1) {
        return false;
    }
    parsed.active = active == 1;
    parsed.paused = paused == 1;
    parsed.remote.mode = static_cast<Mode>(mode);
    parsed.remote.ordering = static_cast<Ordering>(ordering);
    parsed.remote.include_archived = include_archived == 1;
    parsed.remote.has_more = has_more == 1;
    parsed.remote.optional_count = static_cast<int>(optional_count);

    uint16_t count = 0;
    if (!reader.Scalar(&count) || count > kMaxSnapshot) return false;
    parsed.remote.snapshot.reserve(count);
    for (uint16_t index = 0; index < count; ++index) {
        wqn::StoredNotePackSnapshot snapshot;
        std::string notebook_id;
        std::string sha256;
        if (!reader.String(&notebook_id, 36) ||
            !reader.Scalar(&snapshot.content_revision) ||
            !reader.Scalar(&snapshot.pack_revision) ||
            !reader.String(&sha256, 64) ||
            !CopyFixedText(snapshot.notebook_id, sizeof(snapshot.notebook_id), notebook_id) ||
            !CopyFixedText(snapshot.sha256, sizeof(snapshot.sha256), sha256)) {
            return false;
        }
        parsed.remote.snapshot.push_back(std::move(snapshot));
    }
    if (!reader.Scalar(&count) || count > kMaxSessionItems) return false;
    parsed.remote.items.reserve(count);
    for (uint16_t index = 0; index < count; ++index) {
        wqn::StoredNoteSessionItem item;
        std::string item_id;
        std::string notebook_id;
        std::string last_opened_at;
        if (!reader.String(&item_id, 36) ||
            !reader.String(&notebook_id, 36) ||
            !reader.Scalar(&item.ordinal) ||
            !reader.String(&last_opened_at, 32) ||
            !CopyFixedText(item.item_id, sizeof(item.item_id), item_id) ||
            !CopyFixedText(item.notebook_id, sizeof(item.notebook_id), notebook_id) ||
            !CopyFixedText(item.last_opened_at, sizeof(item.last_opened_at), last_opened_at)) {
            return false;
        }
        parsed.remote.items.push_back(std::move(item));
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
    const wqn::PersistedNoteSession& session,
    bool preserve_backup = false)
{
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
        kSessionPath,
        kSessionTempPath,
        kSessionBackupPath,
        file_bytes.data(),
        file_bytes.size(),
        preserve_backup);
}

esp_err_t LoadSessionFile(const char* path, wqn::PersistedNoteSession* session)
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
    if (!payload_ok || trailing != EOF ||
        Crc32(payload.data(), payload.size()) != header.payload_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    return DecodeSession(payload, session) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t LoadSessionRaw(wqn::PersistedNoteSession* session)
{
    if (session == nullptr) return ESP_ERR_INVALID_ARG;
    esp_err_t result = LoadSessionFile(kSessionPath, session);
    if (result == ESP_OK) return ESP_OK;
    if (!FileExists(kSessionBackupPath)) return result;
    const esp_err_t backup_result = LoadSessionFile(kSessionBackupPath, session);
    if (backup_result == ESP_OK) {
        ESP_LOGW(kTag, "recovered note session from backup");
        return SaveSessionRaw(*session, true);
    }
    return result;
}

uint32_t RecordCrc(const OutboxRecord& record)
{
    return Crc32(&record, offsetof(OutboxRecord, crc));
}

bool CopyField(char* output, size_t output_size, const std::string& value)
{
    if (value.empty()) return false;
    return CopyFixedText(output, output_size, value);
}

wqn::DurableNoteObservation ObservationFromRecord(const OutboxRecord& record)
{
    wqn::DurableNoteObservation observation;
    observation.request_id = record.request_id;
    observation.session_id = record.session_id;
    observation.sequence = record.sequence;
    observation.item_id = record.item_id;
    observation.action =
        static_cast<wqn::protocol::note_study_v1::ObservationAction>(record.action);
    observation.mode = static_cast<wqn::protocol::note_study_v1::Mode>(record.mode);
    observation.occurred_at = record.occurred_at;
    observation.next_sequence = record.next_sequence;
    observation.next_position = record.next_position;
    return observation;
}

bool SameObservation(
    const wqn::DurableNoteObservation& left,
    const wqn::DurableNoteObservation& right)
{
    return left.request_id == right.request_id &&
        left.session_id == right.session_id && left.sequence == right.sequence &&
        left.item_id == right.item_id && left.action == right.action &&
        left.mode == right.mode && left.occurred_at == right.occurred_at &&
        left.next_sequence == right.next_sequence &&
        left.next_position == right.next_position;
}

esp_err_t BuildObservationRecord(
    const wqn::DurableNoteObservation& observation,
    OutboxRecordKind kind,
    OutboxRecord* record)
{
    if (record == nullptr ||
        observation.sequence > wqn::protocol::v3::kMaxSafeJsonInteger ||
        observation.next_sequence > wqn::protocol::v3::kMaxSafeJsonInteger ||
        !ValidAction(static_cast<uint8_t>(observation.action)) ||
        !ValidMode(static_cast<uint8_t>(observation.mode))) {
        return ESP_ERR_INVALID_ARG;
    }
    *record = {};
    record->magic = kOutboxMagic;
    record->version = kOutboxSchemaVersion;
    record->kind = static_cast<uint8_t>(kind);
    record->action = static_cast<uint8_t>(observation.action);
    record->mode = static_cast<uint8_t>(observation.mode);
    record->sequence = observation.sequence;
    record->next_sequence = observation.next_sequence;
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
                record.occurred_at[32] != '\0' || !ValidMode(record.mode) ||
                !ValidAction(record.action)) {
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
                scan->pending.begin(), scan->pending.end(),
                [&](const auto& value) { return request_id == value.request_id; });
            const auto acknowledged = std::find_if(
                scan->acknowledged.begin(), scan->acknowledged.end(),
                [&](const auto& value) { return request_id == value.request_id; });
            if (pending != scan->pending.end()) {
                if (acknowledged == scan->acknowledged.end()) {
                    scan->acknowledged.push_back(*pending);
                }
                scan->pending.erase(pending);
            } else if (acknowledged == scan->acknowledged.end()) {
                // ACKs are idempotent tombstones; a crash between compaction
                // steps can leave one whose observation is already gone.
                ++scan->orphan_ack_records;
                ESP_LOGW(kTag, "ignoring orphan note outbox ACK: request=%s", request_id.c_str());
            }
        } else {
            std::fclose(file);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    std::fclose(file);
    return scan->pending.size() <= wqn::kNoteObservationOutboxCapacity
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
        "recovered note outbox from backup: primary_error=%s pending=%u",
        esp_err_to_name(primary_result),
        static_cast<unsigned>(scan->pending.size()));
    return ESP_OK;
}

esp_err_t EnsureOutboxCache(OutboxScan** scan)
{
    if (scan == nullptr) return ESP_ERR_INVALID_ARG;
    if (!g_outbox_cache_loaded) {
        OutboxScan loaded;
        ESP_RETURN_ON_ERROR(ScanOutbox(&loaded), kTag, "load note outbox cache");
        g_outbox_cache = std::move(loaded);
        g_outbox_cache_loaded = true;
        ESP_LOGI(
            kTag,
            "note outbox cache loaded: total=%u pending=%u ack=%u orphan_ack=%u",
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

esp_err_t AppendOutboxRecord(const OutboxRecord& record)
{
    return AppendOutboxRecordTo(kOutboxPath, record);
}

// Bounded rolling forensic journal so a long run of terminal server errors
// cannot make quarantine itself stall the durable upload head.
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
            std::fseek(source, static_cast<long>(skip_records * sizeof(OutboxRecord)), SEEK_SET) != 0) {
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
    const bool durable = ok && std::fflush(output) == 0 && ::fsync(fileno(output)) == 0;
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
            "note observation quarantine rolled over: discarded=%u partial_tail=%d",
            static_cast<unsigned>(skip_records),
            discarded_partial_tail ? 1 : 0);
    }
    return ESP_OK;
}

esp_err_t CompactOutbox(
    const std::vector<OutboxRecord, wqn::NoteStorePsramAllocator<OutboxRecord>>& pending,
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
    ESP_RETURN_ON_ERROR(
        CompactOutbox(scan->pending, scan->backup_source), kTag, "compact cached note outbox");
    scan->acknowledged.clear();
    scan->total_records = scan->pending.size();
    scan->ack_records = 0;
    scan->orphan_ack_records = 0;
    scan->partial_tail = false;
    scan->backup_source = false;
    return ESP_OK;
}

// Advances a persisted note session from a durable observation. Unlike the word
// linear cursor, the note "position" is the highlighted title index carried in
// the record; the monotonic next_sequence is the server-facing invariant.
void ReconcileSession(
    const std::vector<OutboxRecord, wqn::NoteStorePsramAllocator<OutboxRecord>>& records,
    wqn::PersistedNoteSession* session,
    bool* changed)
{
    if (session == nullptr || changed == nullptr) return;
    for (const OutboxRecord& observation : records) {
        if (session->remote.session_id != observation.session_id ||
            observation.sequence < session->remote.next_sequence) {
            continue;
        }
        session->remote.next_sequence = observation.sequence + 1;
        if (observation.next_position <= session->remote.items.size()) {
            session->position = observation.next_position;
        }
        *changed = true;
    }
}

esp_err_t CheckpointSessionFromOutbox(const OutboxScan& scan)
{
    wqn::PersistedNoteSession session;
    const esp_err_t load_result = LoadSessionRaw(&session);
    if (load_result == ESP_ERR_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(load_result, kTag, "load session for outbox checkpoint");
    bool changed = false;
    ReconcileSession(scan.acknowledged, &session, &changed);
    ReconcileSession(scan.pending, &session, &changed);
    if (changed) {
        ESP_RETURN_ON_ERROR(
            SaveSessionRaw(session), kTag, "checkpoint session before outbox compaction");
    }
    return ESP_OK;
}

esp_err_t MaybeCompactCachedOutbox(OutboxScan* scan)
{
    if (scan == nullptr || scan->ack_records < kRuntimeCompactAckThreshold) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(CheckpointSessionFromOutbox(*scan), kTag, "checkpoint runtime note outbox");
    ESP_RETURN_ON_ERROR(CompactCachedOutbox(scan), kTag, "compact runtime note outbox");
    ESP_LOGI(
        kTag,
        "note outbox runtime compaction complete: pending=%u",
        static_cast<unsigned>(scan->pending.size()));
    return ESP_OK;
}

esp_err_t LoadSessionTransaction(void* opaque)
{
    auto* session = static_cast<wqn::PersistedNoteSession*>(opaque);
    if (session == nullptr) return ESP_ERR_INVALID_ARG;
    const esp_err_t session_result = LoadSessionRaw(session);
    if (session_result != ESP_OK) return session_result;
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load note outbox");
    bool changed = false;
    ReconcileSession(scan->acknowledged, session, &changed);
    ReconcileSession(scan->pending, session, &changed);
    if (changed) {
        ESP_RETURN_ON_ERROR(SaveSessionRaw(*session), kTag, "repair note session cursor");
    }
    if (scan->partial_tail || scan->backup_source) {
        ESP_RETURN_ON_ERROR(
            CheckpointSessionFromOutbox(*scan), kTag, "checkpoint before note outbox repair");
        ESP_RETURN_ON_ERROR(CompactCachedOutbox(scan), kTag, "repair note outbox tail");
    }
    return ESP_OK;
}

esp_err_t SaveSessionTransaction(void* context)
{
    return SaveSessionRaw(*static_cast<const wqn::PersistedNoteSession*>(context));
}

esp_err_t ClearSessionTransaction(void*)
{
    if (std::remove(kSessionPath) != 0 && errno != ENOENT) return ESP_FAIL;
    if (std::remove(kSessionTempPath) != 0 && errno != ENOENT) return ESP_FAIL;
    if (std::remove(kSessionBackupPath) != 0 && errno != ENOENT) return ESP_FAIL;
    return ESP_OK;
}

struct CommitContext {
    const wqn::DurableNoteObservation* observation;
    const wqn::PersistedNoteSession* advanced_session;
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
    if (session.remote.session_id != observation.session_id ||
        session.remote.next_sequence != observation.sequence + 1 ||
        session.remote.next_sequence != observation.next_sequence ||
        session.position != observation.next_position) {
        return ESP_ERR_INVALID_ARG;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load outbox before note observation");
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
            scan->acknowledged.begin(), scan->acknowledged.end(),
            [&](const auto& value) { return observation.request_id == value.request_id; }) !=
        scan->acknowledged.end()) {
        return SaveSessionRaw(session);
    }
    if (scan->pending.size() >= wqn::kNoteObservationOutboxCapacity) {
        return ESP_ERR_NO_MEM;
    }
    if (scan->partial_tail || scan->backup_source) {
        ESP_RETURN_ON_ERROR(
            CheckpointSessionFromOutbox(*scan), kTag, "checkpoint before note append repair");
        ESP_RETURN_ON_ERROR(CompactCachedOutbox(scan), kTag, "repair before note append");
    }
    OutboxRecord record = {};
    ESP_RETURN_ON_ERROR(
        BuildObservationRecord(observation, OutboxRecordKind::kObservation, &record),
        kTag,
        "encode note observation");
    ESP_RETURN_ON_ERROR(AppendOutboxRecord(record), kTag, "append note observation");
    scan->pending.push_back(record);
    ++scan->total_records;
    ESP_LOGI(
        kTag,
        "note observation durable: sequence=%llu",
        static_cast<unsigned long long>(observation.sequence));
    // The durable record carries next_sequence and next_position, so
    // LoadSessionTransaction and sleep checkpointing reconstruct the advanced
    // cursor via ReconcileSession. Rewriting and fsyncing the full session
    // snapshot here added ~2 s to every note open on SPIFFS (owner
    // note-observation-commit) without improving power-loss safety, so the
    // fresh-append path skips it -- mirroring the word observation commit.
    return ESP_OK;
}

esp_err_t PeekObservationTransaction(void* context)
{
    auto* observation = static_cast<wqn::DurableNoteObservation*>(context);
    if (observation == nullptr) return ESP_ERR_INVALID_ARG;
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load note outbox");
    if (scan->partial_tail || scan->backup_source) {
        ESP_RETURN_ON_ERROR(
            CheckpointSessionFromOutbox(*scan), kTag, "checkpoint before note peek repair");
        ESP_RETURN_ON_ERROR(CompactCachedOutbox(scan), kTag, "repair note outbox tail");
    }
    if (scan->pending.empty()) return ESP_ERR_NOT_FOUND;
    *observation = ObservationFromRecord(scan->pending.front());
    return ESP_OK;
}

struct RequestIdContext {
    const std::string* request_id;
};

esp_err_t AckObservationTransaction(void* opaque)
{
    auto* context = static_cast<RequestIdContext*>(opaque);
    if (context == nullptr || context->request_id == nullptr || context->request_id->empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load outbox before note ack");
    const auto pending = std::find_if(
        scan->pending.begin(), scan->pending.end(),
        [&](const auto& value) { return *context->request_id == value.request_id; });
    if (pending == scan->pending.end()) {
        return std::find_if(
                   scan->acknowledged.begin(), scan->acknowledged.end(),
                   [&](const auto& value) { return *context->request_id == value.request_id; }) !=
                scan->acknowledged.end()
            ? ESP_OK
            : ESP_ERR_NOT_FOUND;
    }
    OutboxRecord record = {};
    wqn::DurableNoteObservation ack = ObservationFromRecord(*pending);
    const OutboxRecord acknowledged = *pending;
    ESP_RETURN_ON_ERROR(
        BuildObservationRecord(ack, OutboxRecordKind::kAck, &record), kTag, "encode note ack");
    ESP_RETURN_ON_ERROR(AppendOutboxRecord(record), kTag, "append note ack");
    scan->acknowledged.push_back(acknowledged);
    scan->pending.erase(pending);
    ++scan->ack_records;
    ++scan->total_records;
    return MaybeCompactCachedOutbox(scan);
}

esp_err_t QuarantineObservationTransaction(void* opaque)
{
    auto* context = static_cast<RequestIdContext*>(opaque);
    if (context == nullptr || context->request_id == nullptr || context->request_id->empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load outbox before note quarantine");
    const auto pending = std::find_if(
        scan->pending.begin(), scan->pending.end(),
        [&](const auto& value) { return *context->request_id == value.request_id; });
    if (pending == scan->pending.end()) {
        return std::find_if(
                   scan->acknowledged.begin(), scan->acknowledged.end(),
                   [&](const auto& value) { return *context->request_id == value.request_id; }) !=
                scan->acknowledged.end()
            ? ESP_OK
            : ESP_ERR_NOT_FOUND;
    }
    const OutboxRecord rejected_record = *pending;
    ESP_RETURN_ON_ERROR(
        AppendRejectedOutboxRecord(rejected_record), kTag, "append rejected note observation");
    OutboxRecord ack_record = {};
    ESP_RETURN_ON_ERROR(
        BuildObservationRecord(
            ObservationFromRecord(rejected_record), OutboxRecordKind::kAck, &ack_record),
        kTag,
        "encode quarantined note ack");
    ESP_RETURN_ON_ERROR(AppendOutboxRecord(ack_record), kTag, "append quarantined note ack");
    scan->acknowledged.push_back(rejected_record);
    scan->pending.erase(pending);
    ++scan->ack_records;
    ++scan->total_records;
    ESP_LOGW(kTag, "note observation quarantined: request=%s", context->request_id->c_str());
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
        ESP_LOGW(kTag, "note outbox sleep maintenance deferred: deadline reached");
        return ESP_OK;
    }
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load outbox before note sleep");
    if (scan->ack_records == 0 && !scan->partial_tail && !scan->backup_source) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        CheckpointSessionFromOutbox(*scan), kTag, "checkpoint note outbox before sleep");
    if (context->deadline_us > 0 && esp_timer_get_time() >= context->deadline_us) {
        ESP_LOGW(kTag, "note outbox compaction deferred after checkpoint");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(CompactCachedOutbox(scan), kTag, "compact note outbox before sleep");
    ESP_LOGI(
        kTag,
        "note outbox prepared for sleep: pending=%u",
        static_cast<unsigned>(scan->pending.size()));
    return ESP_OK;
}

esp_err_t SnapshotTransaction(void* context)
{
    auto* snapshot = static_cast<wqn::NoteOutboxSnapshot*>(context);
    if (snapshot == nullptr) return ESP_ERR_INVALID_ARG;
    OutboxScan* scan = nullptr;
    ESP_RETURN_ON_ERROR(EnsureOutboxCache(&scan), kTag, "load note outbox snapshot");
    snapshot->pending_count = scan->pending.size();
    snapshot->capacity = wqn::kNoteObservationOutboxCapacity;
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
        ? wqn::services::ExecuteForegroundStorageTransaction(transaction, context, holder)
        : wqn::services::ExecuteStorageTransactionNamed(transaction, context, holder);
}

}  // namespace

namespace wqn {

esp_err_t CompactNoteSessionData(
    const protocol::note_study_v1::SessionData& source,
    StoredNoteSessionData* destination)
{
    if (destination == nullptr ||
        source.scope.notebook_ids.size() > kMaxSnapshot ||
        source.snapshot.size() > kMaxSnapshot ||
        source.items.size() > kMaxSessionItems) {
        return ESP_ERR_INVALID_ARG;
    }
    StoredNoteSessionData compact;
    compact.session_id = source.session_id;
    compact.mode = source.mode;
    compact.ordering = source.ordering;
    compact.seed = source.seed;
    // Note sessions scope exactly one notebook; keep the first (and only) id.
    compact.notebook_id =
        source.scope.notebook_ids.empty() ? std::string() : source.scope.notebook_ids.front();
    compact.include_archived = source.scope.include_archived;
    compact.optional_count = source.optional_count;
    compact.next_sequence = source.next_sequence;
    compact.progress_revision = source.progress_revision;
    compact.cursor = source.cursor;
    compact.has_more = source.has_more;
    compact.snapshot.reserve(source.snapshot.size());
    for (const auto& source_snapshot : source.snapshot) {
        StoredNotePackSnapshot snapshot;
        if (!CopyFixedText(
                snapshot.notebook_id, sizeof(snapshot.notebook_id), source_snapshot.notebook_id) ||
            !CopyFixedText(snapshot.sha256, sizeof(snapshot.sha256), source_snapshot.sha256)) {
            return ESP_ERR_INVALID_ARG;
        }
        snapshot.content_revision = source_snapshot.content_revision;
        snapshot.pack_revision = source_snapshot.pack_revision;
        compact.snapshot.push_back(snapshot);
    }
    compact.items.reserve(source.items.size());
    for (const auto& source_item : source.items) {
        StoredNoteSessionItem item;
        if (!CopyFixedText(item.item_id, sizeof(item.item_id), source_item.item_id) ||
            !CopyFixedText(item.notebook_id, sizeof(item.notebook_id), source_item.notebook_id) ||
            !CopyFixedText(
                item.last_opened_at, sizeof(item.last_opened_at), source_item.last_opened_at)) {
            return ESP_ERR_INVALID_ARG;
        }
        item.ordinal = source_item.ordinal;
        compact.items.push_back(item);
    }
    *destination = std::move(compact);
    return ESP_OK;
}

esp_err_t LoadPersistedNoteSession(PersistedNoteSession* session)
{
    if (session == nullptr) return ESP_ERR_INVALID_ARG;
    *session = {};
    return ExecuteWithStorageLease("note-session-load", LoadSessionTransaction, session);
}

esp_err_t SavePersistedNoteSession(const PersistedNoteSession& session)
{
    return ExecuteWithStorageLease(
        "note-session-save",
        SaveSessionTransaction,
        const_cast<PersistedNoteSession*>(&session));
}

esp_err_t ClearPersistedNoteSession()
{
    return ExecuteWithStorageLease("note-session-clear", ClearSessionTransaction, nullptr);
}

esp_err_t CommitNoteObservation(
    const DurableNoteObservation& observation,
    const PersistedNoteSession& advanced_session)
{
    CommitContext context{&observation, &advanced_session};
    return ExecuteWithStorageLease(
        "note-observation-commit", CommitObservationTransaction, &context, true);
}

esp_err_t PeekPendingNoteObservation(DurableNoteObservation* observation)
{
    if (observation == nullptr) return ESP_ERR_INVALID_ARG;
    *observation = {};
    return ExecuteWithStorageLease("note-outbox-peek", PeekObservationTransaction, observation);
}

esp_err_t AcknowledgeNoteObservation(const std::string& request_id)
{
    RequestIdContext context{&request_id};
    return ExecuteWithStorageLease("note-outbox-ack", AckObservationTransaction, &context);
}

esp_err_t QuarantinePendingNoteObservation(const std::string& request_id)
{
    RequestIdContext context{&request_id};
    return ExecuteWithStorageLease(
        "note-outbox-quarantine", QuarantineObservationTransaction, &context);
}

esp_err_t ReadNoteOutboxSnapshot(NoteOutboxSnapshot* snapshot)
{
    if (snapshot == nullptr) return ESP_ERR_INVALID_ARG;
    *snapshot = {};
    return ExecuteWithStorageLease("note-outbox-snapshot", SnapshotTransaction, snapshot);
}

esp_err_t PrepareNoteObservationOutboxForSleep(int64_t deadline_us)
{
    PrepareOutboxContext context{deadline_us};
    // Sleep quiescing rejects new SleepLeases; PowerCoordinator calls this only
    // after storage blockers are drained, so submit directly to the owner task.
    return services::ExecuteStorageTransactionNamed(
        PrepareOutboxForSleepTransaction, &context, "note-outbox-sleep-compact");
}

}  // namespace wqn
