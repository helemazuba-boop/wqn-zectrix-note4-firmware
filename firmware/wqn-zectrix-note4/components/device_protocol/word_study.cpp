#include "device_protocol/word_study.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "cJSON.h"
#include "esp_check.h"

namespace {

using namespace wqn::protocol;
using namespace wqn::protocol::word_study_v1;

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

bool ParseMode(const std::string& value, Mode* mode)
{
    if (mode == nullptr) return false;
    if (value == "sequential") *mode = Mode::kSequential;
    else if (value == "random") *mode = Mode::kRandom;
    else if (value == "dictionary") *mode = Mode::kDictionary;
    else return false;
    return true;
}

bool ParsePurpose(const std::string& value, Purpose* purpose)
{
    if (purpose == nullptr) return false;
    if (value == "study") *purpose = Purpose::kStudy;
    else if (value == "lookup") *purpose = Purpose::kLookup;
    else return false;
    return true;
}

bool ParseOrdering(const std::string& value, Ordering* ordering)
{
    if (ordering == nullptr) return false;
    if (value == "sequential") *ordering = Ordering::kSequential;
    else if (value == "guided_random_v1") *ordering = Ordering::kGuidedRandomV1;
    else if (value == "lexicographic") *ordering = Ordering::kLexicographic;
    else return false;
    return true;
}

bool ParseAction(const std::string& value, ObservationAction* action)
{
    if (action == nullptr) return false;
    if (value == "shown") *action = ObservationAction::kShown;
    else if (value == "revealed") *action = ObservationAction::kRevealed;
    else if (value == "known") *action = ObservationAction::kKnown;
    else if (value == "unknown") *action = ObservationAction::kUnknown;
    else if (value == "skipped") *action = ObservationAction::kSkipped;
    else if (value == "looked_up") *action = ObservationAction::kLookedUp;
    else return false;
    return true;
}

bool ParseStatus(const std::string& value, CandidateStatus* status)
{
    if (status == nullptr) return false;
    if (value == "new") *status = CandidateStatus::kNew;
    else if (value == "learning") *status = CandidateStatus::kLearning;
    else if (value == "review") *status = CandidateStatus::kReview;
    else if (value == "mastered") *status = CandidateStatus::kMastered;
    else return false;
    return true;
}

bool SemanticsMatch(Mode mode, Purpose purpose, Ordering ordering)
{
    return (mode == Mode::kSequential && purpose == Purpose::kStudy &&
            ordering == Ordering::kSequential) ||
        (mode == Mode::kRandom && purpose == Purpose::kStudy &&
         ordering == Ordering::kGuidedRandomV1) ||
        (mode == Mode::kDictionary && purpose == Purpose::kLookup &&
         ordering == Ordering::kLexicographic);
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
        "display.epd", "sync.v3", "word.study.v1", "ai.sse.v2", "flash.v2"};
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

bool ParseScope(cJSON* object, Scope* scope)
{
    if (!cJSON_IsObject(object) || scope == nullptr) return false;
    cJSON* ids = cJSON_GetObjectItemCaseSensitive(object, "deck_ids");
    cJSON* include = cJSON_GetObjectItemCaseSensitive(object, "include_mastered");
    const int count = cJSON_GetArraySize(ids);
    if (!cJSON_IsArray(ids) || !cJSON_IsBool(include) || count < 0 ||
        static_cast<size_t>(count) > kMaxDecks) {
        return false;
    }
    scope->deck_ids.clear();
    scope->include_mastered = cJSON_IsTrue(include);
    scope->deck_ids.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        cJSON* item = cJSON_GetArrayItem(ids, index);
        const std::string id = cJSON_IsString(item) && item->valuestring != nullptr
            ? item->valuestring
            : "";
        if (!IsUuid(id) ||
            std::find(scope->deck_ids.begin(), scope->deck_ids.end(), id) !=
                scope->deck_ids.end()) {
            return false;
        }
        scope->deck_ids.push_back(id);
    }
    return true;
}

int CandidateBucket(const Candidate& candidate, int64_t now_ms)
{
    if (candidate.status == CandidateStatus::kMastered) return 4;
    if (candidate.status == CandidateStatus::kNew) return 2;
    if (candidate.due_at_ms >= 0 && candidate.due_at_ms > now_ms) return 3;
    return candidate.status == CandidateStatus::kLearning ? 0 : 1;
}

}  // namespace

