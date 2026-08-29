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
#include "esp_log.h"
#include "runtime/sleep_coordinator.h"
#include "services/storage_service.h"

namespace {

constexpr char kTag[] = "problem_store";
constexpr char kOutboxPath[] = "/storage/po_outbox.jsonl";
constexpr char kOutboxTempPath[] = "/storage/po_outbox.tmp";
constexpr char kRejectedPath[] = "/storage/po_rejected.jsonl";
// Park markers (request_id + reason) for verdicts that must not be retried
// nor deleted. The payload line stays in po_outbox.jsonl; Peek consults this
// journal so a parked verdict neither uploads nor wedges the queue. Old
// firmware builds ignore this file entirely: on downgrade they would retry
// the parked verdicts (server rejects them again) rather than lose data.
constexpr char kParkedPath[] = "/storage/po_parked.jsonl";
constexpr size_t kMaxRejectedRecords = 50;
constexpr size_t kMaxParkedRecords = wqn::kProblemObservationOutboxCapacity;
constexpr size_t kMaxLineBytes = 512;
constexpr size_t kMaxParkedLineBytes = 96;

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
        observation.occurred_at.size() <= 40;
}

std::string EncodeObservationLine(const wqn::DurableProblemObservation& observation)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return std::string();
    cJSON_AddStringToObject(root, "request_id", observation.request_id.c_str());
    cJSON_AddStringToObject(root, "problem_id", observation.problem_id.c_str());
    cJSON_AddStringToObject(
        root, "action",
        wqn::protocol::problem_study_v1::ReviewActionName(observation.action));
    cJSON_AddStringToObject(root, "occurred_at", observation.occurred_at.c_str());
    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == nullptr) return std::string();
    std::string line = rendered;
    cJSON_free(rendered);
    return line;
}

bool DecodeObservationLine(
    const std::string& line, wqn::DurableProblemObservation* observation)
{
    if (observation == nullptr) return false;
    *observation = {};
    cJSON* root = cJSON_Parse(line.c_str());
    if (root == nullptr) return false;
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
    return ok && IsValidObservation(*observation);
}

