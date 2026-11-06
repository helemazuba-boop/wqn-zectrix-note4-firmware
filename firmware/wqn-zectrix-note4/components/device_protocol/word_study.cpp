#include "device_protocol/word_study.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

#include "cJSON.h"
#include "device_protocol/json_depth_guard.h"
#include "device_protocol/json_build_helpers.h"
#include "esp_check.h"

namespace {

using namespace wqn::protocol;
using namespace wqn::protocol::word_study_v1;

class JsonDocument {
public:
    explicit JsonDocument(cJSON* root) : root_(root) {}
    explicit JsonDocument(const std::string& body)
        : root_(JsonInputWithinLimits(body.data(), body.size())
                    ? cJSON_Parse(body.c_str())
                    : nullptr) {}
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
    else if (value == "review") *mode = Mode::kReview;
    else if (value == "intake") *mode = Mode::kIntake;
    else if (value == "shuffle") *mode = Mode::kShuffle;
    else if (value == "mistakes") *mode = Mode::kMistakes;
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
    else if (value == "due_queue_v1") *ordering = Ordering::kDueQueueV1;
    else if (value == "new_intake_v1") *ordering = Ordering::kNewIntakeV1;
    else if (value == "pure_random_v1") *ordering = Ordering::kPureRandomV1;
    else if (value == "mistake_words_v1") *ordering = Ordering::kMistakeWordsV1;
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
         ordering == Ordering::kLexicographic) ||
        (mode == Mode::kReview && purpose == Purpose::kStudy &&
         ordering == Ordering::kDueQueueV1) ||
        (mode == Mode::kIntake && purpose == Purpose::kStudy &&
         ordering == Ordering::kNewIntakeV1) ||
        (mode == Mode::kShuffle && purpose == Purpose::kStudy &&
         ordering == Ordering::kPureRandomV1) ||
        (mode == Mode::kMistakes && purpose == Purpose::kStudy &&
         ordering == Ordering::kMistakeWordsV1);
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
    if (cJSON_AddStringToObject(root, "request_id", metadata.request_id.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(root, "boot_id", metadata.boot_id.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(
        root, "firmware_version", metadata.firmware_version.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON* capabilities = cJSON_AddArrayToObject(root, "capabilities");
    if (capabilities == nullptr) return ESP_ERR_NO_MEM;
    const char* values[] = {
        "display.epd", "sync.v3", "word.study.v1", "ai.sse.v2", "flash.v2"};
    for (const char* value : values) {
        if (!AppendJsonString(capabilities, value)) return ESP_ERR_NO_MEM;
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

// The review queue is a plain list: relearning/learning words first, then due
// review words by due_at. Selection (what is due today) happens server-side.
int DueQueueBucket(const Candidate& candidate)
{
    switch (candidate.status) {
        case CandidateStatus::kLearning: return 0;
        case CandidateStatus::kReview: return 1;
        case CandidateStatus::kNew: return 2;
        case CandidateStatus::kMastered: return 3;
    }
    return 3;
}

bool DeckOrderLess(const Candidate& a, const Candidate& b)
{
    if (a.deck_order != b.deck_order) return a.deck_order < b.deck_order;
    if (a.sort_index != b.sort_index) return a.sort_index < b.sort_index;
    if (a.normalized_word != b.normalized_word) {
        return a.normalized_word < b.normalized_word;
    }
    return a.item_id < b.item_id;
}

// A decoded cJSON string is NUL-terminated with no length. Reject any embedded
// NUL (raw or \u0000) in device-authored strings so the builder fails closed
// instead of emitting a value the server's JsonInputWithinLimits would drop.
bool ContainsNul(const std::string& value)
{
    return value.find('\0') != std::string::npos;
}

// Firmware-authored free text must not carry control bytes that cJSON would
// encode into \uXXXX escapes the peer treats as unrepresentable.
bool IsCleanText(const std::string& value, size_t maximum)
{
    if (value.empty() || value.size() > maximum || ContainsNul(value)) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x20 && ch != 0x7f;
    });
}

// Minimal RFC3339 shape check for device timestamps: YYYY-MM-DDTHH:MM:SS with
// an optional fractional part and a Z / +/-HH:MM zone. The server owns full
// calendar validation; the device only refuses obviously malformed strings.
bool IsOccurredAt(const std::string& value)
{
    if (value.size() < 20 || value.size() > 40 || ContainsNul(value)) return false;
    const auto digit = [&](size_t i) { return value[i] >= '0' && value[i] <= '9'; };
    if (!digit(0) || !digit(1) || !digit(2) || !digit(3) || value[4] != '-' ||
        !digit(5) || !digit(6) || value[7] != '-' || !digit(8) || !digit(9) ||
        value[10] != 'T' || !digit(11) || !digit(12) || value[13] != ':' ||
        !digit(14) || !digit(15) || value[16] != ':' || !digit(17) || !digit(18)) {
        return false;
    }
    size_t i = 19;
    if (i < value.size() && value[i] == '.') {
        ++i;
        const size_t start = i;
        while (i < value.size() && digit(i)) ++i;
        if (i == start) return false;
    }
    if (i >= value.size()) return false;
    if (value[i] == 'Z') return i + 1 == value.size();
    if (value[i] == '+' || value[i] == '-') {
        ++i;
        if (i + 5 != value.size() || !digit(i) || !digit(i + 1) ||
            value[i + 2] != ':' || !digit(i + 3) || !digit(i + 4)) return false;
        return true;
    }
    return false;
}

// Strict object shape: exactly the listed keys, nothing more, nothing missing.
// Used on receipts so a malformed/extra-field payload cannot masquerade as a
// successful ACK (the wire schema sets additionalProperties: false).
bool HasExactlyKeys(cJSON* object, std::initializer_list<const char*> keys)
{
    if (!cJSON_IsObject(object)) return false;
    if (static_cast<size_t>(cJSON_GetArraySize(object)) != keys.size()) return false;
    for (const char* key : keys) {
        if (cJSON_GetObjectItemCaseSensitive(object, key) == nullptr) return false;
    }
    return true;
}

bool BoolField(cJSON* object, const char* key, bool* output)
{
    if (output == nullptr) return false;
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsBool(value)) return false;
    *output = cJSON_IsTrue(value);
    return true;
}

// Bounded string field: 1..maximum, URL-safe, and no embedded NUL.
bool SafeIdField(cJSON* object, const char* key, size_t minimum, size_t maximum,
                 std::string* output)
{
    if (output == nullptr) return false;
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
    *output = value->valuestring;
    return IsUrlSafe(*output, minimum, maximum);
}

// Render into a local buffer and only publish on full success. A failed build
// (validation error, OOM, or oversize) must never leave a half-written body.
esp_err_t RenderToBody(cJSON* root, size_t max_bytes, std::string* body)
{
    if (root == nullptr || body == nullptr) return ESP_ERR_INVALID_ARG;
    char* rendered = cJSON_PrintUnformatted(root);
    if (rendered == nullptr) return ESP_ERR_NO_MEM;
    std::string local(rendered);
    cJSON_free(rendered);
    if (local.size() > max_bytes) return ESP_ERR_INVALID_SIZE;
    body->swap(local);
    return ESP_OK;
}

// Local-session metadata: same v3 requestMetadata fields as AddMetadata but
// advertises only the long-sequence capabilities the endpoint expects. The
// schema forbids extra session semantics fields on the wire.
esp_err_t AddLocalSessionMetadata(cJSON* root, const v3::RequestMetadata& metadata)
{
    if (root == nullptr || !IsUrlSafe(metadata.request_id, 16, 64) ||
        !IsUrlSafe(metadata.boot_id, 16, 64) ||
        !IsCleanText(metadata.firmware_version, 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_AddStringToObject(root, "request_id", metadata.request_id.c_str()) == nullptr ||
        cJSON_AddStringToObject(root, "boot_id", metadata.boot_id.c_str()) == nullptr ||
        cJSON_AddStringToObject(
            root, "firmware_version", metadata.firmware_version.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON* capabilities = cJSON_AddArrayToObject(root, "capabilities");
    if (capabilities == nullptr) return ESP_ERR_NO_MEM;
    for (const char* value : {"word.study.v1", "word.study.longseq.v1"}) {
        if (!AppendJsonString(capabilities, value)) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// Validate the immutable local-session request fields before any allocation.
// Returns false for any shape the server would reject, so the builder fails
// closed rather than emitting a doomed request.
bool ValidateLocalRegistration(const LocalSessionRegistrationRequest& r)
{
    if (!IsUuid(r.session_id) || !IsUrlSafe(r.seed, 1, kLocalSeedMaxLength) ||
        r.start_index > kMaxLocalStartIndex) {
        return false;
    }
    if (r.scope.deck_ids.empty() || r.scope.deck_ids.size() > kMaxDecks ||
        !((r.mode == Mode::kSequential && r.scope.include_mastered) ||
          (r.mode == Mode::kShuffle && !r.scope.include_mastered)) ||
        r.progress_revision > v3::kMaxSafeJsonInteger) {
        return false;
    }
    std::vector<std::string> decks = r.scope.deck_ids;
    std::sort(decks.begin(), decks.end());
    if (std::adjacent_find(decks.begin(), decks.end()) != decks.end()) return false;
    for (const auto& id : r.scope.deck_ids) {
        if (!IsUuid(id)) return false;
    }
    if (r.snapshot.empty() || r.snapshot.size() > kMaxDecks ||
        r.snapshot.size() != r.scope.deck_ids.size()) {
        return false;
    }
    std::vector<std::string> snap_decks;
    for (const auto& snap : r.snapshot) {
        if (!IsUuid(snap.deck_id) || !IsSha256(snap.sha256) ||
            snap.content_revision > v3::kMaxSafeJsonInteger ||
            snap.pack_revision > v3::kMaxSafeJsonInteger || snap.pack_revision < 1) {
            return false;
        }
        snap_decks.push_back(snap.deck_id);
    }
    std::sort(snap_decks.begin(), snap_decks.end());
    if (std::adjacent_find(snap_decks.begin(), snap_decks.end()) != snap_decks.end() ||
        snap_decks != decks) {
        return false;  // exact deck-set equality between scope and snapshot
    }
    if (r.items.empty() || r.items.size() > kMaxSessionItems) return false;
    std::vector<std::string> item_ids;
    for (size_t index = 0; index < r.items.size(); ++index) {
        const auto& item = r.items[index];
        if (!IsUuid(item.item_id) || !IsUuid(item.deck_id) ||
            item.ordinal != index ||
            std::find(decks.begin(), decks.end(), item.deck_id) == decks.end()) {
            return false;  // ordinal 0..n-1 in order; item deck must be in scope
        }
        item_ids.push_back(item.item_id);
    }
    std::sort(item_ids.begin(), item_ids.end());
    return std::adjacent_find(item_ids.begin(), item_ids.end()) == item_ids.end();
}

bool ValidateBatchRequest(const ObservationBatchRequest& r)
{
    if (!IsUuid(r.session_id) || r.observations.empty() ||
        r.observations.size() > kMaxObservationBatchItems) {
        return false;
    }
    std::vector<std::string> ids;
    for (size_t index = 0; index < r.observations.size(); ++index) {
        const auto& o = r.observations[index];
        if (!IsUrlSafe(o.request_id, 16, 64) || !IsUuid(o.item_id) ||
            o.sequence > v3::kMaxSafeJsonInteger || !IsOccurredAt(o.occurred_at) ||
            (index > 0 && o.sequence != r.observations[index - 1].sequence + 1)) {
            return false;  // unique ids + consecutive ascending sequences
        }
        if (ObservationActionName(o.action)[0] == '\0' || ModeName(o.mode)[0] == '\0') {
            return false;
        }
        ids.push_back(o.request_id);
    }
    std::sort(ids.begin(), ids.end());
    return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

}  // namespace

namespace wqn::protocol::word_study_v1 {

const char* ModeName(Mode mode)
{
    switch (mode) {
        case Mode::kSequential: return "sequential";
        case Mode::kRandom: return "random";
        case Mode::kDictionary: return "dictionary";
        case Mode::kReview: return "review";
        case Mode::kIntake: return "intake";
        case Mode::kShuffle: return "shuffle";
        case Mode::kMistakes: return "mistakes";
    }
    return "";
}

const char* CandidatePolicyVersionName(Ordering ordering)
{
    switch (ordering) {
        case Ordering::kSequential: return "sequential_v1";
        case Ordering::kGuidedRandomV1: return "guided_random_v1";
        case Ordering::kLexicographic: return "lexicographic_v1";
        case Ordering::kDueQueueV1: return "due_queue_v1";
        case Ordering::kNewIntakeV1: return "new_intake_v1";
        case Ordering::kPureRandomV1: return "pure_random_v1";
        case Ordering::kMistakeWordsV1: return "mistake_words_v1";
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
        if (ordering == Ordering::kPureRandomV1 ||
            ordering == Ordering::kMistakeWordsV1) {
            const uint64_t a_hash = GuidedRandomHash(seed, a.item_id);
            const uint64_t b_hash = GuidedRandomHash(seed, b.item_id);
            if (a_hash != b_hash) return a_hash < b_hash;
            return a.item_id < b.item_id;
        }
        if (ordering == Ordering::kDueQueueV1) {
            const int a_bucket = DueQueueBucket(a);
            const int b_bucket = DueQueueBucket(b);
            if (a_bucket != b_bucket) return a_bucket < b_bucket;
            const int64_t a_due = a.due_at_ms >= 0 ? a.due_at_ms : INT64_MAX;
            const int64_t b_due = b.due_at_ms >= 0 ? b.due_at_ms : INT64_MAX;
            if (a_due != b_due) return a_due < b_due;
            return DeckOrderLess(a, b);
        }
        if (ordering == Ordering::kLexicographic) {
            if (a.normalized_word != b.normalized_word) {
                return a.normalized_word < b.normalized_word;
            }
            return a.item_id < b.item_id;
        }
        // `kSequential` and `kNewIntakeV1` are both a faithful deck-order walk.
        return DeckOrderLess(a, b);
    });
}

esp_err_t BuildCreateSessionRequest(
    const CreateSessionRequest& request,
    std::string* body)
{
    if (body == nullptr || request.scope.deck_ids.size() > kMaxDecks ||
        request.optional_count < 1 || request.optional_count > 500 ||
        (request.start_index >= 0 && request.start_index > 1000000) ||
        request.new_word_limit < 0 || request.new_word_limit > 200 ||
        (!request.seed.empty() && !IsUrlSafe(request.seed, 1, 64))) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(AddMetadata(document.root(), request.metadata), "word_study", "metadata");
    if (cJSON_AddStringToObject(document.root(), "domain", "word") == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(document.root(), "mode", ModeName(request.mode)) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON* scope = cJSON_AddObjectToObject(document.root(), "scope");
    if (scope == nullptr) return ESP_ERR_NO_MEM;
    cJSON* deck_ids = cJSON_AddArrayToObject(scope, "deck_ids");
    if (deck_ids == nullptr) return ESP_ERR_NO_MEM;
    for (const std::string& id : request.scope.deck_ids) {
        if (!IsUuid(id)) return ESP_ERR_INVALID_ARG;
        if (!AppendJsonString(deck_ids, id.c_str())) return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddBoolToObject(scope, "include_mastered", request.scope.include_mastered) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddNumberToObject(document.root(), "optional_count", request.optional_count) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (!request.seed.empty()) {
        if (cJSON_AddStringToObject(document.root(), "seed", request.seed.c_str()) == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (request.start_index >= 0) {
        if (cJSON_AddNumberToObject(document.root(), "start_index", request.start_index) == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (request.new_word_limit > 0) {
        if (cJSON_AddNumberToObject(document.root(), "new_word_limit", request.new_word_limit) == nullptr) {
            return ESP_ERR_NO_MEM;
        }
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
    if (cJSON_AddStringToObject(document.root(), "session_id", request.session_id.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddNumberToObject(
        document.root(), "sequence", static_cast<double>(request.sequence)) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(document.root(), "item_id", request.item_id.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(
        document.root(), "action", ObservationActionName(request.action)) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(document.root(), "mode", ModeName(request.mode)) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(document.root(), "occurred_at", request.occurred_at.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
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
    if (cJSON_AddStringToObject(document.root(), "cursor", request.cursor.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddNumberToObject(document.root(), "limit", request.limit) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
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
    if (cJSON_AddStringToObject(document.root(), "cursor", cursor_text.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddNumberToObject(document.root(), "limit", limit) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
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
    // Not ESP_RETURN_ON_ERROR: a rejected observation (409 SEQUENCE_GAP and
    // friends) legitimately carries a protocol error envelope instead of an
    // observation payload. Logging that as an E-level firmware fault made
    // every normal rejection look like a parser bug and masked the real
    // failure. The caller inspects `error` and logs status/code once.
    const esp_err_t envelope_result =
        ParseEnvelope(document.root(), expected_request_id, &payload, error);
    if (envelope_result != ESP_OK) {
        return envelope_result;
    }
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
                (StringField(pack, "compression") != "zlib" &&
                 StringField(pack, "compression") != "none") ||
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

esp_err_t BuildEligibilityRequest(const EligibilityRequest& request, std::string* body)
{
    if (body == nullptr || request.snapshot.empty() || request.snapshot.size() > kMaxDecks)
        return ESP_ERR_INVALID_ARG;
    JsonDocument doc(cJSON_CreateObject());
    if (doc.root() == nullptr)
        return ESP_ERR_NO_MEM;
    auto result = AddLocalSessionMetadata(doc.root(), request.metadata);
    if (result != ESP_OK)
        return result;
    cJSON* pins = cJSON_AddArrayToObject(doc.root(), "snapshot");
    if (pins == nullptr)
        return ESP_ERR_NO_MEM;
    std::vector<std::string> ids;
    for (const auto& pin : request.snapshot)
    {
        if (!IsUuid(pin.deck_id) || !IsSha256(pin.sha256) || pin.pack_revision < 1 ||
            pin.pack_revision > v3::kMaxSafeJsonInteger ||
            pin.content_revision > v3::kMaxSafeJsonInteger ||
            std::find(ids.begin(), ids.end(), pin.deck_id) != ids.end())
            return ESP_ERR_INVALID_ARG;
        ids.push_back(pin.deck_id);
        cJSON* value = cJSON_CreateObject();
        if (value == nullptr)
            return ESP_ERR_NO_MEM;
        if (cJSON_AddStringToObject(value, "deck_id", pin.deck_id.c_str()) == nullptr ||
            cJSON_AddStringToObject(value, "sha256", pin.sha256.c_str()) == nullptr ||
            cJSON_AddNumberToObject(value, "pack_revision",
                                    static_cast<double>(pin.pack_revision)) == nullptr ||
            cJSON_AddNumberToObject(value, "content_revision",
                                    static_cast<double>(pin.content_revision)) == nullptr)
        {
            cJSON_Delete(value);
            return ESP_ERR_NO_MEM;
        }
        if (!cJSON_AddItemToArray(pins, value))
        {
            cJSON_Delete(value);
            return ESP_ERR_NO_MEM;
        }
    }
    char* rendered = cJSON_PrintUnformatted(doc.root());
    if (!rendered)
        return ESP_ERR_NO_MEM;
    *body = rendered;
    cJSON_free(rendered);
    return body->size() <= 16U * 1024U ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t ParseEligibilityResponse(const std::string& body, const std::string& request_id,
                                   EligibilityData* data, v3::Error* error)
{
    if (data == nullptr || error == nullptr)
        return ESP_ERR_INVALID_ARG;
    *data = {};
    if (body.size() > 16U * 1024U)
        return ESP_ERR_INVALID_SIZE;
    JsonDocument doc(body);
    cJSON* payload = nullptr;
    auto result = ParseEnvelope(doc.root(), request_id, &payload, error);
    if (result != ESP_OK)
        return result;
    if (!HasExactlyKeys(payload, {"progress_revision", "item_ids_sha256", "mastered_bitmap"}))
        return ESP_ERR_INVALID_RESPONSE;
    EligibilityData parsed;
    parsed.item_ids_sha256 = StringField(payload, "item_ids_sha256");
    parsed.mastered_bitmap = StringField(payload, "mastered_bitmap");
    if (!U64Field(payload, "progress_revision", &parsed.progress_revision) ||
        !IsSha256(parsed.item_ids_sha256) || parsed.mastered_bitmap.empty() ||
        parsed.mastered_bitmap.size() > 10000 ||
        !std::all_of(parsed.mastered_bitmap.begin(), parsed.mastered_bitmap.end(),
                     [](char c) { return c == '0' || c == '1'; }))
        return ESP_ERR_INVALID_RESPONSE;
    *data = std::move(parsed);
    return ESP_OK;
}

esp_err_t BuildLocalSessionRegistrationRequest(
    const LocalSessionRegistrationRequest& request,
    std::string* body)
{
    if (body == nullptr || !ValidateLocalRegistration(request)) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) return ESP_ERR_NO_MEM;
    esp_err_t result = AddLocalSessionMetadata(document.root(), request.metadata);
    if (result != ESP_OK) return result;
    if (cJSON_AddStringToObject(
            document.root(), "session_id", request.session_id.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    // Preserve byte/semantic compatibility for legacy sequential registrations.
    if (request.mode == Mode::kShuffle &&
        (cJSON_AddStringToObject(document.root(),"mode","shuffle")==nullptr ||
         cJSON_AddNumberToObject(document.root(),"progress_revision",static_cast<double>(request.progress_revision))==nullptr)) return ESP_ERR_NO_MEM;
    cJSON* scope = cJSON_AddObjectToObject(document.root(), "scope");
    cJSON* deck_ids = scope != nullptr ? cJSON_AddArrayToObject(scope, "deck_ids") : nullptr;
    if (scope == nullptr || deck_ids == nullptr ||
        cJSON_AddBoolToObject(scope, "include_mastered", request.scope.include_mastered) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    for (const std::string& id : request.scope.deck_ids) {
        if (!AppendJsonString(deck_ids, id.c_str())) return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(document.root(), "seed", request.seed.c_str()) == nullptr ||
        cJSON_AddNumberToObject(
            document.root(), "start_index", static_cast<double>(request.start_index)) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON* snapshot = cJSON_AddArrayToObject(document.root(), "snapshot");
    if (snapshot == nullptr) return ESP_ERR_NO_MEM;
    for (const PackSnapshot& snap : request.snapshot) {
        cJSON* object = cJSON_CreateObject();
        if (object == nullptr) return ESP_ERR_NO_MEM;
        const bool ok =
            cJSON_AddStringToObject(object, "deck_id", snap.deck_id.c_str()) != nullptr &&
            cJSON_AddNumberToObject(
                object, "content_revision", static_cast<double>(snap.content_revision)) != nullptr &&
            cJSON_AddNumberToObject(
                object, "pack_revision", static_cast<double>(snap.pack_revision)) != nullptr &&
            cJSON_AddStringToObject(object, "sha256", snap.sha256.c_str()) != nullptr;
        if (!ok) {
            cJSON_Delete(object);
            return ESP_ERR_NO_MEM;
        }
        if (!cJSON_AddItemToArray(snapshot, object)) {
            cJSON_Delete(object);
            return ESP_ERR_NO_MEM;
        }
    }
    cJSON* items = cJSON_AddArrayToObject(document.root(), "items");
    if (items == nullptr) return ESP_ERR_NO_MEM;
    for (const SessionItem& item : request.items) {
        cJSON* object = cJSON_CreateObject();
        if (object == nullptr) return ESP_ERR_NO_MEM;
        const bool ok =
            cJSON_AddStringToObject(object, "item_id", item.item_id.c_str()) != nullptr &&
            cJSON_AddStringToObject(object, "deck_id", item.deck_id.c_str()) != nullptr &&
            cJSON_AddNumberToObject(
                object, "ordinal", static_cast<double>(item.ordinal)) != nullptr;
        if (!ok) {
            cJSON_Delete(object);
            return ESP_ERR_NO_MEM;
        }
        if (!cJSON_AddItemToArray(items, object)) {
            cJSON_Delete(object);
            return ESP_ERR_NO_MEM;
        }
    }
    return RenderToBody(document.root(), kMaxLocalSessionRequestBytes, body);
}

esp_err_t BuildObservationBatchRequest(
    const ObservationBatchRequest& request,
    std::string* body)
{
    if (body == nullptr || !ValidateBatchRequest(request)) {
        return ESP_ERR_INVALID_ARG;
    }
    JsonDocument document(cJSON_CreateObject());
    if (document.root() == nullptr) return ESP_ERR_NO_MEM;
    esp_err_t result = AddLocalSessionMetadata(document.root(), request.metadata);
    if (result != ESP_OK) return result;
    if (cJSON_AddStringToObject(
            document.root(), "session_id", request.session_id.c_str()) == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON* observations = cJSON_AddArrayToObject(document.root(), "observations");
    if (observations == nullptr) return ESP_ERR_NO_MEM;
    for (const BatchObservation& observation : request.observations) {
        cJSON* object = cJSON_CreateObject();
        if (object == nullptr) return ESP_ERR_NO_MEM;
        const bool ok =
            cJSON_AddStringToObject(
                object, "request_id", observation.request_id.c_str()) != nullptr &&
            cJSON_AddNumberToObject(
                object, "sequence", static_cast<double>(observation.sequence)) != nullptr &&
            cJSON_AddStringToObject(object, "item_id", observation.item_id.c_str()) != nullptr &&
            cJSON_AddStringToObject(
                object, "action", ObservationActionName(observation.action)) != nullptr &&
            cJSON_AddStringToObject(object, "mode", ModeName(observation.mode)) != nullptr &&
            cJSON_AddStringToObject(
                object, "occurred_at", observation.occurred_at.c_str()) != nullptr;
        if (!ok) {
            cJSON_Delete(object);
            return ESP_ERR_NO_MEM;
        }
        if (!cJSON_AddItemToArray(observations, object)) {
            cJSON_Delete(object);
            return ESP_ERR_NO_MEM;
        }
    }
    return RenderToBody(document.root(), kMaxObservationBatchRequestBytes, body);
}

esp_err_t ParseLocalSessionRegistrationResponse(
    const std::string& body,
    const std::string& expected_request_id,
    const std::string& expected_session_id,
    LocalSessionRegistrationData* data,
    v3::Error* error)
{
    if (data == nullptr || error == nullptr || !IsUuid(expected_session_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    const esp_err_t envelope_result =
        ParseEnvelope(document.root(), expected_request_id, &payload, error);
    if (envelope_result != ESP_OK) return envelope_result;
    if (!HasExactlyKeys(payload, {"session_id", "registered", "replayed"})) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->session_id = StringField(payload, "session_id");
    bool registered = false;
    if (!IsUuid(data->session_id) || data->session_id != expected_session_id ||
        !BoolField(payload, "registered", &registered) || !registered ||
        !BoolField(payload, "replayed", &data->replayed)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->registered = registered;
    return ESP_OK;
}

esp_err_t ValidateObservationBatchReceipt(
    const ObservationBatchData& data,
    const ObservationBatchRequest& expected)
{
    if (!IsUuid(data.session_id) || data.session_id != expected.session_id) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (data.accepted.size() > expected.observations.size() ||
        data.accepted.size() > kMaxObservationBatchItems) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    // Empty accepted + null failure is never a successful empty batch.
    if (data.accepted.empty() && !data.has_failure) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    // accepted must be the exact input prefix (request_id + sequence).
    for (size_t index = 0; index < data.accepted.size(); ++index) {
        const auto& accepted = data.accepted[index];
        const auto& input = expected.observations[index];
        if (accepted.request_id != input.request_id ||
            accepted.sequence != input.sequence) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    // Accepted sequences must be consecutive (defensive; also implied by prefix).
    for (size_t index = 1; index < data.accepted.size(); ++index) {
        if (data.accepted[index].sequence != data.accepted[index - 1].sequence + 1) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (data.accepted.size() == expected.observations.size()) {
        // Full success: failure must be null.
        return data.has_failure ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
    }
    // A partial result must carry a failure for the immediately following input.
    if (!data.has_failure) return ESP_ERR_INVALID_RESPONSE;
    const auto& next = expected.observations[data.accepted.size()];
    if (data.failure.request_id != next.request_id ||
        data.failure.sequence != next.sequence) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t ParseObservationBatchResponse(
    const std::string& body,
    const std::string& expected_request_id,
    const ObservationBatchRequest& expected,
    ObservationBatchData* data,
    v3::Error* error)
{
    if (data == nullptr || error == nullptr || !IsUuid(expected.session_id) ||
        expected.observations.empty() ||
        expected.observations.size() > kMaxObservationBatchItems) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = {};
    JsonDocument document(body);
    cJSON* payload = nullptr;
    const esp_err_t envelope_result =
        ParseEnvelope(document.root(), expected_request_id, &payload, error);
    if (envelope_result != ESP_OK) return envelope_result;
    if (!HasExactlyKeys(payload, {"session_id", "accepted", "failure"})) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->session_id = StringField(payload, "session_id");
    if (!IsUuid(data->session_id)) return ESP_ERR_INVALID_RESPONSE;

    cJSON* accepted = cJSON_GetObjectItemCaseSensitive(payload, "accepted");
    const int accepted_count = cJSON_GetArraySize(accepted);
    if (!cJSON_IsArray(accepted) || accepted_count < 0 ||
        static_cast<size_t>(accepted_count) > kMaxObservationBatchItems) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data->accepted.reserve(static_cast<size_t>(accepted_count));
    for (int index = 0; index < accepted_count; ++index) {
        cJSON* object = cJSON_GetArrayItem(accepted, index);
        BatchAcceptedItem item;
        if (!HasExactlyKeys(
                object, {"request_id", "observation_id", "sequence", "replayed"}) ||
            !SafeIdField(object, "request_id", 16, 64, &item.request_id) ||
            !IsUuid(StringField(object, "observation_id")) ||
            !U64Field(object, "sequence", &item.sequence) ||
            !BoolField(object, "replayed", &item.replayed)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        item.observation_id = StringField(object, "observation_id");
        data->accepted.push_back(std::move(item));
    }

    cJSON* failure = cJSON_GetObjectItemCaseSensitive(payload, "failure");
    if (cJSON_IsNull(failure)) {
        data->has_failure = false;
    } else {
        BatchFailure item;
        if (!HasExactlyKeys(
                failure, {"request_id", "sequence", "code", "retryable"}) ||
            !SafeIdField(failure, "request_id", 16, 64, &item.request_id) ||
            !U64Field(failure, "sequence", &item.sequence) ||
            !BoolField(failure, "retryable", &item.retryable)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        item.code = StringField(failure, "code");
        // Preserve unknown server error codes verbatim; only bound the shape.
        if (item.code.empty() || item.code.size() > 64 || ContainsNul(item.code)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        data->failure = std::move(item);
        data->has_failure = true;
    }
    // Only a fully validated receipt may be returned as an ACK-able result.
    const esp_err_t receipt_result = ValidateObservationBatchReceipt(*data, expected);
    if (receipt_result != ESP_OK) {
        *data = {};
        return receipt_result;
    }
    return ESP_OK;
}

}  // namespace wqn::protocol::word_study_v1