namespace wqn::protocol::word_study_v1 {

const char* ModeName(Mode mode)
{
    switch (mode) {
        case Mode::kSequential: return "sequential";
        case Mode::kRandom: return "random";
        case Mode::kDictionary: return "dictionary";
    }
    return "";
}

const char* CandidatePolicyVersionName(Ordering ordering)
{
    switch (ordering) {
        case Ordering::kSequential: return "sequential_v1";
        case Ordering::kGuidedRandomV1: return "guided_random_v1";
        case Ordering::kLexicographic: return "lexicographic_v1";
    }
    return "";
}

const char* ObservationActionName(ObservationAction action)
{
    switch (action) {
        case ObservationAction::kShown: return "shown";
        case ObservationAction::kRevealed: return "revealed";
        case ObservationAction::kKnown: return "known";
        case ObservationAction::kUnknown: return "unknown";
        case ObservationAction::kSkipped: return "skipped";
        case ObservationAction::kLookedUp: return "looked_up";
    }
    return "";
}

uint64_t GuidedRandomHash(const std::string& seed, const std::string& item_id)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    constexpr uint64_t kPrime = UINT64_C(0x100000001b3);
    for (const unsigned char ch : seed) {
        hash ^= ch;
        hash *= kPrime;
    }
    hash ^= 0;
    hash *= kPrime;
    for (const unsigned char ch : item_id) {
        hash ^= ch;
        hash *= kPrime;
    }
    return hash;
}

void OrderCandidates(
    std::vector<Candidate>* candidates,
    Ordering ordering,
    const std::string& seed,
    int64_t now_ms)
{
    if (candidates == nullptr) return;
    std::sort(candidates->begin(), candidates->end(), [&](const Candidate& a, const Candidate& b) {
        if (ordering == Ordering::kGuidedRandomV1) {
            const int a_bucket = CandidateBucket(a, now_ms);
            const int b_bucket = CandidateBucket(b, now_ms);
            if (a_bucket != b_bucket) return a_bucket < b_bucket;
            const uint64_t a_hash = GuidedRandomHash(seed, a.item_id);
            const uint64_t b_hash = GuidedRandomHash(seed, b.item_id);
            if (a_hash != b_hash) return a_hash < b_hash;
            return a.item_id < b.item_id;
        }
        if (ordering == Ordering::kLexicographic) {
            if (a.normalized_word != b.normalized_word) {
                return a.normalized_word < b.normalized_word;
            }
            return a.item_id < b.item_id;
        }
        if (a.deck_order != b.deck_order) return a.deck_order < b.deck_order;
        if (a.sort_index != b.sort_index) return a.sort_index < b.sort_index;
        if (a.normalized_word != b.normalized_word) {
            return a.normalized_word < b.normalized_word;
        }
        return a.item_id < b.item_id;
    });
}