// Reads every journal line (bounded); malformed lines are dropped so one
// corrupt record cannot wedge the queue forever.
esp_err_t ReadOutboxLines(std::vector<std::string>* lines)
{
    lines->clear();
    FILE* file = std::fopen(kOutboxPath, "rb");
    if (file == nullptr) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    std::vector<char> buffer(kMaxLineBytes + 2, 0);
    while (std::fgets(buffer.data(), buffer.size(), file) != nullptr) {
        size_t length = std::strlen(buffer.data());
        while (length > 0 &&
               (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
            --length;
        }
        if (length == 0) continue;
        if (length > kMaxLineBytes) {
            ESP_LOGW(kTag, "dropping overlong problem outbox line");
            continue;
        }
        lines->emplace_back(buffer.data(), length);
        if (lines->size() > wqn::kProblemObservationOutboxCapacity + 8) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    std::fclose(file);
    return ESP_OK;
}

esp_err_t WriteOutboxLines(const std::vector<std::string>& lines)
{
    if (lines.empty()) {
        if (std::remove(kOutboxPath) != 0 && errno != ENOENT) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    FILE* file = std::fopen(kOutboxTempPath, "wb");
    if (file == nullptr) {
        return ESP_FAIL;
    }
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
    if (std::remove(kOutboxPath) != 0 && errno != ENOENT) {
        std::remove(kOutboxTempPath);
        return ESP_FAIL;
    }
    if (std::rename(kOutboxTempPath, kOutboxPath) != 0) {
        std::remove(kOutboxTempPath);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Reads the parked request_ids from the park journal (bounded). A missing
// file means nothing is parked.
esp_err_t ReadParkedRequestIds(std::vector<std::string>* request_ids)
{
    if (request_ids == nullptr) return ESP_ERR_INVALID_ARG;
    request_ids->clear();
    FILE* file = std::fopen(kParkedPath, "rb");
    if (file == nullptr) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    while (true) {
        std::string line;
        bool overlong = false;
        bool saw_byte = false;
        int ch = 0;
        while ((ch = std::fgetc(file)) != EOF) {
            saw_byte = true;
            if (ch == '\n') break;
            if (ch == '\r') continue;
            if (line.size() < kMaxParkedLineBytes) {
                line.push_back(static_cast<char>(ch));
            } else {
                overlong = true;
            }
        }
        if (!saw_byte && ch == EOF) break;
        std::string request_id;
        if (!overlong && DecodeParkedLine(line, &request_id) &&
            std::find(request_ids->begin(), request_ids->end(), request_id) ==
                request_ids->end()) {
            // A valid EOF-terminated final record is accepted. An incomplete
            // crash tail fails DecodeParkedLine and is ignored.
            request_ids->push_back(request_id);
        } else if (!line.empty()) {
            ESP_LOGW(kTag, "ignoring malformed problem park marker");
        }
        if (request_ids->size() > kMaxParkedRecords) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
        if (ch == EOF) break;
    }
    std::fclose(file);
    return ESP_OK;
}

// Best-effort bounded append of one park marker. Returns ESP_ERR_NO_MEM when
// the journal is full so the caller can fail the park transaction instead of
// silently losing the marker (the verdict then keeps its queue place).
esp_err_t AppendParkedEntry(
    const std::string& request_id, wqn::OutboxSuspendReason reason)
{
    const std::string reason_name = wqn::OutboxSuspendReasonName(reason);
    if (!IsValidRequestId(request_id) || !IsValidSuspendReasonName(reason_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    std::vector<std::string> parked;
    const esp_err_t read_result = ReadParkedRequestIds(&parked);
    if (read_result != ESP_OK) return read_result;
    if (std::find(parked.begin(), parked.end(), request_id) != parked.end()) {
        return ESP_OK;
    }
    if (parked.size() >= kMaxParkedRecords) {
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
        std::fclose(probe);
    } else if (errno != ENOENT) {
        return ESP_FAIL;
    }
    FILE* journal = std::fopen(kParkedPath, "ab");
    if (journal == nullptr) return ESP_FAIL;
    const std::string entry = request_id + ' ' + reason_name;
    const bool separated = !needs_separator || std::fputc('\n', journal) != EOF;
    const bool ok = separated &&
        std::fwrite(entry.data(), 1, entry.size(), journal) ==
            entry.size() &&
        std::fputc('\n', journal) != EOF && std::fflush(journal) == 0 &&
        ::fsync(fileno(journal)) == 0;
    if (std::fclose(journal) != 0 || !ok) return ESP_FAIL;
    return ESP_OK;
}

struct CommitContext {
    const wqn::DurableProblemObservation* observation;
};

esp_err_t CommitTransaction(void* opaque)
{
    auto* context = static_cast<CommitContext*>(opaque);
    std::vector<std::string> lines;
    esp_err_t result = ReadOutboxLines(&lines);
    if (result != ESP_OK) return result;
    for (const std::string& line : lines) {
        // Idempotent by request_id: a retried commit is already durable.
        if (line.find(context->observation->request_id) != std::string::npos) {
            return ESP_OK;
        }
    }
    if (lines.size() >= wqn::kProblemObservationOutboxCapacity) {
        return ESP_ERR_NO_MEM;
    }
    const std::string encoded = EncodeObservationLine(*context->observation);
    if (encoded.empty() || encoded.size() > kMaxLineBytes) {
        return ESP_ERR_INVALID_SIZE;
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
    esp_err_t result = ReadOutboxLines(&lines);
    if (result != ESP_OK) return result;
    std::vector<std::string> parked;
    result = ReadParkedRequestIds(&parked);
    if (result != ESP_OK) return result;
    for (const std::string& line : lines) {
        if (!DecodeObservationLine(line, observation)) {
            ESP_LOGW(kTag, "skipping malformed problem outbox head");
            continue;
        }
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
    std::vector<std::string> lines;
    esp_err_t result = ReadOutboxLines(&lines);
    if (result != ESP_OK) return result;
    std::vector<std::string> retained;
    retained.reserve(lines.size());
    std::string removed;
    for (const std::string& line : lines) {
        wqn::DurableProblemObservation parsed;
        if (!DecodeObservationLine(line, &parsed)) {
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
    return WriteOutboxLines(retained);
}

esp_err_t SnapshotTransaction(void* opaque)
{
    auto* snapshot = static_cast<wqn::ProblemOutboxSnapshot*>(opaque);
    std::vector<std::string> lines;
    esp_err_t result = ReadOutboxLines(&lines);
    if (result != ESP_OK) return result;
    std::vector<std::string> parked;
    result = ReadParkedRequestIds(&parked);
    if (result != ESP_OK) return result;
    size_t valid = 0;
    size_t suspended = 0;
    for (const std::string& line : lines) {
        wqn::DurableProblemObservation parsed;
        if (!DecodeObservationLine(line, &parsed)) continue;
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
        if (DecodeObservationLine(line, &parsed) &&
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
    return foreground
        ? wqn::services::ExecuteForegroundStorageTransaction(transaction, context, label)
        : wqn::services::ExecuteStorageTransaction(transaction, context);
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
