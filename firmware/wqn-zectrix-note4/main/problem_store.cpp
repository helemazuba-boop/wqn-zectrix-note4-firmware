// Problem verdict outbox: a small durable JSONL journal mirroring the note
// outbox semantics (request_id idempotency, peek/ack head processing, terminal
// quarantine) without the session-snapshot half -- a verdict is a standalone
// observation, so the journal is the whole durable state. Records are tiny
// (~150 B) and capped at 200, so ack/quarantine rewrite the file atomically
// instead of maintaining a binary head cursor.

#include "problem_store.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "cJSON.h"
#include "device_protocol/json_depth_guard.h"
#include "device_protocol/json_syntax_probe.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "runtime/sleep_coordinator.h"
#include "services/storage_service.h"

namespace {

constexpr char kTag[] = "problem_store";
constexpr char kOutboxPath[] = "/storage/po_outbox.jsonl";
constexpr char kOutboxTempPath[] = "/storage/po_outbox.tmp";
constexpr char kOutboxBackupPath[] = "/storage/po_outbox.bak";
constexpr char kRejectedPath[] = "/storage/po_rejected.jsonl";
// Park markers (request_id + reason) for verdicts that must not be retried
// nor deleted. The payload line stays in po_outbox.jsonl; Peek consults this
// journal so a parked verdict neither uploads nor wedges the queue. Old
// firmware builds ignore this file entirely: on downgrade they would retry
// the parked verdicts (server rejects them again) rather than lose data.
constexpr char kParkedPath[] = "/storage/po_parked.jsonl";
constexpr size_t kMaxRejectedRecords = 50;
constexpr unsigned kProblemEvidenceSlots = 4;
constexpr size_t kMaxProblemEvidenceBytes = 128 * 1024;
constexpr size_t kMaxParkedRecords = wqn::kProblemObservationOutboxCapacity;
constexpr size_t kMaxLineBytes = 512;
constexpr size_t kMaxParkedLineBytes = 96;
constexpr size_t kMaxParkedJournalBytes =
    kMaxParkedRecords * (kMaxParkedLineBytes + 2);

// Owner-task-only negative cache, not a count of uploadable records. Publish
// only after successful reads of both journals find no payload bytes. Parked
// or malformed nonempty payloads must keep the ordinary read/validation path.
// Every payload/marker write primitive invalidates this BEFORE attempting I/O,
// including failures that may already have appended partial bytes.
bool g_empty_problem_outbox_known = false;
// Storage-owner only. An append/flush/sync/close failure may leave complete
// bytes behind. Reading those bytes cannot confirm the failed transaction.
// Reconfirm the existing file without truncation before publishing any IDs.
// Malformed tails remain errors and require separately controlled recovery.
bool g_problem_park_sync_pending = false;

bool IsValidRequestId(const std::string& value)
{
    if (value.size() < 16 || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

bool IsValidSuspendReasonName(const std::string& value)
{
    return value == "idempotency-conflict" || value == "actor-ownership" ||
        value == "invalid-identity" || value == "protocol-blocked" ||
        value == "unknown-terminal";
}

bool DecodeParkedLine(const std::string& line, std::string* request_id)
{
    if (request_id == nullptr || line.empty() ||
        line.size() > kMaxParkedLineBytes) {
        return false;
    }
    const size_t separator = line.find(' ');
    if (separator == std::string::npos ||
        line.find(' ', separator + 1) != std::string::npos) {
        return false;
    }
    const std::string identity = line.substr(0, separator);
    const std::string reason = line.substr(separator + 1);
    if (!IsValidRequestId(identity) || !IsValidSuspendReasonName(reason)) {
        return false;
    }
    *request_id = identity;
    return true;
}

bool IsUuid(const std::string& value)
{
    if (value.size() != 36) return false;
    for (size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool IsValidObservation(const wqn::DurableProblemObservation& observation)
{
    return IsValidRequestId(observation.request_id) &&
        IsUuid(observation.problem_id) && !observation.occurred_at.empty() &&
        observation.occurred_at.size() <= 40 &&
        observation.occurred_at.find('\0') == std::string::npos;
}

std::string EncodeObservationLine(const wqn::DurableProblemObservation& observation)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return std::string();
    // A local durable ACK requires every field, even if one allocation fails
    // and later allocations recover enough for cJSON_Print to succeed.
    if (!cJSON_AddStringToObject(root, "request_id", observation.request_id.c_str()) ||
        !cJSON_AddStringToObject(root, "problem_id", observation.problem_id.c_str()) ||
        !cJSON_AddStringToObject(
            root, "action",
            wqn::protocol::problem_study_v1::ReviewActionName(observation.action)) ||
        !cJSON_AddStringToObject(root, "occurred_at", observation.occurred_at.c_str())) {
        cJSON_Delete(root);
        return std::string();
    }
    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == nullptr) return std::string();
    std::string line = rendered;
    cJSON_free(rendered);
    return line;
}

enum class ObservationDecode { kValid, kMalformed, kUnavailable, kPreserved };

using ObservationJsonSyntax = wqn::protocol::JsonSyntaxProbe;

ObservationDecode DecodeObservationLine(
    const std::string& line, wqn::DurableProblemObservation* observation)
{
    if (observation == nullptr) return ObservationDecode::kMalformed;
    *observation = {};
    if (line.find('\0') != std::string::npos ||
        !wqn::protocol::JsonNestingWithinLimit(line.data(), line.size())) {
        return ObservationDecode::kMalformed;
    }
    // cJSON loses decoded string lengths. Keep complete, unrepresentable
    // records opaque: never bind them to a prefix ID or drop them on rewrite.
    if (!wqn::protocol::JsonStringsRepresentable(line.data(), line.size())) {
        return ObservationJsonSyntax(line).Complete()
            ? ObservationDecode::kPreserved : ObservationDecode::kMalformed;
    }
    cJSON* root = cJSON_ParseWithOpts(line.c_str(), nullptr, true);
    if (root == nullptr) {
        return ObservationJsonSyntax(line).Complete()
            ? ObservationDecode::kUnavailable : ObservationDecode::kMalformed;
    }
    cJSON* request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    cJSON* problem_id = cJSON_GetObjectItemCaseSensitive(root, "problem_id");
    cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    cJSON* occurred_at = cJSON_GetObjectItemCaseSensitive(root, "occurred_at");
    bool ok = cJSON_IsString(request_id) && cJSON_IsString(problem_id) &&
        cJSON_IsString(action) && cJSON_IsString(occurred_at);
    if (ok) {
        observation->request_id = request_id->valuestring;
        observation->problem_id = problem_id->valuestring;
        observation->occurred_at = occurred_at->valuestring;
        const std::string action_name = action->valuestring;
        using wqn::protocol::problem_study_v1::ReviewAction;
        if (action_name == "correct") observation->action = ReviewAction::kCorrect;
        else if (action_name == "hesitant") observation->action = ReviewAction::kHesitant;
        else if (action_name == "wrong") observation->action = ReviewAction::kWrong;
        else if (action_name == "skip") observation->action = ReviewAction::kSkip;
        else ok = false;
    }
    cJSON_Delete(root);
    return ok && IsValidObservation(*observation)
        ? ObservationDecode::kValid : ObservationDecode::kMalformed;
}

// Reads every journal line (bounded); only healthy frames are uploadable.
// A lossy rewrite first preserves the complete source, using an
// owner-scoped proof and bounded, non-overwriting evidence slots.
// The read proof detects drift within one owner transaction. Its SDK CRC is
// not an identity/authentication claim; preservation also compares all bytes.
struct OutboxReadProof {
    bool present = false;
    bool needs_repair = false;
    size_t bytes = 0;
    size_t discarded_frames = 0;
    uint32_t crc = 0;
};

esp_err_t SameEvidenceSource(const char* evidence, const OutboxReadProof& proof)
{
    FILE* source = std::fopen(kOutboxPath, "rb");
    if (source == nullptr) return ESP_FAIL;
    FILE* saved = std::fopen(evidence, "rb");
    if (saved == nullptr) { std::fclose(source); return ESP_FAIL; }
    uint8_t left[256], right[256];
    size_t total = 0;
    uint32_t crc = UINT32_MAX;
    bool equal = true;
    while (equal) {
        const size_t a = std::fread(left, 1, sizeof(left), source);
        const size_t b = std::fread(right, 1, sizeof(right), saved);
        if (a != b || total + a > kMaxProblemEvidenceBytes ||
            std::memcmp(left, right, a) != 0) { equal = false; break; }
        total += a;
        crc = esp_rom_crc32_le(crc, left, a);
        if (a == 0) break;
    }
    bool ok = std::ferror(source) == 0 && std::ferror(saved) == 0;
    if (std::fclose(source) != 0) ok = false;
    if (std::fclose(saved) != 0) ok = false;
    if (!ok) return ESP_FAIL;
    if (!equal || total != proof.bytes || (crc ^ UINT32_MAX) != proof.crc) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t EvidenceSlotState(const char* path, bool* exists, size_t* bytes)
{
    struct stat info = {};
#ifdef ESP_PLATFORM
    // Flat SPIFFS has no links/lstat. Host checks reject links explicitly.
    const int result = ::stat(path, &info);
#else
    const int result = ::lstat(path, &info);
#endif
    if (result != 0) {
        if (errno != ENOENT) return ESP_FAIL;
        *exists = false; *bytes = 0; return ESP_OK;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) return ESP_ERR_INVALID_STATE;
    *exists = true; *bytes = static_cast<size_t>(info.st_size);
    return ESP_OK;
}

esp_err_t PreserveOutboxSource(
    const OutboxReadProof& proof, char* selected, size_t capacity)
{
    if (!proof.present || proof.bytes == 0 || proof.bytes > kMaxProblemEvidenceBytes)
        return ESP_ERR_INVALID_STATE;
    char path[sizeof(kOutboxPath) + 16], temporary[sizeof(kOutboxPath) + 16];
    unsigned free_slot = kProblemEvidenceSlots;
    for (unsigned slot = 0; slot < kProblemEvidenceSlots; ++slot) {
        std::snprintf(path, sizeof(path), "%s.raw%u", kOutboxPath, slot);
        std::snprintf(temporary, sizeof(temporary), "%s.raw%u.tmp", kOutboxPath, slot);
        bool final_exists = false, temp_exists = false; size_t bytes = 0, temp_bytes = 0;
        esp_err_t result = EvidenceSlotState(path, &final_exists, &bytes);
        if (result != ESP_OK) return result;
        result = EvidenceSlotState(temporary, &temp_exists, &temp_bytes);
        if (result != ESP_OK) return result;
        if (final_exists && bytes == proof.bytes) {
            // Equality, never length alone, authorizes reuse after a failed
            // publication. A different/opaque generation remains untouched.
            result = SameEvidenceSource(path, proof);
            if (result == ESP_OK) {
                std::snprintf(selected, capacity, "%s", path);
                return ESP_OK;
            }
            if (result != ESP_ERR_INVALID_STATE) return result;
        }
        if (!final_exists && !temp_exists && free_slot == kProblemEvidenceSlots) free_slot = slot;
    }
    if (free_slot == kProblemEvidenceSlots) {
        ESP_LOGW(kTag, "problem repair held: evidence slots full; export retained originals");
        return ESP_ERR_INVALID_STATE;
    }
    std::snprintf(path, sizeof(path), "%s.raw%u", kOutboxPath, free_slot);
    std::snprintf(temporary, sizeof(temporary), "%s.raw%u.tmp", kOutboxPath, free_slot);
    FILE* source = std::fopen(kOutboxPath, "rb");
    if (source == nullptr) return ESP_FAIL;
    // Never replace a pre-existing or newly raced staging file. Interrupted
    // stages occupy their slot and are not automatically promoted/deleted.
    FILE* destination = std::fopen(temporary, "wbx");
    if (destination == nullptr) { std::fclose(source); return ESP_FAIL; }
    uint8_t buffer[256]; size_t total = 0;
    uint32_t crc = UINT32_MAX; bool ok = true;
    while (ok) {
        const size_t count = std::fread(buffer, 1, sizeof(buffer), source);
        if (count == 0) break;
        if (total + count > kMaxProblemEvidenceBytes) { ok = false; break; }
        total += count;
        crc = esp_rom_crc32_le(crc, buffer, count);
        ok = std::fwrite(buffer, 1, count, destination) == count;
    }
    ok = std::ferror(source) == 0 && ok;
    if (std::fclose(source) != 0) ok = false;
    if (total != proof.bytes || (crc ^ UINT32_MAX) != proof.crc) ok = false;
    ok = ok && std::fflush(destination) == 0 && ::fsync(fileno(destination)) == 0;
    if (std::fclose(destination) != 0) ok = false;
    if (!ok) return ESP_FAIL;  // Leave this attempt's uncertain copy intact.
    esp_err_t result = SameEvidenceSource(temporary, proof);
    if (result != ESP_OK) return result;
    // Recheck the destination before the owner-only rename as well. SPIFFS
    // has one writer; no external task may publish a competing generation.
    bool exists = false; size_t bytes = 0;
    result = EvidenceSlotState(path, &exists, &bytes);
    if (result != ESP_OK || exists) return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    if (std::rename(temporary, path) != 0) return ESP_FAIL;
    result = SameEvidenceSource(path, proof);
    if (result != ESP_OK) return result;
    std::snprintf(selected, capacity, "%s", path);
    ESP_LOGW(kTag, "problem repair preserved: slot=%u source_bytes=%u discarded_frames=%u",
        free_slot, static_cast<unsigned>(proof.bytes), static_cast<unsigned>(proof.discarded_frames));
    return ESP_OK;
}

// Recovery publication remains independent of evidence-slot selection.
esp_err_t RecoverOutboxPublication()
{
    struct stat info = {};
    if (::stat(kOutboxPath, &info) == 0) return ESP_OK;
    if (errno != ENOENT) return ESP_FAIL;
    if (::stat(kOutboxBackupPath, &info) != 0) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    g_empty_problem_outbox_known = false;
    if (std::rename(kOutboxBackupPath, kOutboxPath) != 0) return ESP_FAIL;
    ESP_LOGW(kTag, "recovered problem outbox from interrupted publication");
    return ESP_OK;
}

esp_err_t ReadOutboxLines(
    std::vector<std::string>* lines, bool* physically_empty = nullptr,
    OutboxReadProof* proof = nullptr)
{
    if (lines == nullptr) return ESP_ERR_INVALID_ARG;
    if (physically_empty != nullptr) *physically_empty = false;
    if (proof != nullptr) *proof = {};
    lines->clear();
    const esp_err_t recovery = RecoverOutboxPublication();
    if (recovery != ESP_OK) return recovery;
    FILE* file = std::fopen(kOutboxPath, "rb");
    if (file == nullptr) {
        if (errno == ENOENT && physically_empty != nullptr) *physically_empty = true;
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    std::string line;
    line.reserve(kMaxLineBytes);
    bool saw_bytes = false, discard_line = false, repair = false;
    size_t source_bytes = 0, discarded_frames = 0;
    uint32_t source_crc = UINT32_MAX;
    char chunk[128];
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (source_bytes + read > kMaxProblemEvidenceBytes) {
            std::fclose(file); lines->clear(); return ESP_ERR_INVALID_SIZE;
        }
        source_bytes += read;
        if (proof != nullptr)
            source_crc = esp_rom_crc32_le(source_crc, reinterpret_cast<const uint8_t*>(chunk), read);
        for (size_t index = 0; index < read; ++index) {
            const unsigned char ch = static_cast<unsigned char>(chunk[index]);
            saw_bytes = true;
            if (ch != '\n') {
                if (ch == 0 || line.size() >= kMaxLineBytes + 1) {
                    discard_line = repair = true;
                } else if (!discard_line) {
                    line.push_back(static_cast<char>(ch));
                }
                continue;
            }
            const bool had_cr = !line.empty() && line.back() == '\r';
            if (had_cr) line.pop_back();
            if (!discard_line && !line.empty()) {
                wqn::DurableProblemObservation parsed;
                const auto decoded = line.size() <= kMaxLineBytes
                    ? DecodeObservationLine(line, &parsed) : ObservationDecode::kMalformed;
                if (decoded == ObservationDecode::kUnavailable) {
                    std::fclose(file);
                    lines->clear();
                    return ESP_ERR_NO_MEM;
                }
                if (decoded == ObservationDecode::kValid ||
                    decoded == ObservationDecode::kPreserved) {
                    // Preserve opaque payload bytes and their CRLF framing.
                    if (decoded == ObservationDecode::kPreserved && had_cr) line.push_back('\r');
                    lines->push_back(line);
                } else { repair = true; ++discarded_frames; }
            } else { repair = true; ++discarded_frames; }
            line.clear();
            discard_line = false;
            if (lines->size() > wqn::kProblemObservationOutboxCapacity + 8) {
                std::fclose(file);
                return ESP_ERR_INVALID_SIZE;
            }
        }
    }
    if (discard_line || !line.empty()) { repair = true; ++discarded_frames; }
    const bool read_ok = std::ferror(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!read_ok || !closed) return ESP_FAIL;
    if (physically_empty != nullptr) *physically_empty = !saw_bytes;
    if (proof != nullptr) {
        proof->present = true; proof->needs_repair = repair; proof->bytes = source_bytes;
        proof->crc = source_crc ^ UINT32_MAX; proof->discarded_frames = discarded_frames;
    }
    return ESP_OK;
}

esp_err_t WriteOutboxLines(const std::vector<std::string>& lines, const OutboxReadProof& proof)
{
    g_empty_problem_outbox_known = false;
    const esp_err_t recovery = RecoverOutboxPublication();
    if (recovery != ESP_OK) return recovery;
    char evidence[sizeof(kOutboxPath) + 16] = {};
    if (proof.needs_repair) {
        const esp_err_t retained = PreserveOutboxSource(proof, evidence, sizeof(evidence));
        if (retained != ESP_OK) return retained;
    }
    // An empty file is a committed empty generation. Removing it would allow
    // an interrupted cleanup's backup to resurrect already-acknowledged data.
    FILE* file = std::fopen(kOutboxTempPath, "wb");
    if (file == nullptr) return ESP_FAIL;
    bool ok = true;
    for (const std::string& line : lines) {
        ok = ok && std::fwrite(line.data(), 1, line.size(), file) == line.size() &&
            std::fputc('\n', file) != EOF;
    }
    ok = ok && std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) {
        std::remove(kOutboxTempPath);
        return ESP_FAIL;
    }
    if (proof.needs_repair) {
        const esp_err_t retained = SameEvidenceSource(evidence, proof);
        if (retained != ESP_OK) {
            std::remove(kOutboxTempPath);
            return retained;
        }
    }
    struct stat info = {};
    const bool had_primary = ::stat(kOutboxPath, &info) == 0;
    if (!had_primary && errno != ENOENT) {
        std::remove(kOutboxTempPath);
        return ESP_FAIL;
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
    if (std::remove(kOutboxBackupPath) != 0 && errno != ENOENT) {
        ESP_LOGW(kTag, "problem outbox published; backup cleanup deferred");
    }
    return ESP_OK;
}

// Confirm an uncertain in-boot append against the complete validated read.
// CRC detects drift during this confirmation; it is not an on-disk checksum
// or authentication for legacy park markers. No create/truncate is allowed.
esp_err_t SyncParkedJournal(size_t expected_bytes, uint32_t expected_crc)
{
    FILE* file = std::fopen(kParkedPath, "r+b");
    if (file == nullptr) return ESP_FAIL;
    // Confirmation must reread storage after fsync, not a stdio read buffer.
    const bool unbuffered = std::setvbuf(file, nullptr, _IONBF, 0) == 0;
    const auto matches = [&]() {
        uint8_t buffer[128];
        size_t total = 0;
        uint32_t crc = UINT32_MAX;
        size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
            if (count > kMaxParkedJournalBytes - total) return false;
            total += count;
            crc = esp_rom_crc32_le(crc, buffer, count);
        }
        return std::ferror(file) == 0 && total == expected_bytes &&
            (crc ^ UINT32_MAX) == expected_crc;
    };
    const bool confirmed = unbuffered && matches() &&
        ::fsync(fileno(file)) == 0 && std::fseek(file, 0, SEEK_SET) == 0 &&
        matches();
    const bool closed = std::fclose(file) == 0;
    return confirmed && closed ? ESP_OK : ESP_FAIL;
}

// Validate every physical frame before publishing any parked identity. Legacy
// LF, CRLF and complete EOF records remain supported; a torn/unknown frame
// holds upload admission rather than silently reviving a suspended verdict.
esp_err_t ReadParkedRequestIds(
    std::vector<std::string>* request_ids, size_t* physical_records = nullptr)
{
    if (request_ids == nullptr) return ESP_ERR_INVALID_ARG;
    request_ids->clear();
    if (physical_records != nullptr) *physical_records = 0;
    bool exists = false;
    size_t stored_bytes = 0;
    esp_err_t result = EvidenceSlotState(kParkedPath, &exists, &stored_bytes);
    if (result != ESP_OK) return result;
    if (stored_bytes > kMaxParkedJournalBytes) return ESP_ERR_INVALID_SIZE;
    FILE* file = std::fopen(kParkedPath, "rb");
    if (file == nullptr) {
        if (errno != ENOENT) return ESP_FAIL;
        return !exists && !g_problem_park_sync_pending
            ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    size_t records = 0, bytes = 0;
    uint32_t crc = UINT32_MAX;
    while (result == ESP_OK) {
        std::string line;
        bool saw_byte = false;
        int ch = 0;
        while ((ch = std::fgetc(file)) != EOF) {
            saw_byte = true;
            if (++bytes > kMaxParkedJournalBytes) {
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (g_problem_park_sync_pending) {
                const uint8_t byte = static_cast<uint8_t>(ch);
                crc = esp_rom_crc32_le(crc, &byte, 1);
            }
            if (ch == '\n') break; // park frame boundary
            if (line.size() >= kMaxParkedLineBytes + 1) {
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            line.push_back(static_cast<char>(ch));
        }
        if (result != ESP_OK || (!saw_byte && ch == EOF)) break;
        if (++records > kMaxParkedRecords) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (ch == '\n' && !line.empty() && line.back() == '\r')
            line.pop_back();
        std::string request_id;
        if (!DecodeParkedLine(line, &request_id)) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (std::find(request_ids->begin(), request_ids->end(), request_id) ==
            request_ids->end()) request_ids->push_back(request_id);
        if (ch == EOF) break;
    }
    const bool read_ok = std::ferror(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!read_ok || !closed) result = ESP_FAIL;
    if (result == ESP_OK && bytes != stored_bytes) result = ESP_ERR_INVALID_STATE;
    if (result == ESP_OK && g_problem_park_sync_pending) {
        result = SyncParkedJournal(bytes, crc ^ UINT32_MAX);
        if (result == ESP_OK) g_problem_park_sync_pending = false;
    }
    if (result != ESP_OK) {
        request_ids->clear();
        ESP_LOGW(kTag, "problem park journal unavailable: bytes=%u frames=%u",
                 static_cast<unsigned>(bytes), static_cast<unsigned>(records));
        return result;
    }
    if (physical_records != nullptr) *physical_records = records;
    return ESP_OK;
}

// Bounded durable append of one park marker. Returns ESP_ERR_NO_MEM when
// the journal is full so the caller can fail the park transaction instead of
// silently losing the marker (the verdict then keeps its queue place).
esp_err_t AppendParkedEntry(
    const std::string& request_id, wqn::OutboxSuspendReason reason)
{
    g_empty_problem_outbox_known = false;
    const std::string reason_name = wqn::OutboxSuspendReasonName(reason);
    if (!IsValidRequestId(request_id) || !IsValidSuspendReasonName(reason_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    std::vector<std::string> parked;
    size_t physical_records = 0;
    const esp_err_t read_result = ReadParkedRequestIds(&parked, &physical_records);
    if (read_result != ESP_OK) return read_result;
    if (std::find(parked.begin(), parked.end(), request_id) != parked.end()) {
        return ESP_OK;
    }
    if (physical_records >= kMaxParkedRecords) {
        return ESP_ERR_NO_MEM;
    }
    bool needs_separator = false;
    FILE* probe = std::fopen(kParkedPath, "rb");
    if (probe != nullptr) {
        if (std::fseek(probe, 0, SEEK_END) != 0) {
            std::fclose(probe);
            return ESP_FAIL;
        }
        const long size = std::ftell(probe);
        if (size < 0) {
            std::fclose(probe);
            return ESP_FAIL;
        }
        if (size > 0) {
            if (std::fseek(probe, -1, SEEK_END) != 0) {
                std::fclose(probe);
                return ESP_FAIL;
            }
            const int last = std::fgetc(probe);
            if (last == EOF && std::ferror(probe)) {
                std::fclose(probe);
                return ESP_FAIL;
            }
            needs_separator = last != '\n';
        }
        if (std::fclose(probe) != 0) return ESP_FAIL; // park separator
    } else if (errno != ENOENT) {
        return ESP_FAIL;
    }
    FILE* journal = std::fopen(kParkedPath, "ab");
    if (journal == nullptr) return ESP_FAIL;
    g_problem_park_sync_pending = true;
    const std::string entry = request_id + ' ' + reason_name;
    const bool separated = !needs_separator || std::fputc('\n', journal) != EOF;
    const bool ok = separated &&
        std::fwrite(entry.data(), 1, entry.size(), journal) ==
            entry.size() &&
        std::fputc('\n', journal) != EOF && std::fflush(journal) == 0 &&
        ::fsync(fileno(journal)) == 0;
    if (std::fclose(journal) != 0 || !ok) return ESP_FAIL;
    g_problem_park_sync_pending = false;
    return ESP_OK;
}

esp_err_t ReadOutboxForQuery(
    std::vector<std::string>* lines, std::vector<std::string>* parked)
{
    if (lines == nullptr || parked == nullptr) return ESP_ERR_INVALID_ARG;
    lines->clear();
    parked->clear();
    if (g_empty_problem_outbox_known) return ESP_OK;
    bool physically_empty = false;
    esp_err_t result = ReadOutboxLines(lines, &physically_empty);
    if (result != ESP_OK) return result;
    result = ReadParkedRequestIds(parked);
    if (result != ESP_OK) {
        lines->clear();
        parked->clear();
        return result;
    }
    g_empty_problem_outbox_known = physically_empty;
    ESP_LOGI(
        kTag, "problem outbox query read: lines=%u parked=%u empty_cached=%u",
        static_cast<unsigned>(lines->size()),
        static_cast<unsigned>(parked->size()),
        static_cast<unsigned>(g_empty_problem_outbox_known));
    return ESP_OK;
}

struct CommitContext {
    const wqn::DurableProblemObservation* observation;
};

esp_err_t CommitTransaction(void* opaque)
{
    auto* context = static_cast<CommitContext*>(opaque);
    // Also invalidate if a preparatory read fails: a later query must not
    // hide that storage error behind the older successful empty probe.
    g_empty_problem_outbox_known = false;
    std::vector<std::string> lines;
    OutboxReadProof proof;
    esp_err_t result = ReadOutboxLines(&lines, nullptr, &proof);
    if (result != ESP_OK) return result;
    for (const std::string& line : lines) {
        wqn::DurableProblemObservation parsed;
        const auto decoded = DecodeObservationLine(line, &parsed);
        if (decoded == ObservationDecode::kUnavailable) return ESP_ERR_NO_MEM;
        if (decoded == ObservationDecode::kValid &&
            parsed.request_id == context->observation->request_id) {
            return parsed.problem_id == context->observation->problem_id &&
                parsed.action == context->observation->action &&
                parsed.occurred_at == context->observation->occurred_at
                ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
    }
    if (lines.size() >= wqn::kProblemObservationOutboxCapacity) {
        return ESP_ERR_NO_MEM;
    }
    const std::string encoded = EncodeObservationLine(*context->observation);
    if (encoded.empty()) return ESP_ERR_NO_MEM;
    if (encoded.size() > kMaxLineBytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (proof.needs_repair) {
        result = WriteOutboxLines(lines, proof);
        if (result != ESP_OK) return result;
    }
    FILE* file = std::fopen(kOutboxPath, "ab");
    if (file == nullptr) {
        return ESP_FAIL;
    }
    const bool ok =
        std::fwrite(encoded.data(), 1, encoded.size(), file) == encoded.size() &&
        std::fputc('\n', file) != EOF && std::fflush(file) == 0 &&
        ::fsync(fileno(file)) == 0;
    if (std::fclose(file) != 0 || !ok) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t PeekTransaction(void* opaque)
{
    auto* observation = static_cast<wqn::DurableProblemObservation*>(opaque);
    std::vector<std::string> lines;
    std::vector<std::string> parked;
    const esp_err_t result = ReadOutboxForQuery(&lines, &parked);
    if (result != ESP_OK) return result;
    for (const std::string& line : lines) {
        const auto decoded = DecodeObservationLine(line, observation);
        if (decoded == ObservationDecode::kUnavailable) return ESP_ERR_NO_MEM;
        // Preserved records are visible in the suspended count. Do not flood
        // the serial log once per retained record on every queue probe.
        if (decoded != ObservationDecode::kValid) continue;
        const bool is_parked = std::find(
            parked.begin(), parked.end(), observation->request_id) !=
            parked.end();
        if (is_parked) {
            continue;
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

struct RequestIdContext {
    const std::string* request_id;
    bool quarantine = false;
};

esp_err_t RemoveTransaction(void* opaque)
{
    auto* context = static_cast<RequestIdContext*>(opaque);
    g_empty_problem_outbox_known = false;
    std::vector<std::string> parked;
    esp_err_t result = ReadParkedRequestIds(&parked); // ACK admission
    if (result != ESP_OK) return result;
    std::vector<std::string> lines;
    OutboxReadProof proof;
    result = ReadOutboxLines(&lines, nullptr, &proof);
    if (result != ESP_OK) return result;
    std::vector<std::string> retained;
    retained.reserve(lines.size());
    std::string removed;
    for (const std::string& line : lines) {
        wqn::DurableProblemObservation parsed;
        const auto decoded = DecodeObservationLine(line, &parsed);
        if (decoded == ObservationDecode::kUnavailable) return ESP_ERR_NO_MEM;
        if (decoded == ObservationDecode::kPreserved) {
            retained.push_back(line);
            continue;
        }
        if (decoded != ObservationDecode::kValid) {
            // Malformed lines are dropped as part of any rewrite.
            continue;
        }
        if (removed.empty() && parsed.request_id == *context->request_id) {
            removed = line;
            continue;
        }
        retained.push_back(line);
    }
    if (context->quarantine && !removed.empty()) {
        // Bounded forensic journal: count lines cheaply, then best-effort
        // append -- a full journal only loses forensics, never durability.
        std::vector<std::string> rejected;
        FILE* probe = std::fopen(kRejectedPath, "rb");
        size_t rejected_count = 0;
        if (probe != nullptr) {
            int ch = 0;
            while ((ch = std::fgetc(probe)) != EOF) {
                if (ch == '\n') ++rejected_count;
            }
            std::fclose(probe);
        }
        if (rejected_count < kMaxRejectedRecords) {
            FILE* journal = std::fopen(kRejectedPath, "ab");
            if (journal != nullptr) {
                std::fwrite(removed.data(), 1, removed.size(), journal);
                std::fputc('\n', journal);
                std::fclose(journal);
            }
        }
    }
    return WriteOutboxLines(retained, proof);
}

esp_err_t SnapshotTransaction(void* opaque)
{
    auto* snapshot = static_cast<wqn::ProblemOutboxSnapshot*>(opaque);
    std::vector<std::string> lines;
    std::vector<std::string> parked;
    const esp_err_t result = ReadOutboxForQuery(&lines, &parked);
    if (result != ESP_OK) return result;
    size_t valid = 0;
    size_t suspended = 0;
    for (const std::string& line : lines) {
        wqn::DurableProblemObservation parsed;
        const auto decoded = DecodeObservationLine(line, &parsed);
        if (decoded == ObservationDecode::kUnavailable) return ESP_ERR_NO_MEM;
        if (decoded == ObservationDecode::kPreserved) {
            ++suspended;
            continue;
        }
        if (decoded != ObservationDecode::kValid) continue;
        const bool is_parked =
            std::find(parked.begin(), parked.end(), parsed.request_id) !=
            parked.end();
        if (is_parked) {
            ++suspended;
            continue;
        }
        ++valid;
    }
    snapshot->pending_count = valid;
    snapshot->suspended_count = suspended;
    snapshot->capacity = wqn::kProblemObservationOutboxCapacity;
    return ESP_OK;
}

struct ParkContext {
    const std::string* request_id;
    wqn::OutboxSuspendReason reason;
};

esp_err_t SuspendTransaction(void* opaque)
{
    auto* context = static_cast<ParkContext*>(opaque);
    if (context == nullptr || context->request_id == nullptr ||
        context->request_id->empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    g_empty_problem_outbox_known = false;
    std::vector<std::string> parked;
    esp_err_t result = ReadParkedRequestIds(&parked);
    if (result != ESP_OK) return result;
    if (std::find(parked.begin(), parked.end(), *context->request_id) !=
        parked.end()) {
        // Idempotent replay of an earlier park.
        return ESP_OK;
    }
    std::vector<std::string> lines;
    result = ReadOutboxLines(&lines);
    if (result != ESP_OK) return result;
    bool exists = false;
    for (const std::string& line : lines) {
        wqn::DurableProblemObservation parsed;
        const auto decoded = DecodeObservationLine(line, &parsed);
        if (decoded == ObservationDecode::kUnavailable) return ESP_ERR_NO_MEM;
        if (decoded == ObservationDecode::kValid &&
            parsed.request_id == *context->request_id) {
            exists = true;
            break;
        }
    }
    if (!exists) return ESP_ERR_NOT_FOUND;
    // Single durable write: the payload stays in po_outbox.jsonl untouched,
    // only the skip-marker journal gains an entry.
    return AppendParkedEntry(*context->request_id, context->reason);
}

esp_err_t ExecuteWithStorageLease(
    const char* label, esp_err_t (*transaction)(void*), void* context,
    bool foreground = false)
{
    wqn::runtime::SleepLease storage_lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kStorage, label, __FILE__, __LINE__);
    if (!storage_lease) {
        return ESP_ERR_INVALID_STATE;
    }
    // [measure] Pass `label` on the background path too. word_study_store.cpp
    // and note_store.cpp both forward their holder here; this one did not, so
    // every background problem transaction was logged with the default owner
    // "background" and the §一 table could not tell problem traffic apart.
    return foreground
        ? wqn::services::ExecuteForegroundStorageTransaction(transaction, context, label)
        : wqn::services::ExecuteStorageTransactionNamed(transaction, context, label);
}

}  // namespace

namespace wqn {

esp_err_t CommitProblemObservation(const DurableProblemObservation& observation)
{
    if (!IsValidObservation(observation)) {
        return ESP_ERR_INVALID_ARG;
    }
    CommitContext context{&observation};
    // Foreground: bounds the QUEUE wait behind background pack writes for the
    // persist worker; once the transaction has started it may still wait
    // without a fixed deadline (storage_service degrades to portMAX_DELAY).
    return ExecuteWithStorageLease(
        "problem-observation-commit", CommitTransaction, &context, true);
}

esp_err_t PeekPendingProblemObservation(DurableProblemObservation* observation)
{
    if (observation == nullptr) return ESP_ERR_INVALID_ARG;
    *observation = {};
    return ExecuteWithStorageLease(
        "problem-outbox-peek", PeekTransaction, observation);
}

esp_err_t AcknowledgeProblemObservation(const std::string& request_id)
{
    if (request_id.empty()) return ESP_ERR_INVALID_ARG;
    RequestIdContext context{&request_id, false};
    return ExecuteWithStorageLease(
        "problem-outbox-ack", RemoveTransaction, &context);
}

esp_err_t QuarantinePendingProblemObservation(const std::string& request_id)
{
    if (request_id.empty()) return ESP_ERR_INVALID_ARG;
    RequestIdContext context{&request_id, true};
    return ExecuteWithStorageLease(
        "problem-outbox-quarantine", RemoveTransaction, &context);
}

esp_err_t SuspendPendingProblemObservation(
    const std::string& request_id,
    OutboxSuspendReason reason)
{
    if (request_id.empty()) return ESP_ERR_INVALID_ARG;
    ParkContext context{&request_id, reason};
    return ExecuteWithStorageLease(
        "problem-outbox-suspend", SuspendTransaction, &context);
}

esp_err_t ReadProblemOutboxSnapshot(ProblemOutboxSnapshot* snapshot)
{
    if (snapshot == nullptr) return ESP_ERR_INVALID_ARG;
    *snapshot = {};
    return ExecuteWithStorageLease(
        "problem-outbox-snapshot", SnapshotTransaction, snapshot);
}

}  // namespace wqn