esp_err_t BuildCreateSessionRequest(
    const CreateSessionRequest& request,
    std::string* body)
{
    if (body == nullptr || request.scope.deck_ids.size() > kMaxDecks ||
        request.optional_count < 1 || request.optional_count > 500 ||
        (!request.seed.empty() && !IsUrlSafe(request.seed, 1, 64))) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(AddMetadata(document.root(), request.metadata), "word_study", "metadata");
    cJSON_AddStringToObject(document.root(), "domain", "word");
    cJSON_AddStringToObject(document.root(), "mode", ModeName(request.mode));
    cJSON* scope = cJSON_CreateObject();
    cJSON* deck_ids = cJSON_CreateArray();
    if (scope == nullptr || deck_ids == nullptr) {
        cJSON_Delete(scope);
        cJSON_Delete(deck_ids);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(document.root(), "scope", scope);
    cJSON_AddItemToObject(scope, "deck_ids", deck_ids);
    for (const std::string& id : request.scope.deck_ids) {
        if (!IsUuid(id)) return ESP_ERR_INVALID_ARG;
        cJSON_AddItemToArray(deck_ids, cJSON_CreateString(id.c_str()));
    }
    cJSON_AddBoolToObject(scope, "include_mastered", request.scope.include_mastered);
    cJSON_AddNumberToObject(document.root(), "optional_count", request.optional_count);
    if (!request.seed.empty()) {
        cJSON_AddStringToObject(document.root(), "seed", request.seed.c_str());
    }
    return Render(document.root(), body);
}

esp_err_t BuildObservationRequest(
    const ObservationRequest& request,
    std::string* body)
{
    if (body == nullptr || !IsUuid(request.session_id) ||
        !IsUuid(request.item_id) || request.occurred_at.empty() ||
        request.sequence > v3::kMaxSafeJsonInteger) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(AddMetadata(document.root(), request.metadata), "word_study", "metadata");
    cJSON_AddStringToObject(document.root(), "session_id", request.session_id.c_str());
    cJSON_AddNumberToObject(
        document.root(), "sequence", static_cast<double>(request.sequence));
    cJSON_AddStringToObject(document.root(), "item_id", request.item_id.c_str());
    cJSON_AddStringToObject(
        document.root(), "action", ObservationActionName(request.action));
    cJSON_AddStringToObject(document.root(), "mode", ModeName(request.mode));
    cJSON_AddStringToObject(document.root(), "occurred_at", request.occurred_at.c_str());
    return Render(document.root(), body);
}

esp_err_t BuildCandidatePageRequest(
    const CandidatePageRequest& request,
    std::string* body)
{
    uint64_t cursor = 0;
    if (body == nullptr || !ParseSafeDecimal(request.cursor, &cursor) ||
        request.limit < 1 ||
        request.limit > static_cast<int>(kMaxCandidatePageItems)) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(
        AddMetadata(document.root(), request.metadata),
        "word_study",
        "candidate metadata");
    cJSON_AddStringToObject(document.root(), "cursor", request.cursor.c_str());
    cJSON_AddNumberToObject(document.root(), "limit", request.limit);
    return Render(document.root(), body);
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
    ESP_RETURN_ON_ERROR(AddMetadata(document.root(), metadata), "word_study", "metadata");
    const std::string cursor_text = std::to_string(cursor);
    cJSON_AddStringToObject(document.root(), "cursor", cursor_text.c_str());
    cJSON_AddNumberToObject(document.root(), "limit", limit);
    return Render(document.root(), body);
}

esp_err_t ParseSessionResponse(
    const std::string& body,
    const std::string& expected_request_id,
    SessionData* data,
    v3::Error* error)
{
    if (data == nullptr || error == nullptr) return ESP_ERR_INVALID_ARG;
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    ESP_RETURN_ON_ERROR(
        ParseEnvelope(document.root(), expected_request_id, &payload, error),
        "word_study",
        "session envelope");

    data->session_id = StringField(payload, "session_id");
    data->seed = StringField(payload, "seed");
    data->candidate_policy_version =
        StringField(payload, "candidate_policy_version");
    cJSON* optional_count = cJSON_GetObjectItemCaseSensitive(payload, "optional_count");
    if (!IsUuid(data->session_id) || StringField(payload, "domain") != "word" ||
        !ParseMode(StringField(payload, "mode"), &data->mode) ||
        !ParsePurpose(StringField(payload, "purpose"), &data->purpose) ||
        !ParseOrdering(StringField(payload, "ordering"), &data->ordering) ||
        !SemanticsMatch(data->mode, data->purpose, data->ordering) ||
        data->candidate_policy_version !=
            CandidatePolicyVersionName(data->ordering) ||
        !IsUrlSafe(data->seed, 1, 64) ||
        !ParseScope(cJSON_GetObjectItemCaseSensitive(payload, "scope"), &data->scope) ||
        !U64Field(payload, "next_sequence", &data->next_sequence) ||
        !U64Field(payload, "progress_revision", &data->progress_revision)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (cJSON_IsNumber(optional_count)) {
        if (optional_count->valueint < 1 || optional_count->valueint > 500 ||
            optional_count->valuedouble != optional_count->valueint) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        data->optional_count = optional_count->valueint;
    } else {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* snapshots = cJSON_GetObjectItemCaseSensitive(payload, "snapshot");
    const int snapshot_count = cJSON_GetArraySize(snapshots);
    if (!cJSON_IsArray(snapshots) || snapshot_count < 0 ||
        static_cast<size_t>(snapshot_count) > kMaxDecks) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->snapshot.reserve(static_cast<size_t>(snapshot_count));
    for (int index = 0; index < snapshot_count; ++index) {
        cJSON* object = cJSON_GetArrayItem(snapshots, index);
        PackSnapshot item;
        item.deck_id = StringField(object, "deck_id");
        item.sha256 = StringField(object, "sha256");
        if (!cJSON_IsObject(object) || !IsUuid(item.deck_id) ||
            !IsSha256(item.sha256) ||
            !U64Field(object, "content_revision", &item.content_revision) ||
            !U64Field(object, "pack_revision", &item.pack_revision)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        data->snapshot.push_back(std::move(item));
    }

    cJSON* items = cJSON_GetObjectItemCaseSensitive(payload, "items");
    const int item_count = cJSON_GetArraySize(items);
    if (!cJSON_IsArray(items) || item_count < 0 ||
        static_cast<size_t>(item_count) > kMaxSessionItems) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->items.reserve(static_cast<size_t>(item_count));
    for (int index = 0; index < item_count; ++index) {
        cJSON* object = cJSON_GetArrayItem(items, index);
        SessionItem item;
        item.item_id = StringField(object, "item_id");
        item.deck_id = StringField(object, "deck_id");
        if (!cJSON_IsObject(object) || !IsUuid(item.item_id) ||
            !IsUuid(item.deck_id) || !U64Field(object, "ordinal", &item.ordinal)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        data->items.push_back(std::move(item));
    }
    cJSON* has_more = cJSON_GetObjectItemCaseSensitive(payload, "has_more");
    if (!cJSON_IsBool(has_more)) return ESP_ERR_INVALID_RESPONSE;
    data->has_more = cJSON_IsTrue(has_more);
    cJSON* cursor = cJSON_GetObjectItemCaseSensitive(payload, "cursor");
    if (cursor != nullptr) {
        data->cursor = StringField(payload, "cursor");
        if (data->cursor.size() > 256) return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t ParseCandidatePageResponse(
    const std::string& body,
    const std::string& expected_request_id,
    CandidatePageData* data,
    v3::Error* error)
{
    if (data == nullptr || error == nullptr) return ESP_ERR_INVALID_ARG;
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    ESP_RETURN_ON_ERROR(
        ParseEnvelope(document.root(), expected_request_id, &payload, error),
        "word_study",
        "candidate page envelope");

    data->session_id = StringField(payload, "session_id");
    data->candidate_policy_version =
        StringField(payload, "candidate_policy_version");
    data->seed = StringField(payload, "seed");
    data->cursor = StringField(payload, "cursor");
    data->next_cursor = StringField(payload, "next_cursor");
    uint64_t cursor = 0;
    uint64_t next_cursor = 0;
    if (!IsUuid(data->session_id) ||
        !ParseOrdering(StringField(payload, "ordering"), &data->ordering) ||
        data->candidate_policy_version !=
            CandidatePolicyVersionName(data->ordering) ||
        !IsUrlSafe(data->seed, 1, 64) ||
        !U64Field(payload, "progress_revision", &data->progress_revision) ||
        !ParseSafeDecimal(data->cursor, &cursor) ||
        !ParseSafeDecimal(data->next_cursor, &next_cursor) ||
        next_cursor < cursor) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* snapshots = cJSON_GetObjectItemCaseSensitive(payload, "snapshot");
    const int snapshot_count = cJSON_GetArraySize(snapshots);
    if (!cJSON_IsArray(snapshots) || snapshot_count < 0 ||
        static_cast<size_t>(snapshot_count) > kMaxDecks) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->snapshot.reserve(static_cast<size_t>(snapshot_count));
    for (int index = 0; index < snapshot_count; ++index) {
        cJSON* object = cJSON_GetArrayItem(snapshots, index);
        PackSnapshot item;
        item.deck_id = StringField(object, "deck_id");
        item.sha256 = StringField(object, "sha256");
        if (!cJSON_IsObject(object) || !IsUuid(item.deck_id) ||
            !IsSha256(item.sha256) ||
            !U64Field(object, "content_revision", &item.content_revision) ||
            !U64Field(object, "pack_revision", &item.pack_revision)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        data->snapshot.push_back(std::move(item));
    }

    cJSON* items = cJSON_GetObjectItemCaseSensitive(payload, "items");
    const int item_count = cJSON_GetArraySize(items);
    if (!cJSON_IsArray(items) || item_count < 0 ||
        static_cast<size_t>(item_count) > kMaxCandidatePageItems ||
        next_cursor - cursor != static_cast<uint64_t>(item_count)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->items.reserve(static_cast<size_t>(item_count));
    for (int index = 0; index < item_count; ++index) {
        cJSON* object = cJSON_GetArrayItem(items, index);
        SessionItem item;
        item.item_id = StringField(object, "item_id");
        item.deck_id = StringField(object, "deck_id");
        if (!cJSON_IsObject(object) || !IsUuid(item.item_id) ||
            !IsUuid(item.deck_id) || !U64Field(object, "ordinal", &item.ordinal) ||
            item.ordinal != cursor + static_cast<uint64_t>(index)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        data->items.push_back(std::move(item));
    }
    cJSON* has_more = cJSON_GetObjectItemCaseSensitive(payload, "has_more");
    if (!cJSON_IsBool(has_more)) return ESP_ERR_INVALID_RESPONSE;
    data->has_more = cJSON_IsTrue(has_more);
    if ((data->has_more && data->items.empty()) ||
        (!data->has_more && next_cursor < cursor)) {
        return ESP_ERR_INVALID_RESPONSE;
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
        "word_study",
        "observation envelope");
    data->observation_id = StringField(payload, "observation_id");
    data->session_id = StringField(payload, "session_id");
    data->item_id = StringField(payload, "item_id");
    cJSON* replayed = cJSON_GetObjectItemCaseSensitive(payload, "replayed");
    cJSON* projection_applied =
        cJSON_GetObjectItemCaseSensitive(payload, "projection_applied");
    if (!IsUuid(data->observation_id) || !IsUuid(data->session_id) ||
        !IsUuid(data->item_id) ||
        !U64Field(payload, "sequence", &data->sequence) ||
        !ParseAction(StringField(payload, "action"), &data->action) ||
        !cJSON_IsBool(projection_applied) || !cJSON_IsBool(replayed)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->replayed = cJSON_IsTrue(replayed);
    data->projection_applied = cJSON_IsTrue(projection_applied);

    cJSON* progress = cJSON_GetObjectItemCaseSensitive(payload, "progress");
    if (cJSON_IsNull(progress)) return ESP_OK;
    if (!cJSON_IsObject(progress) ||
        !ParseStatus(StringField(progress, "status"), &data->progress.status) ||
        !U64Field(progress, "reviewed_count", &data->progress.reviewed_count) ||
        !U64Field(progress, "known_count", &data->progress.known_count) ||
        !U64Field(progress, "unknown_count", &data->progress.unknown_count)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON* due = cJSON_GetObjectItemCaseSensitive(progress, "due_at");
    if (cJSON_IsString(due) && due->valuestring != nullptr) {
        data->progress.due_at = due->valuestring;
    } else if (!cJSON_IsNull(due)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->progress.present = true;
    return ESP_OK;
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
        "word_study",
        "manifest envelope");
    cJSON* has_more = cJSON_GetObjectItemCaseSensitive(payload, "has_more");
    if (!ParseSafeDecimal(StringField(payload, "cursor"), &data->cursor) ||
        !cJSON_IsBool(has_more)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->has_more = cJSON_IsTrue(has_more);
    cJSON* decks = cJSON_GetObjectItemCaseSensitive(payload, "decks");
    const int count = cJSON_GetArraySize(decks);
    if (!cJSON_IsArray(decks) || count < 0 ||
        static_cast<size_t>(count) > kMaxManifestDecks) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->decks.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        cJSON* object = cJSON_GetArrayItem(decks, index);
        ManifestDeck deck;
        deck.deck_id = StringField(object, "deck_id");
        deck.title = StringField(object, "title");
        cJSON* deleted = cJSON_GetObjectItemCaseSensitive(object, "deleted");
        if (!cJSON_IsObject(object) || !IsUuid(deck.deck_id) ||
            deck.title.empty() || deck.title.size() > 80 || !cJSON_IsBool(deleted) ||
            !U64Field(object, "change_sequence", &deck.change_sequence) ||
            !U64Field(object, "content_revision", &deck.content_revision)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        deck.deleted = cJSON_IsTrue(deleted);
        cJSON* pack = cJSON_GetObjectItemCaseSensitive(object, "pack");
        if (deck.deleted) {
            if (!cJSON_IsNull(pack)) return ESP_ERR_INVALID_RESPONSE;
        } else {
            uint64_t schema_version = 0;
            uint64_t entry_count = 0;
            uint64_t byte_size = 0;
            deck.pack.pack_id = StringField(pack, "pack_id");
            deck.pack.sha256 = StringField(pack, "sha256");
            deck.pack.download_url = StringField(pack, "download_url");
            if (!cJSON_IsObject(pack) || !IsUuid(deck.pack.pack_id) ||
                !IsSha256(deck.pack.sha256) || deck.pack.download_url.empty() ||
                deck.pack.download_url.size() > 512 ||
                StringField(pack, "format") != "jsonl" ||
                StringField(pack, "compression") != "none" ||
                !U64Field(pack, "pack_revision", &deck.pack.pack_revision) ||
                !U64Field(pack, "schema_version", &schema_version) ||
                !U64Field(pack, "entry_count", &entry_count) ||
                !U64Field(pack, "byte_size", &byte_size) ||
                schema_version != kPackSchemaVersion ||
                entry_count > kMaxPackEntries || byte_size == 0 ||
                byte_size > kMaxPackBytes) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            deck.pack.schema_version = static_cast<uint32_t>(schema_version);
            deck.pack.entry_count = static_cast<uint32_t>(entry_count);
            deck.pack.byte_size = static_cast<uint32_t>(byte_size);
            deck.has_pack = true;
        }
        data->decks.push_back(std::move(deck));
    }
    return ESP_OK;
}

}  // namespace wqn::protocol::word_study_v1
