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
#include "esp_log.h"
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
constexpr size_t kMaxParkedRecords = wqn::kProblemObservationOutboxCapacity;
constexpr size_t kMaxLineBytes = 512;
constexpr size_t kMaxParkedLineBytes = 96;

// Owner-task-only negative cache, not a count of uploadable records. Publish
// only after successful reads of both journals find no payload bytes. Parked
// or malformed nonempty payloads must keep the ordinary read/validation path.
// Every payload/marker write primitive invalidates this BEFORE attempting I/O,
// including failures that may already have appended partial bytes.
bool g_empty_problem_outbox_known = false;

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

enum class ObservationDecode { kValid, kMalformed, kUnavailable };

// cJSON reports the same null result for malformed JSON and allocation failure.
// Only run this allocation-free syntax probe after failure: a complete document
// may have failed for lack of memory and must never be dropped during repair.
// Fixed container states bound stack use; no recursion or global allocator hooks.
// Match cJSON's whitespace, BOM, raw string bytes and permissive number forms.
class ObservationJsonSyntax {
public:
    explicit ObservationJsonSyntax(const std::string& text) : text_(text) {}
    bool Complete()
    {
        enum State : unsigned char {
            kValue, kDone, kArrayFirst, kArrayNext, kArrayAfter,
            kObjectFirst, kObjectNext, kObjectColon, kObjectAfter,
        };
        State states[wqn::protocol::kMaxJsonNestingDepth + 1] = {kValue};
        size_t depth = 0;
        if (text_.compare(0, 3, "\xef\xbb\xbf") == 0) position_ = 3;
        while (true) {
            while (position_ < text_.size() &&
                   static_cast<unsigned char>(text_[position_]) <= 32) ++position_;
            const char ch = position_ < text_.size() ? text_[position_] : '\0';
            State& state = states[depth];
            if (state == kDone) return position_ == text_.size();
            if ((state == kArrayFirst && ch == ']') ||
                (state == kObjectFirst && ch == '}')) {
                ++position_; --depth; continue;
            }
            if (state == kArrayAfter || state == kObjectAfter) {
                if (ch == (state == kArrayAfter ? ']' : '}')) {
                    ++position_; --depth; continue;
                }
                if (ch != ',') return false;
                state = state == kArrayAfter ? kArrayNext : kObjectNext;
                ++position_; continue;
            }
            if (state == kObjectFirst || state == kObjectNext) {
                if (!String()) return false;
                state = kObjectColon; continue;
            }
            if (state == kObjectColon) {
                if (ch != ':') return false;
                ++position_; state = kValue; continue;
            }
            state = depth == 0 ? kDone :
                (state == kArrayFirst || state == kArrayNext ? kArrayAfter : kObjectAfter);
            if (ch == '[' || ch == '{') {
                if (depth == wqn::protocol::kMaxJsonNestingDepth) return false;
                states[++depth] = ch == '[' ? kArrayFirst : kObjectFirst;
                ++position_;
            } else if (ch == '"') {
                if (!String()) return false;
            } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
                if (!Number()) return false;
            } else if (text_.compare(position_, 4, "null") == 0 ||
                       text_.compare(position_, 4, "true") == 0) {
                position_ += 4;
            } else if (text_.compare(position_, 5, "false") == 0) {
                position_ += 5;
            } else return false;
        }
    }
