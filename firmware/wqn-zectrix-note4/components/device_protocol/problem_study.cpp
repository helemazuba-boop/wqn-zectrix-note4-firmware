#include "device_protocol/problem_study.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cJSON.h"
#include "esp_check.h"

namespace {

using namespace wqn::protocol;
using namespace wqn::protocol::problem_study_v1;

class JsonDocument {
public:
    explicit JsonDocument(cJSON* root) : root_(root) {}
    explicit JsonDocument(const std::string& body) : root_(cJSON_Parse(body.c_str())) {}
    ~JsonDocument() { cJSON_Delete(root_); }
    cJSON* root() const { return root_; }

private:
    cJSON* root_ = nullptr;
};

bool IsUrlSafe(const std::string& value, size_t minimum, size_t maximum)
{
    if (value.size() < minimum || value.size() > maximum) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

std::string StringField(cJSON* object, const char* key)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring != nullptr
        ? value->valuestring
        : "";
}

bool U64Field(cJSON* object, const char* key, uint64_t* output)
{
    if (output == nullptr) {
        return false;
    }
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < 0 ||
        value->valuedouble > static_cast<double>(v3::kMaxSafeJsonInteger) ||
        std::floor(value->valuedouble) != value->valuedouble) {
        return false;
    }
    *output = static_cast<uint64_t>(value->valuedouble);
    return true;
}

bool ParseSafeDecimal(const std::string& value, uint64_t* output)
{
    if (output == nullptr || value.empty() || value.size() > 20) {
        return false;
    }
    uint64_t parsed = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (parsed > (v3::kMaxSafeJsonInteger - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *output = parsed;
    return true;
}

bool IsUuid(const std::string& value)
{
    if (value.size() != 36) {
        return false;
    }
    for (size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') ||
                     (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool IsSha256(const std::string& value)
{
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

bool ParseAction(const std::string& value, ReviewAction* action)
{
    if (action == nullptr) return false;
    if (value == "correct") *action = ReviewAction::kCorrect;
    else if (value == "hesitant") *action = ReviewAction::kHesitant;
    else if (value == "wrong") *action = ReviewAction::kWrong;
    else if (value == "skip") *action = ReviewAction::kSkip;
    else return false;
    return true;
}

esp_err_t Render(cJSON* root, std::string* body)
{
    if (root == nullptr || body == nullptr) return ESP_ERR_INVALID_ARG;
    char* rendered = cJSON_PrintUnformatted(root);
    if (rendered == nullptr) return ESP_ERR_NO_MEM;
    *body = rendered;
    cJSON_free(rendered);
    return ESP_OK;
}

esp_err_t AddMetadata(cJSON* root, const v3::RequestMetadata& metadata)
{
    if (root == nullptr || !IsUrlSafe(metadata.request_id, 16, 64) ||
        !IsUrlSafe(metadata.boot_id, 16, 64) ||
        metadata.firmware_version.empty() || metadata.firmware_version.size() > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON* capabilities = cJSON_CreateArray();
    if (capabilities == nullptr) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "request_id", metadata.request_id.c_str());
    cJSON_AddStringToObject(root, "boot_id", metadata.boot_id.c_str());
    cJSON_AddStringToObject(
        root, "firmware_version", metadata.firmware_version.c_str());
    cJSON_AddItemToObject(root, "capabilities", capabilities);
    const char* values[] = {
        "display.epd", "sync.v3", "problem.study.v1"};
    for (const char* value : values) {
        cJSON* item = cJSON_CreateString(value);
        if (item == nullptr) return ESP_ERR_NO_MEM;
        cJSON_AddItemToArray(capabilities, item);
    }
    return ESP_OK;
}

esp_err_t ParseEnvelope(
    cJSON* root,
    const std::string& expected_request_id,
    cJSON** data,
    v3::Error* error)
{
    if (!cJSON_IsObject(root) || data == nullptr || error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = nullptr;
    *error = {};
    uint64_t server_time_ms = 0;
    if (StringField(root, "request_id") != expected_request_id) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON* ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok)) return ESP_ERR_INVALID_RESPONSE;
    if (!cJSON_IsTrue(ok)) {
        cJSON* object = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON* retryable = cJSON_GetObjectItemCaseSensitive(object, "retryable");
        if (!cJSON_IsObject(object) || !cJSON_IsBool(retryable)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        error->code = StringField(object, "code");
        error->retryable = cJSON_IsTrue(retryable);
        uint64_t retry_after_ms = 0;
        if (cJSON_GetObjectItemCaseSensitive(object, "retry_after_ms") != nullptr &&
            !U64Field(object, "retry_after_ms", &retry_after_ms)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        error->retry_after_ms = static_cast<uint32_t>(std::min<uint64_t>(
            retry_after_ms, std::numeric_limits<uint32_t>::max()));
        return error->code.empty() ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL;
    }
    if (!U64Field(root, "server_time_ms", &server_time_ms)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsObject(*data) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

}  // namespace

namespace wqn::protocol::problem_study_v1 {

const char* ReviewActionName(ReviewAction action)
{
    switch (action) {
        case ReviewAction::kCorrect: return "correct";
        case ReviewAction::kHesitant: return "hesitant";
        case ReviewAction::kWrong: return "wrong";
        case ReviewAction::kSkip: return "skip";
    }
    return "";
}

const char* ProblemStatusName(ProblemStatus status)
{
    switch (status) {
        case ProblemStatus::kWrong: return "wrong";
        case ProblemStatus::kNeedsReview: return "needs_review";
        case ProblemStatus::kMastered: return "mastered";
    }
    return "";
}

bool ParseProblemStatus(const std::string& value, ProblemStatus* status)
{
    if (status == nullptr) return false;
    if (value == "wrong") *status = ProblemStatus::kWrong;
    else if (value == "needs_review") *status = ProblemStatus::kNeedsReview;
    else if (value == "mastered") *status = ProblemStatus::kMastered;
    else return false;
    return true;
}

esp_err_t BuildManifestRequest(
    const v3::RequestMetadata& metadata,
    uint64_t cursor,
    int limit,
    std::string* body)
{
    if (body == nullptr || cursor > v3::kMaxSafeJsonInteger || limit < 1 || limit > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(AddMetadata(document.root(), metadata), "problem_study", "metadata");
    const std::string cursor_text = std::to_string(cursor);
    cJSON_AddStringToObject(document.root(), "cursor", cursor_text.c_str());
    cJSON_AddNumberToObject(document.root(), "limit", limit);
    return Render(document.root(), body);
}

esp_err_t BuildObservationRequest(
    const ObservationRequest& request,
    std::string* body)
{
    if (body == nullptr || !IsUuid(request.problem_id) ||
        request.occurred_at.empty() || request.occurred_at.size() > 40) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(AddMetadata(document.root(), request.metadata), "problem_study", "metadata");
    cJSON_AddStringToObject(document.root(), "problem_id", request.problem_id.c_str());
    cJSON_AddStringToObject(
        document.root(), "action", ReviewActionName(request.action));
    cJSON_AddStringToObject(document.root(), "occurred_at", request.occurred_at.c_str());
    return Render(document.root(), body);
}

esp_err_t ParseManifestResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ManifestData* data,
    v3::Error* error)
{
    if (data == nullptr || error == nullptr) return ESP_ERR_INVALID_ARG;
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    ESP_RETURN_ON_ERROR(
        ParseEnvelope(document.root(), expected_request_id, &payload, error),
        "problem_study",
        "manifest envelope");
    cJSON* has_more = cJSON_GetObjectItemCaseSensitive(payload, "has_more");
    if (!ParseSafeDecimal(StringField(payload, "cursor"), &data->cursor) ||
        !cJSON_IsBool(has_more)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->has_more = cJSON_IsTrue(has_more);
    cJSON* sets = cJSON_GetObjectItemCaseSensitive(payload, "problem_sets");
    const int count = cJSON_GetArraySize(sets);
    if (!cJSON_IsArray(sets) || count < 0 ||
        static_cast<size_t>(count) > kMaxManifestSets) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->problem_sets.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        cJSON* object = cJSON_GetArrayItem(sets, index);
        ManifestSet set;
        set.problem_set_id = StringField(object, "problem_set_id");
        set.name = StringField(object, "name");
        cJSON* is_smart = cJSON_GetObjectItemCaseSensitive(object, "is_smart");
        cJSON* deleted = cJSON_GetObjectItemCaseSensitive(object, "deleted");
        if (!cJSON_IsObject(object) || !IsUuid(set.problem_set_id) ||
            set.name.empty() || set.name.size() > 800 ||
            !cJSON_IsBool(is_smart) || !cJSON_IsBool(deleted)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        set.is_smart = cJSON_IsTrue(is_smart);
        set.deleted = cJSON_IsTrue(deleted);
        cJSON* pack = cJSON_GetObjectItemCaseSensitive(object, "pack");
        if (cJSON_IsNull(pack)) {
            // A set whose pack cannot be built yet arrives with a null pack.
            set.has_pack = false;
        } else {
            uint64_t schema_version = 0;
            uint64_t entry_count = 0;
            uint64_t byte_size = 0;
            set.pack.pack_id = StringField(pack, "pack_id");
            set.pack.sha256 = StringField(pack, "sha256");
            set.pack.download_url = StringField(pack, "download_url");
            if (!cJSON_IsObject(pack) || !IsUuid(set.pack.pack_id) ||
                !IsSha256(set.pack.sha256) ||
                set.pack.download_url.empty() ||
                set.pack.download_url.size() > 512 ||
                StringField(pack, "format") != "jsonl" ||
                StringField(pack, "compression") != "zlib" ||
                !U64Field(pack, "pack_revision", &set.pack.pack_revision) ||
                !U64Field(pack, "schema_version", &schema_version) ||
                !U64Field(pack, "entry_count", &entry_count) ||
                !U64Field(pack, "byte_size", &byte_size) ||
                schema_version != kPackSchemaVersion ||
                entry_count > kMaxPackEntries || byte_size == 0 ||
                byte_size > kMaxPackBytes) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            set.pack.schema_version = static_cast<uint32_t>(schema_version);
            set.pack.entry_count = static_cast<uint32_t>(entry_count);
            set.pack.byte_size = static_cast<uint32_t>(byte_size);
            set.has_pack = true;
        }
        data->problem_sets.push_back(std::move(set));
    }
    return ESP_OK;
}

esp_err_t ParseObservationResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ObservationData* data,
    v3::Error* error)
{
    if (data == nullptr || error == nullptr) return ESP_ERR_INVALID_ARG;
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    ESP_RETURN_ON_ERROR(
        ParseEnvelope(document.root(), expected_request_id, &payload, error),
        "problem_study",
        "observation envelope");
    data->observation_id = StringField(payload, "observation_id");
    data->problem_id = StringField(payload, "problem_id");
    cJSON* replayed = cJSON_GetObjectItemCaseSensitive(payload, "replayed");
    cJSON* projection_applied =
        cJSON_GetObjectItemCaseSensitive(payload, "projection_applied");
    if (!IsUuid(data->observation_id) || !IsUuid(data->problem_id) ||
        !ParseAction(StringField(payload, "action"), &data->action) ||
        !ParseProblemStatus(StringField(payload, "status"), &data->status) ||
        !cJSON_IsBool(projection_applied) || !cJSON_IsBool(replayed)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->replayed = cJSON_IsTrue(replayed);
    data->projection_applied = cJSON_IsTrue(projection_applied);

    cJSON* schedule = cJSON_GetObjectItemCaseSensitive(payload, "schedule");
    if (cJSON_IsNull(schedule)) return ESP_OK;
    cJSON* ease_factor = cJSON_GetObjectItemCaseSensitive(schedule, "ease_factor");
    data->schedule.next_review_at = StringField(schedule, "next_review_at");
    if (!cJSON_IsObject(schedule) || data->schedule.next_review_at.empty() ||
        data->schedule.next_review_at.size() > 40 ||
        !U64Field(schedule, "interval_days", &data->schedule.interval_days) ||
        !U64Field(schedule, "repetition_number", &data->schedule.repetition_number) ||
        !cJSON_IsNumber(ease_factor) || !std::isfinite(ease_factor->valuedouble) ||
        ease_factor->valuedouble < 1.0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->schedule.ease_factor = ease_factor->valuedouble;
    data->schedule.present = true;
    return ESP_OK;
}

}  // namespace wqn::protocol::problem_study_v1