private:
    bool String()
    {
        if (position_ >= text_.size() || text_[position_++] != '"') return false;
        size_t end = position_;
        while (end < text_.size() && text_[end] != '"') {
            if (text_[end] == '\\') ++end;
            ++end;
        }
        if (end >= text_.size()) return false;
        while (position_ < end) {
            const char ch = text_[position_++];
            if (ch != '\\') continue;
            if (position_ == text_.size()) return false;
            const char escaped = text_[position_++];
            if (escaped == 'u') {
                unsigned first = 0;
                if (!Unicode(&first, end) || (first >= 0xdc00 && first <= 0xdfff)) return false;
                if (first >= 0xd800 && first <= 0xdbff) {
                    if (text_.compare(position_, 2, "\\u") != 0) return false;
                    position_ += 2;
                    unsigned second = 0;
                    if (!Unicode(&second, end) || second < 0xdc00 || second > 0xdfff) return false;
                }
            } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                       escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                       escaped != 'r' && escaped != 't') return false;
        }
        position_ = end + 1;
        return true;
    }
    bool Unicode(unsigned* value, size_t end)
    {
        if (position_ > end || end - position_ < 4) return false;
        bool hex = true;
        for (unsigned i = 0; i < 4; ++i) {
            const unsigned char ch = text_[position_++];
            *value <<= 4;
            if (ch >= '0' && ch <= '9') *value += ch - '0';
            else if (ch >= 'a' && ch <= 'f') *value += ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') *value += ch - 'A' + 10;
            else hex = false;
        }
        // cJSON's parse_hex4 maps non-hex sequences to zero too. Keeping that
        // compatibility prevents discarding an older readable record on OOM.
        if (!hex) *value = 0;
        return true;
    }
    bool Number()
    {
        if (text_[position_] == '-') ++position_;
        const size_t integer = position_;
        while (Digit()) ++position_;
        bool digits = position_ != integer;
        if (position_ < text_.size() && text_[position_] == '.') {
            const size_t fraction = ++position_;
            while (Digit()) ++position_;
            digits = digits || position_ != fraction;
        }
        if (!digits) return false;
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const size_t exponent = position_;
            while (Digit()) ++position_;
            if (position_ == exponent) return false;
        }
        return true;
    }
    bool Digit() const
    {
        return position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9';
    }
    const std::string& text_;
    size_t position_ = 0;
};

ObservationDecode DecodeObservationLine(
    const std::string& line, wqn::DurableProblemObservation* observation)
{
    if (observation == nullptr || line.find('\0') != std::string::npos ||
        !wqn::protocol::JsonNestingWithinLimit(line.data(), line.size())) {
        return ObservationDecode::kMalformed;
    }
    *observation = {};
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

// Reads every journal line (bounded); malformed lines are dropped so one
// corrupt record cannot wedge the queue forever. Only newline-framed, fully
// decoded records are eligible for idempotence or a later rewrite.
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
    bool* needs_repair = nullptr)
{
    if (lines == nullptr) return ESP_ERR_INVALID_ARG;
    if (physically_empty != nullptr) *physically_empty = false;
    if (needs_repair != nullptr) *needs_repair = false;
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
    char chunk[128];
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
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
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!discard_line && !line.empty()) {
                wqn::DurableProblemObservation parsed;
                const auto decoded = line.size() <= kMaxLineBytes
                    ? DecodeObservationLine(line, &parsed) : ObservationDecode::kMalformed;
                if (decoded == ObservationDecode::kUnavailable) {
                    std::fclose(file);
                    lines->clear();
                    return ESP_ERR_NO_MEM;
                }
                if (decoded == ObservationDecode::kValid) lines->push_back(line);
                else repair = true;
            }
            line.clear();
            discard_line = false;
            if (lines->size() > wqn::kProblemObservationOutboxCapacity + 8) {
                std::fclose(file);
                return ESP_ERR_INVALID_SIZE;
            }
        }
    }
    repair = repair || discard_line || !line.empty();
    const bool read_ok = std::ferror(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!read_ok || !closed) return ESP_FAIL;
    if (physically_empty != nullptr) *physically_empty = !saw_bytes;
    if (needs_repair != nullptr) *needs_repair = repair;
    return ESP_OK;
}

esp_err_t WriteOutboxLines(const std::vector<std::string>& lines)
{
    g_empty_problem_outbox_known = false;
    const esp_err_t recovery = RecoverOutboxPublication();
    if (recovery != ESP_OK) return recovery;
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
    const bool read_ok = std::ferror(file) == 0;
    const bool closed = std::fclose(file) == 0;
    return read_ok && closed ? ESP_OK : ESP_FAIL;
}

// Best-effort bounded append of one park marker. Returns ESP_ERR_NO_MEM when
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
    if (result != ESP_OK) return result;
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
    bool needs_repair = false;
    esp_err_t result = ReadOutboxLines(&lines, nullptr, &needs_repair);
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
    if (needs_repair) {
        result = WriteOutboxLines(lines);
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
        if (decoded != ObservationDecode::kValid) {
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
    g_empty_problem_outbox_known = false;
    std::vector<std::string> lines;
    esp_err_t result = ReadOutboxLines(&lines);
    if (result != ESP_OK) return result;
    std::vector<std::string> retained;
    retained.reserve(lines.size());
    std::string removed;
    for (const std::string& line : lines) {
        wqn::DurableProblemObservation parsed;
        const auto decoded = DecodeObservationLine(line, &parsed);
        if (decoded == ObservationDecode::kUnavailable) return ESP_ERR_NO_MEM;
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
    return WriteOutboxLines(retained);
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
