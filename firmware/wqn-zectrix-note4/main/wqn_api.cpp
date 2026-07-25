#include "wqn_api.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "config.h"
#include "device_protocol/note_study.h"
#include "device_protocol/word_study.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "services/connectivity_service.h"
#include "storage.h"
#include "text_render.h"

namespace {

constexpr char kTag[] = "wqn_api";
constexpr int kHttpTimeoutMs = 10000;
constexpr int kWordSessionHttpTimeoutMs = 30000;
constexpr TickType_t kWifiConnectTimeout = pdMS_TO_TICKS(30000);
constexpr TickType_t kSntpSyncTimeout = pdMS_TO_TICKS(15000);
constexpr TickType_t kPollDelay = pdMS_TO_TICKS(2000);
constexpr int kPollAttempts = 60;
constexpr size_t kProblemPreviewChars = 240;
constexpr std::time_t kMinReasonableUnixTime = 1704067200;  // 2024-01-01 UTC

bool GetRequiredSafeUint64(cJSON* object, const char* key, uint64_t* value)
{
    if (!cJSON_IsObject(object) || key == nullptr || value == nullptr) {
        return false;
    }
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0 ||
        item->valuedouble > static_cast<double>(wqn::protocol::v3::kMaxSafeJsonInteger) ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    *value = static_cast<uint64_t>(item->valuedouble);
    return true;
}

bool GetRequiredSafeUint32(cJSON* object, const char* key, uint32_t* value)
{
    uint64_t parsed = 0;
    if (!GetRequiredSafeUint64(object, key, &parsed) ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

bool IsLowercaseSha256(const std::string& value)
{
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::string StableRequestIdForBody(const std::string& body)
{
    std::array<unsigned char, 32> digest = {};
    if (mbedtls_sha256(
            reinterpret_cast<const unsigned char*>(body.data()),
            body.size(),
            digest.data(),
            0) != 0) {
        return "";
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string request_id = "req_";
    request_id.reserve(52);
    for (size_t index = 0; index < 24; ++index) {
        request_id.push_back(kHex[digest[index] >> 4]);
        request_id.push_back(kHex[digest[index] & 0x0f]);
    }
    return request_id;
}

class JsonDocument {
public:
    explicit JsonDocument(const std::string& payload) : root_(cJSON_Parse(payload.c_str())) {}
    ~JsonDocument() { cJSON_Delete(root_); }

    cJSON* root() const { return root_; }
    bool ok() const { return root_ != nullptr; }

private:
    cJSON* root_ = nullptr;
};

void FeedTaskWatchdogIfSubscribed()
{
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_reset());
    }
}

void DelayAndFeedWatchdog(TickType_t delay)
{
    FeedTaskWatchdogIfSubscribed();
    vTaskDelay(delay);
    FeedTaskWatchdogIfSubscribed();
}

std::string BuildMacAddress()
{
    std::array<uint8_t, 6> mac = {};
    const esp_err_t result = esp_read_mac(mac.data(), ESP_MAC_WIFI_STA);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "MAC read failed for poll URL: %s", esp_err_to_name(result));
        return "";
    }

    char buffer[18] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]);
    return std::string(buffer);
}

std::string UrlEncode(const std::string& value)
{
    std::string encoded;
    encoded.reserve(value.size() * 3);
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char c : value) {
        const bool safe =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' ||
            c == '_' ||
            c == '.' ||
            c == '~';
        if (safe) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[c >> 4]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return encoded;
}

std::string BuildUrl(const std::string& path)
{
    std::string url = WQN_API_BASE;
    if (!url.empty() && url.back() == '/' && path[0] == '/') {
        url.pop_back();
    } else if (!url.empty() && url.back() != '/' && path[0] != '/') {
        url.push_back('/');
    }
    url += path;
    return url;
}

bool IsClockReasonable()
{
    std::time_t now = 0;
    std::time(&now);
    return now >= kMinReasonableUnixTime;
}

esp_err_t EnsureClockSyncedForHttps()
{
    if (IsClockReasonable()) {
        return ESP_OK;
    }

    ESP_LOGI(kTag, "starting SNTP sync before HTTPS requests");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t result = esp_netif_sntp_init(&config);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }

    FeedTaskWatchdogIfSubscribed();
    result = esp_netif_sntp_sync_wait(kSntpSyncTimeout);
    FeedTaskWatchdogIfSubscribed();
    if (result != ESP_OK) {
        return result;
    }

    std::time_t now = 0;
    std::time(&now);
    ESP_LOGI(kTag, "SNTP synced: unix_time=%lld", static_cast<long long>(now));
    return IsClockReasonable() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t WaitForNetworkReadyForHttps()
{
    ESP_LOGI(kTag, "waiting for WiFi before WQN API request");
    FeedTaskWatchdogIfSubscribed();
    ESP_RETURN_ON_ERROR(wqn::services::WaitForConnectivity(kWifiConnectTimeout), kTag, "wait for WiFi");
    FeedTaskWatchdogIfSubscribed();
    ESP_RETURN_ON_ERROR(EnsureClockSyncedForHttps(), kTag, "sync clock for HTTPS");
    FeedTaskWatchdogIfSubscribed();
    return ESP_OK;
}

esp_err_t HttpRequest(
    const char* method,
    const std::string& url,
    const std::string* bearer_token,
    const std::string* request_body,
    int* status_code,
    std::string* response_body,
    const char* protocol = nullptr,
    const std::string* request_id = nullptr,
    int timeout_ms = kHttpTimeoutMs)
{
    if (status_code == nullptr || response_body == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    response_body->clear();
    const bool is_post = std::strcmp(method, "POST") == 0;
    const std::string empty_post_body = "{}";
    const std::string& post_body = request_body != nullptr ? *request_body : empty_post_body;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    config.timeout_ms = timeout_ms;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_http_client_set_header(client, "Accept", "application/json");
    if (result == ESP_OK && bearer_token != nullptr && !bearer_token->empty()) {
        const std::string auth = "Bearer " + *bearer_token;
        result = esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    if (result == ESP_OK && std::strcmp(method, "POST") == 0) {
        result = esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (result == ESP_OK && protocol != nullptr) {
        result = esp_http_client_set_header(client, "X-WQN-Protocol", protocol);
    }
    if (result == ESP_OK && request_id != nullptr && !request_id->empty()) {
        result = esp_http_client_set_header(client, "X-WQN-Request-Id", request_id->c_str());
    }
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }

    result = esp_http_client_open(client, is_post ? post_body.size() : 0);
    FeedTaskWatchdogIfSubscribed();
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }
    if (is_post) {
        const int written = esp_http_client_write(client, post_body.c_str(), post_body.size());
        FeedTaskWatchdogIfSubscribed();
        if (written < 0 || static_cast<size_t>(written) != post_body.size()) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    const int content_length = esp_http_client_fetch_headers(client);
    *status_code = esp_http_client_get_status_code(client);
    if (content_length < 0) {
        ESP_LOGW(kTag, "HTTP response has unknown content length");
    }

    std::array<char, 512> buffer = {};
    while (true) {
        const int read = esp_http_client_read(client, buffer.data(), buffer.size() - 1);
        if (read < 0) {
            result = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        response_body->append(buffer.data(), read);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

esp_err_t HttpBinaryPost(
    const std::string& url,
    const std::string& bearer_token,
    const uint8_t* request_body,
    size_t request_body_size,
    int duration_ms,
    const std::string& conversation_id,
    const std::string& tier,
    int* status_code,
    std::string* response_body)
{
    if (status_code == nullptr || response_body == nullptr || (request_body == nullptr && request_body_size > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    response_body->clear();
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 600000;  // 10 minutes: covers ASR (up to 90s poll + submit) + LLM (up to 6min)
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_http_client_set_header(client, "Accept", "application/json");
    if (result == ESP_OK) {
        const std::string auth = "Bearer " + bearer_token;
        result = esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "X-WQN-Audio-Sample-Rate", "16000");
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "X-WQN-Audio-Sample-Format", "s16le");
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "X-WQN-Audio-Channels", "1");
    }
    char duration_header[16] = {};
    std::snprintf(duration_header, sizeof(duration_header), "%d", duration_ms);
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "X-WQN-Audio-Duration-Ms", duration_header);
    }
    if (result == ESP_OK && !conversation_id.empty()) {
        result = esp_http_client_set_header(client, "X-WQN-Conversation-Id", conversation_id.c_str());
    }
    if (result == ESP_OK && !tier.empty()) {
        result = esp_http_client_set_header(client, "X-WQN-Ai-Tier", tier.c_str());
    }
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }

    result = esp_http_client_open(client, request_body_size);
    FeedTaskWatchdogIfSubscribed();
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }

    size_t written_total = 0;
    while (written_total < request_body_size) {
        const size_t remaining = request_body_size - written_total;
        const int written = esp_http_client_write(
            client,
            reinterpret_cast<const char*>(request_body + written_total),
            remaining > 2048 ? 2048 : remaining);
        if (written <= 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        written_total += static_cast<size_t>(written);
        FeedTaskWatchdogIfSubscribed();
    }

    const int content_length = esp_http_client_fetch_headers(client);
    FeedTaskWatchdogIfSubscribed();
    *status_code = esp_http_client_get_status_code(client);
    if (content_length < 0) {
        ESP_LOGW(kTag, "AI HTTP response has unknown content length");
    }

    std::array<char, 512> buffer = {};
    while (true) {
        const int read = esp_http_client_read(client, buffer.data(), buffer.size() - 1);
        if (read < 0) {
            result = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        response_body->append(buffer.data(), read);
        FeedTaskWatchdogIfSubscribed();
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

bool GetSuccess(cJSON* root)
{
    cJSON* success = cJSON_GetObjectItemCaseSensitive(root, "success");
    return cJSON_IsBool(success) && cJSON_IsTrue(success);
}

std::string TruncateForLog(const std::string& value, size_t max_chars)
{
    if (value.size() <= max_chars) {
        return value;
    }
    return value.substr(0, max_chars) + "...";
}

std::string GetOptionalString(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

int GetOptionalInt(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

std::string BuildApiErrorMessage(cJSON* root, int status_code)
{
    std::string code;
    std::string message;
    cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
        code = GetOptionalString(error, "code");
        message = GetOptionalString(error, "message");
    }
    if (message.empty()) {
        message = GetOptionalString(root, "message");
    }
    if (message.empty() && !code.empty()) {
        message = code;
    }
    if (message.empty()) {
        char fallback[48] = {};
        std::snprintf(fallback, sizeof(fallback), "HTTP %d", status_code);
        message = fallback;
    }
    if (!code.empty()) {
        return code + ": " + message;
    }
    return message;
}

bool GetOptionalBool(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(item) && cJSON_IsTrue(item);
}

bool IsUnsupportedStatus(int status_code)
{
    return status_code == 404 || status_code == 501;
}

esp_err_t ValidateTokenOrClear(const std::string& token, const char* operation)
{
    if (!wqn::IsValidAccessToken(token)) {
        ESP_LOGW(kTag, "%s token shape invalid; clearing stored token", operation);
        ESP_RETURN_ON_ERROR(wqn::ClearAccessToken(), kTag, "clear invalid token");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t ClearTokenOnUnauthorized(const char* operation)
{
    ESP_LOGW(kTag, "%s unauthorized; clearing stored token", operation);
    ESP_RETURN_ON_ERROR(wqn::ClearAccessToken(), kTag, "clear unauthorized token");
    return ESP_ERR_INVALID_STATE;
}

void ParseAssetManifest(cJSON* array, std::vector<wqn::WqnAssetManifestItem>* manifest)
{
    if (manifest == nullptr) {
        return;
    }
    manifest->clear();
    if (!cJSON_IsArray(array)) {
        return;
    }

    const int count = cJSON_GetArraySize(array);
    manifest->reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        wqn::WqnAssetManifestItem asset;
        asset.role = GetOptionalString(item, "role");
        asset.kind = GetOptionalString(item, "kind");
        asset.mime_type = GetOptionalString(item, "mime_type");
        asset.url = GetOptionalString(item, "url");
        asset.sha256 = GetOptionalString(item, "sha256");
        asset.width = GetOptionalInt(item, "width");
        asset.height = GetOptionalInt(item, "height");
        asset.bytes = GetOptionalInt(item, "bytes");
        manifest->push_back(std::move(asset));
    }
}

void ParseAiActions(cJSON* array, std::vector<wqn::WqnAiAction>* actions)
{
    if (actions == nullptr) {
        return;
    }
    actions->clear();
    if (!cJSON_IsArray(array)) {
        return;
    }

    const int count = cJSON_GetArraySize(array);
    actions->reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        wqn::WqnAiAction action;
        action.type = GetOptionalString(item, "type");
        action.notebook_id = GetOptionalString(item, "notebook_id");
        action.note_id = GetOptionalString(item, "note_id");
        action.todo_id = GetOptionalString(item, "todo_id");
        action.word_id = GetOptionalString(item, "word_id");
        if (action.word_id.empty()) {
            action.word_id = GetOptionalString(item, "word_entry_id");
        }
        action.deck_id = GetOptionalString(item, "deck_id");
        action.word = GetOptionalString(item, "word");
        action.problem_set_id = GetOptionalString(item, "problem_set_id");
        action.problem_id = GetOptionalString(item, "problem_id");
        action.title = GetOptionalString(item, "title");
        action.status = GetOptionalString(item, "status");
        action.outcome = GetOptionalString(item, "outcome");
        action.due_at = GetOptionalString(item, "due_at");
        action.reminder_at = GetOptionalString(item, "reminder_at");
        if (!action.type.empty()) {
            actions->push_back(std::move(action));
        }
    }
}

void ParseAiStatusTrace(cJSON* array, std::vector<wqn::WqnAiStatusTraceItem>* trace)
{
    if (trace == nullptr) {
        return;
    }
    trace->clear();
    if (!cJSON_IsArray(array)) {
        return;
    }

    const int count = cJSON_GetArraySize(array);
    trace->reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        wqn::WqnAiStatusTraceItem parsed;
        parsed.stage = GetOptionalString(item, "stage");
        parsed.status = GetOptionalString(item, "status");
        parsed.detail = GetOptionalString(item, "detail");
        parsed.elapsed_ms = GetOptionalInt(item, "elapsed_ms");
        if (!parsed.stage.empty()) {
            trace->push_back(std::move(parsed));
        }
    }
}

void ParseAiAsrSummary(cJSON* object, wqn::WqnAiAsrSummary* asr)
{
    if (asr == nullptr) {
        return;
    }
    *asr = wqn::WqnAiAsrSummary{};
    if (!cJSON_IsObject(object)) {
        return;
    }

    asr->provider = GetOptionalString(object, "provider");
    asr->model = GetOptionalString(object, "model");
    asr->status = GetOptionalString(object, "status");
    asr->text = GetOptionalString(object, "text");
    asr->request_id = GetOptionalString(object, "request_id");
    asr->elapsed_ms = GetOptionalInt(object, "elapsed_ms");
}

void ParseAiFunctionCalls(cJSON* array, std::vector<wqn::WqnAiFunctionCallSummary>* calls)
{
    if (calls == nullptr) {
        return;
    }
    calls->clear();
    if (!cJSON_IsArray(array)) {
        return;
    }

    const int count = cJSON_GetArraySize(array);
    calls->reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        wqn::WqnAiFunctionCallSummary call;
        call.name = GetOptionalString(item, "name");
        call.status = GetOptionalString(item, "status");
        call.display = GetOptionalString(item, "display");
        call.action_type = GetOptionalString(item, "action_type");
        call.title = GetOptionalString(item, "title");
        if (!call.name.empty() || !call.display.empty()) {
            calls->push_back(std::move(call));
        }
    }
}

esp_err_t ParseTodoObject(cJSON* item, int index, wqn::WqnTodoItem* todo)
{
    if (!cJSON_IsObject(item) || todo == nullptr) {
        ESP_LOGW(kTag, "todo response contains non-object at index=%d", index);
        return ESP_FAIL;
    }

    wqn::WqnTodoItem parsed;
    parsed.id = GetOptionalString(item, "id");
    parsed.title = GetOptionalString(item, "title");
    parsed.status = GetOptionalString(item, "status");
    parsed.priority = GetOptionalString(item, "priority");
    parsed.due_at = GetOptionalString(item, "due_at");
    parsed.reminder_at = GetOptionalString(item, "reminder_at");
    parsed.subject_name = GetOptionalString(item, "subject_name");
    parsed.updated_at = GetOptionalString(item, "updated_at");
    parsed.completed_at = GetOptionalString(item, "completed_at");

    if (parsed.id.empty() || parsed.title.empty()) {
        ESP_LOGW(kTag, "todo item missing id/title at index=%d", index);
        return ESP_FAIL;
    }
    if (parsed.status.empty()) {
        parsed.status = "pending";
    }
    if (parsed.status != "pending" && parsed.status != "completed" && parsed.status != "cancelled") {
        ESP_LOGW(kTag, "todo item has unsupported status=%s at index=%d", parsed.status.c_str(), index);
        return ESP_FAIL;
    }

    *todo = std::move(parsed);
    return ESP_OK;
}

int FindNearestDueTodoIndex(const std::vector<wqn::WqnTodoItem>& todos, const std::string& server_time)
{
    if (todos.empty()) {
        return -1;
    }

    int nearest_future = -1;
    std::string nearest_future_due;
    int nearest_past = -1;
    std::string nearest_past_due;
    for (size_t i = 0; i < todos.size(); ++i) {
        const std::string& due = todos[i].due_at;
        if (due.empty()) {
            continue;
        }
        if (!server_time.empty() && due < server_time) {
            if (nearest_past < 0 || due > nearest_past_due) {
                nearest_past = static_cast<int>(i);
                nearest_past_due = due;
            }
        } else if (nearest_future < 0 || due < nearest_future_due) {
            nearest_future = static_cast<int>(i);
            nearest_future_due = due;
        }
    }
    if (nearest_future >= 0) {
        return nearest_future;
    }
    if (nearest_past >= 0) {
        return nearest_past;
    }
    return 0;
}

bool IsValidWordStatus(const std::string& status)
{
    return status.empty() ||
           status == "new" ||
           status == "learning" ||
           status == "review" ||
           status == "mastered";
}

esp_err_t ParseWordEntryObject(cJSON* item, int index, wqn::WqnWordEntry* entry)
{
    if (!cJSON_IsObject(item) || entry == nullptr) {
        ESP_LOGW(kTag, "word entry response contains non-object at index=%d", index);
        return ESP_FAIL;
    }

    wqn::WqnWordEntry parsed;
    parsed.id = GetOptionalString(item, "id");
    parsed.deck_id = GetOptionalString(item, "deck_id");
    parsed.word = GetOptionalString(item, "word");
    parsed.normalized_word = GetOptionalString(item, "normalized_word");
    parsed.phonetic = GetOptionalString(item, "phonetic");
    parsed.meaning = GetOptionalString(item, "meaning");
    parsed.example = GetOptionalString(item, "example");
    parsed.example_translation = GetOptionalString(item, "example_translation");
    parsed.part_of_speech = GetOptionalString(item, "part_of_speech");
    parsed.status = GetOptionalString(item, "status");
    parsed.due_at = GetOptionalString(item, "due_at");
    parsed.deleted = GetOptionalBool(item, "deleted");
    parsed.revision = GetOptionalInt(item, "revision");

    if (parsed.id.empty()) {
        ESP_LOGW(kTag, "word entry missing id at index=%d", index);
        return ESP_FAIL;
    }
    if (!parsed.deleted && (parsed.word.empty() || parsed.meaning.empty())) {
        ESP_LOGW(kTag, "word entry missing word/meaning at index=%d", index);
        return ESP_FAIL;
    }
    if (!IsValidWordStatus(parsed.status)) {
        ESP_LOGW(kTag, "word entry unsupported status=%s at index=%d", parsed.status.c_str(), index);
        return ESP_FAIL;
    }

    *entry = std::move(parsed);
    return ESP_OK;
}

esp_err_t ParseWordPackManifestItem(cJSON* item, int index, wqn::WqnWordPackManifestItem* pack)
{
    if (!cJSON_IsObject(item) || pack == nullptr) {
        ESP_LOGW(kTag, "word pack manifest contains non-object at index=%d", index);
        return ESP_FAIL;
    }

    wqn::WqnWordPackManifestItem parsed;
    parsed.pack_id = GetOptionalString(item, "pack_id");
    if (parsed.pack_id.empty()) {
        parsed.pack_id = GetOptionalString(item, "id");
    }
    parsed.deck_id = GetOptionalString(item, "deck_id");
    parsed.title = GetOptionalString(item, "title");
    parsed.format = GetOptionalString(item, "format");
    parsed.compression = GetOptionalString(item, "compression");
    parsed.sha256 = GetOptionalString(item, "sha256");
    parsed.download_url = GetOptionalString(item, "download_url");

    if (!GetRequiredSafeUint64(item, "revision", &parsed.revision)) {
        ESP_LOGW(kTag, "word pack revision is not an exact safe integer at index=%d", index);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!GetRequiredSafeUint64(item, "content_revision", &parsed.content_revision)) {
        parsed.content_revision = parsed.revision;
    }
    if (!GetRequiredSafeUint64(item, "pack_revision", &parsed.pack_revision)) {
        parsed.pack_revision = parsed.revision;
    }
    if (!GetRequiredSafeUint64(item, "change_sequence", &parsed.change_sequence)) {
        parsed.change_sequence = 0;
    }
    if (!GetRequiredSafeUint32(item, "schema_version", &parsed.schema_version) ||
        !GetRequiredSafeUint32(item, "entry_count", &parsed.entry_count) ||
        !GetRequiredSafeUint32(item, "byte_size", &parsed.byte_size)) {
        ESP_LOGW(kTag, "word pack manifest has invalid bounded counters at index=%d", index);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (parsed.pack_id.empty() || parsed.deck_id.empty() ||
        !IsLowercaseSha256(parsed.sha256) || parsed.download_url.empty() ||
        parsed.schema_version != wqn::protocol::word_study_v1::kPackSchemaVersion ||
        parsed.format != "jsonl" || parsed.compression != "none" ||
        parsed.entry_count > wqn::protocol::word_study_v1::kMaxPackEntries ||
        parsed.byte_size == 0 ||
        parsed.byte_size > wqn::protocol::word_study_v1::kMaxPackBytes) {
        ESP_LOGW(kTag, "word pack manifest item violates v2 contract at index=%d", index);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *pack = std::move(parsed);
    return ESP_OK;
}

void ParseLooseWordEntry(cJSON* item, wqn::WqnWordEntry* word)
{
    if (!cJSON_IsObject(item) || word == nullptr) {
        return;
    }
    word->id = GetOptionalString(item, "id");
    if (word->id.empty()) {
        word->id = GetOptionalString(item, "word_id");
    }
    if (word->id.empty()) {
        word->id = GetOptionalString(item, "word_entry_id");
    }
    word->deck_id = GetOptionalString(item, "deck_id");
    word->word = GetOptionalString(item, "word");
    word->normalized_word = GetOptionalString(item, "normalized_word");
    word->phonetic = GetOptionalString(item, "phonetic");
    word->meaning = GetOptionalString(item, "meaning");
    word->example = GetOptionalString(item, "example");
    word->example_translation = GetOptionalString(item, "example_translation");
    word->part_of_speech = GetOptionalString(item, "part_of_speech");
    word->status = GetOptionalString(item, "status");
    word->due_at = GetOptionalString(item, "due_at");
    word->revision = GetOptionalInt(item, "revision");
}

esp_err_t ParseProblemObject(cJSON* item, int index, wqn::WqnProblem* problem)
{
    if (!cJSON_IsObject(item) || problem == nullptr) {
        ESP_LOGW(kTag, "problem response contains non-object at index=%d", index);
        return ESP_FAIL;
    }

    wqn::WqnProblem parsed;
    parsed.id = GetOptionalString(item, "id");
    parsed.title = GetOptionalString(item, "title");
    parsed.problem_type = GetOptionalString(item, "problem_type");
    if (parsed.problem_type.empty()) {
        parsed.problem_type = GetOptionalString(item, "type");
    }
    parsed.status = GetOptionalString(item, "status");
    parsed.subject_id = GetOptionalString(item, "subject_id");
    parsed.subject_name = GetOptionalString(item, "subject_name");
    parsed.updated_at = GetOptionalString(item, "updated_at");
    parsed.next_review_at = GetOptionalString(item, "next_review_at");
    parsed.content_text = GetOptionalString(item, "content_text");
    if (parsed.content_text.empty()) {
        parsed.content_text = wqn::HtmlToPlainText(GetOptionalString(item, "content"));
    }
    parsed.solution_text = GetOptionalString(item, "solution_text");
    if (parsed.solution_text.empty()) {
        parsed.solution_text = wqn::HtmlToPlainText(GetOptionalString(item, "solution"));
    }
    ParseAssetManifest(cJSON_GetObjectItemCaseSensitive(item, "assets"), &parsed.assets);
    if (parsed.assets.empty()) {
        ParseAssetManifest(cJSON_GetObjectItemCaseSensitive(item, "attachments"), &parsed.assets);
    }
    ParseAssetManifest(cJSON_GetObjectItemCaseSensitive(item, "solution_assets"), &parsed.solution_assets);
    parsed.asset_count = static_cast<int>(parsed.assets.size());
    parsed.solution_asset_count = static_cast<int>(parsed.solution_assets.size());

    if (parsed.id.empty()) {
        ESP_LOGW(kTag, "problem detail missing id at index=%d", index);
        return ESP_FAIL;
    }

    *problem = std::move(parsed);
    return ESP_OK;
}

esp_err_t ParseSyncResponse(const std::string& body, std::vector<std::string>* due_problem_ids, int* total)
{
    if (due_problem_ids == nullptr || total == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    due_problem_ids->clear();
    *total = 0;

    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "sync response is not valid JSON");
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* due = cJSON_GetObjectItemCaseSensitive(data, "due_problems");
    cJSON* total_item = cJSON_GetObjectItemCaseSensitive(data, "total");
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data) || !cJSON_IsArray(due)) {
        ESP_LOGW(kTag, "sync response missing success/data/due_problems");
        return ESP_FAIL;
    }

    *total = cJSON_IsNumber(total_item) ? total_item->valueint : cJSON_GetArraySize(due);
    const int count = cJSON_GetArraySize(due);
    due_problem_ids->reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* id = cJSON_GetArrayItem(due, i);
        if (!cJSON_IsString(id) || id->valuestring == nullptr || std::strlen(id->valuestring) == 0) {
            ESP_LOGW(kTag, "sync response contains invalid due problem id at index=%d", i);
            return ESP_FAIL;
        }
        due_problem_ids->emplace_back(id->valuestring);
    }

    return ESP_OK;
}

std::string JoinProblemIdsForQuery(const std::vector<std::string>& ids)
{
    std::string joined;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            joined.push_back(',');
        }
        joined += UrlEncode(ids[i]);
    }
    return joined;
}

std::vector<std::string> SplitDebugProblemIds()
{
    std::vector<std::string> ids;
    const std::string raw = WQN_DEBUG_PROBLEM_IDS;
    size_t start = 0;
    while (start < raw.size()) {
        const size_t comma = raw.find(',', start);
        const size_t end = comma == std::string::npos ? raw.size() : comma;
        const std::string id = raw.substr(start, end - start);
        if (!id.empty()) {
            ids.push_back(id);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return ids;
}

esp_err_t ParseProblemsResponse(const std::string& body, std::vector<wqn::WqnProblem>* problems)
{
    if (problems == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    problems->clear();
    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "problems response is not valid JSON");
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* items = cJSON_GetObjectItemCaseSensitive(data, "problems");
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data) || !cJSON_IsArray(items)) {
        ESP_LOGW(kTag, "problems response missing success/data/problems");
        return ESP_FAIL;
    }

    const int count = cJSON_GetArraySize(items);
    problems->reserve(count);
    for (int i = 0; i < count; ++i) {
        wqn::WqnProblem problem;
        ESP_RETURN_ON_ERROR(ParseProblemObject(cJSON_GetArrayItem(items, i), i, &problem), kTag, "parse problem");
        problems->push_back(std::move(problem));
    }

    return ESP_OK;
}

esp_err_t ParseProblemIndexResponse(const std::string& body, wqn::WqnProblemIndexPage* page)
{
    if (page == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    page->problems.clear();
    page->next_cursor.clear();
    page->has_more = false;
    page->total = 0;

    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "problem-index response is not valid JSON");
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* items = cJSON_GetObjectItemCaseSensitive(data, "problems");
    if (!cJSON_IsArray(items)) {
        items = cJSON_GetObjectItemCaseSensitive(data, "items");
    }
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data) || !cJSON_IsArray(items)) {
        ESP_LOGW(kTag, "problem-index response missing success/data/problems");
        return ESP_FAIL;
    }

    page->next_cursor = GetOptionalString(data, "next_cursor");
    page->has_more = GetOptionalBool(data, "has_more");
    page->total = GetOptionalInt(data, "total");
    cJSON* pagination = cJSON_GetObjectItemCaseSensitive(data, "page");
    if (cJSON_IsObject(pagination)) {
        if (page->next_cursor.empty()) {
            page->next_cursor = GetOptionalString(pagination, "next_cursor");
        }
        page->has_more = page->has_more || GetOptionalBool(pagination, "has_more");
        if (page->total == 0) {
            page->total = GetOptionalInt(pagination, "total");
        }
    }

    const int count = cJSON_GetArraySize(items);
    page->problems.reserve(count);
    for (int i = 0; i < count; ++i) {
        wqn::WqnProblem problem;
        ESP_RETURN_ON_ERROR(ParseProblemObject(cJSON_GetArrayItem(items, i), i, &problem), kTag, "parse problem-index item");
        page->problems.push_back(std::move(problem));
    }

    return ESP_OK;
}

esp_err_t ParseTodoListResponseImpl(const std::string& body, wqn::WqnTodoListPage* page)
{
    if (page == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    page->todos.clear();
    page->previous_cursor.clear();
    page->next_cursor.clear();
    page->has_earlier = false;
    page->has_later = false;
    page->has_more = false;
    page->total = 0;
    page->server_time.clear();
    page->selected_todo_id.clear();
    page->selected_index = -1;

    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "todo list response is not valid JSON");
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* items = cJSON_GetObjectItemCaseSensitive(data, "todos");
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data) || !cJSON_IsArray(items)) {
        ESP_LOGW(kTag, "todo list response missing success/data/todos");
        return ESP_FAIL;
    }

    page->previous_cursor = GetOptionalString(data, "previous_cursor");
    page->next_cursor = GetOptionalString(data, "next_cursor");
    page->has_earlier = GetOptionalBool(data, "has_earlier");
    page->has_later = GetOptionalBool(data, "has_later");
    page->has_more = GetOptionalBool(data, "has_more");
    page->total = GetOptionalInt(data, "total");
    page->server_time = GetOptionalString(data, "server_time");
    page->selected_todo_id = GetOptionalString(data, "selected_todo_id");
    cJSON* selected_index = cJSON_GetObjectItemCaseSensitive(data, "selected_index");
    page->selected_index = cJSON_IsNumber(selected_index) ? selected_index->valueint : -1;

    const int count = std::min(cJSON_GetArraySize(items), 24);
    page->todos.reserve(count);
    for (int i = 0; i < count; ++i) {
        wqn::WqnTodoItem todo;
        const esp_err_t parse_result = ParseTodoObject(cJSON_GetArrayItem(items, i), i, &todo);
        if (parse_result != ESP_OK) {
            ESP_LOGW(kTag, "skip invalid todo item at index=%d", i);
            continue;
        }
        page->todos.push_back(std::move(todo));
    }
    if (page->total == 0) {
        page->total = cJSON_GetArraySize(items);
    }
    if (!page->selected_todo_id.empty()) {
        for (size_t i = 0; i < page->todos.size(); ++i) {
            if (page->todos[i].id == page->selected_todo_id) {
                page->selected_index = static_cast<int>(i);
                break;
            }
        }
    }
    if (page->selected_index < 0 || page->selected_index >= static_cast<int>(page->todos.size())) {
        page->selected_index = FindNearestDueTodoIndex(page->todos, page->server_time);
    }

    return ESP_OK;
}

esp_err_t ParseTodoCompleteResponseImpl(const std::string& body, wqn::WqnTodoItem* todo)
{
    if (todo == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *todo = wqn::WqnTodoItem{};

    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "todo complete response is not valid JSON");
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* item = cJSON_GetObjectItemCaseSensitive(data, "todo");
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data) || !cJSON_IsObject(item)) {
        ESP_LOGW(kTag, "todo complete response missing success/data/todo");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(ParseTodoObject(item, 0, todo), kTag, "parse completed todo");
    if (todo->status != "completed") {
        ESP_LOGW(kTag, "todo complete response returned status=%s", todo->status.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ParseWordSearchResponseImpl(const std::string& body, wqn::WqnWordSearchResult* result)
{
    if (result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = wqn::WqnWordSearchResult{};

    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "word search response is not valid JSON");
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* words = cJSON_GetObjectItemCaseSensitive(data, "words");
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data) || !cJSON_IsArray(words)) {
        ESP_LOGW(kTag, "word search response missing success/data/words");
        return ESP_FAIL;
    }

    result->prefix = GetOptionalString(data, "prefix");
    const int word_count = cJSON_GetArraySize(words);
    result->words.reserve(word_count);
    for (int i = 0; i < word_count; ++i) {
        wqn::WqnWordEntry word;
        const esp_err_t parsed = ParseWordEntryObject(cJSON_GetArrayItem(words, i), i, &word);
        if (parsed != ESP_OK) {
            ESP_LOGW(kTag, "skip invalid search word at index=%d", i);
            continue;
        }
        result->words.push_back(std::move(word));
    }

    cJSON* next_letters = cJSON_GetObjectItemCaseSensitive(data, "next_letters");
    if (next_letters != nullptr && !cJSON_IsArray(next_letters)) {
        ESP_LOGW(kTag, "word search response has invalid next_letters");
        return ESP_FAIL;
    }
    if (cJSON_IsArray(next_letters)) {
        const int letter_count = cJSON_GetArraySize(next_letters);
        result->next_letters.reserve(letter_count);
        for (int i = 0; i < letter_count; ++i) {
            cJSON* item = cJSON_GetArrayItem(next_letters, i);
            if (cJSON_IsString(item) && item->valuestring != nullptr && std::strlen(item->valuestring) > 0) {
                result->next_letters.emplace_back(item->valuestring);
            }
        }
    }

    return ESP_OK;
}

esp_err_t ParseWordPackManifestResponseImpl(const std::string& body, wqn::WqnWordPackManifest* manifest)
{
    if (manifest == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest = wqn::WqnWordPackManifest{};

    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "word pack manifest response is not valid JSON");
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* packs = cJSON_GetObjectItemCaseSensitive(data, "packs");
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data) || !cJSON_IsArray(packs)) {
        ESP_LOGW(kTag, "word pack manifest response missing success/data/packs");
        return ESP_FAIL;
    }

    manifest->server_time = GetOptionalString(data, "server_time");
    cJSON* cursor = cJSON_GetObjectItemCaseSensitive(data, "cursor");
    if (cursor != nullptr &&
        !GetRequiredSafeUint64(data, "cursor", &manifest->cursor)) {
        ESP_LOGW(kTag, "word pack manifest cursor is invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON* has_more = cJSON_GetObjectItemCaseSensitive(data, "has_more");
    if (has_more != nullptr && !cJSON_IsBool(has_more)) {
        ESP_LOGW(kTag, "word pack manifest has_more is invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }
    manifest->has_more = cJSON_IsTrue(has_more);
    const int count = cJSON_GetArraySize(packs);
    manifest->packs.reserve(count);
    for (int i = 0; i < count; ++i) {
        wqn::WqnWordPackManifestItem item;
        const esp_err_t parsed = ParseWordPackManifestItem(cJSON_GetArrayItem(packs, i), i, &item);
        if (parsed != ESP_OK) {
            ESP_LOGW(kTag, "reject invalid word pack manifest item at index=%d", i);
            return parsed;
        }
        manifest->packs.push_back(std::move(item));
    }
    return ESP_OK;
}

esp_err_t ParseWordAiLookupResponseImpl(const std::string& body, wqn::WqnWordAiLookupResult* result)
{
    if (result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = wqn::WqnWordAiLookupResult{};

    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "word AI lookup response is not valid JSON");
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* word = cJSON_GetObjectItemCaseSensitive(data, "lookup");
    if (!cJSON_IsObject(word)) {
        word = cJSON_GetObjectItemCaseSensitive(data, "word");
    }
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data) || !cJSON_IsObject(word)) {
        ESP_LOGW(kTag, "word AI lookup response missing success/data/lookup");
        return ESP_FAIL;
    }
    ParseLooseWordEntry(word, &result->word);
    if (result->word.word.empty()) {
        result->word.word = GetOptionalString(word, "query");
    }
    if (result->word.normalized_word.empty()) {
        result->word.normalized_word = result->word.word;
    }
    if (result->word.meaning.empty()) {
        ESP_LOGW(kTag, "word AI lookup response missing meaning");
        return ESP_FAIL;
    }
    if (result->word.status.empty()) {
        result->word.status = "new";
    }
    result->reply_text = GetOptionalString(data, "reply_text");
    return ESP_OK;
}

std::string BuildProblemIndexPath(const wqn::WqnProblemIndexRequest& request)
{
    std::string path = "/problem-index?limit=" + std::to_string(request.limit > 0 ? request.limit : 50);
    if (!request.cursor.empty()) {
        path += "&cursor=" + UrlEncode(request.cursor);
    }
    if (!request.status.empty()) {
        path += "&status=" + UrlEncode(request.status);
    }
    if (!request.subject_id.empty()) {
        path += "&subject_id=" + UrlEncode(request.subject_id);
    }
    return path;
}

std::string BuildTodoListPath(const wqn::WqnTodoTimelineRequest& request)
{
    const int limit = std::clamp(request.limit > 0 ? request.limit : 24, 1, 24);
    std::string path = "/todos?scope=timeline&status=pending&limit=" + std::to_string(limit);
    if (!request.cursor.empty()) {
        path += "&cursor=" + UrlEncode(request.cursor);
    }
    return path;
}

int ClampRequestLimit(int value, int fallback, int maximum)
{
    return std::clamp(value > 0 ? value : fallback, 1, maximum);
}

std::string BuildWordSearchPath(const wqn::WqnWordSearchRequest& request)
{
    const int limit = ClampRequestLimit(request.limit, 8, 50);
    std::string path = "/words/search?";
    if (!request.query.empty()) {
        path += "q=" + UrlEncode(request.query);
    } else {
        path += "prefix=" + UrlEncode(request.prefix);
    }
    path += "&limit=" + std::to_string(limit);
    return path;
}

std::string BuildWordPackDownloadUrl(const std::string& download_url)
{
    if (download_url.rfind("http://", 0) == 0 || download_url.rfind("https://", 0) == 0) {
        return download_url;
    }
    constexpr char kEsp32Prefix[] = "/api/esp32";
    if (download_url.rfind(kEsp32Prefix, 0) == 0) {
        return BuildUrl(download_url.substr(std::strlen(kEsp32Prefix)));
    }
    return BuildUrl(download_url);
}

esp_err_t BuildWordAiLookupBody(const wqn::WqnWordAiLookupRequest& request, std::string* body)
{
    if (body == nullptr || (request.query.empty() && request.prefix.empty())) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const std::string word = !request.query.empty() ? request.query : request.prefix;
    cJSON_AddStringToObject(root, "word", word.c_str());
    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    *body = rendered;
    cJSON_free(rendered);
    return ESP_OK;
}

void AddJsonString(cJSON* object, const char* key, const std::string& value)
{
    if (!value.empty()) {
        cJSON_AddStringToObject(object, key, value.c_str());
    }
}

esp_err_t BuildReviewCompleteBody(const std::vector<wqn::WqnReviewResult>& results, std::string* body)
{
    if (body == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    body->clear();

    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_CreateArray();
    if (root == nullptr || items == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "firmware_version", WQN_FIRMWARE_VERSION);
    cJSON_AddItemToObject(root, "results", items);

    for (const wqn::WqnReviewResult& result : results) {
        if (result.problem_id.empty() || result.selected_status.empty()) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        AddJsonString(item, "problem_id", result.problem_id);
        AddJsonString(item, "selected_status", result.selected_status);
        AddJsonString(item, "reviewed_at", result.reviewed_at);
        if (result.duration_ms > 0) {
            cJSON_AddNumberToObject(item, "duration_ms", result.duration_ms);
        }
        cJSON_AddItemToArray(items, item);
    }

    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    *body = rendered;
    cJSON_free(rendered);
    return ESP_OK;
}

esp_err_t BuildTodoCompleteBody(const std::string& todo_id, std::string* body)
{
    if (body == nullptr || todo_id.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    body->clear();

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "todo_id", todo_id.c_str());

    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    *body = rendered;
    cJSON_free(rendered);
    return ESP_OK;
}

void LogProblemSummary(const wqn::WqnProblem& problem, int index, int total)
{
    ESP_LOGI(kTag, "problem %d/%d id=%s type=%s", index, total, problem.id.c_str(), problem.problem_type.c_str());
    ESP_LOGI(kTag, "problem title: %s", problem.title.empty() ? "<untitled>" : problem.title.c_str());
    ESP_LOGI(kTag, "problem content: %s", TruncateForLog(problem.content_text, kProblemPreviewChars).c_str());
    if (!problem.solution_text.empty()) {
        ESP_LOGI(kTag, "problem solution: %s", TruncateForLog(problem.solution_text, kProblemPreviewChars).c_str());
    }
}

esp_err_t ParsePollResponse(const std::string& body, std::string* status, std::string* token)
{
    if (status == nullptr || token == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "poll response is not valid JSON");
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* status_item = cJSON_GetObjectItemCaseSensitive(data, "status");
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data) || !cJSON_IsString(status_item)) {
        ESP_LOGW(kTag, "poll response missing success/data/status");
        return ESP_FAIL;
    }

    *status = status_item->valuestring;
    token->clear();
    if (*status == "paired") {
        cJSON* token_item = cJSON_GetObjectItemCaseSensitive(data, "access_token");
        if (!cJSON_IsString(token_item)) {
            ESP_LOGW(kTag, "paired poll response missing access_token");
            return ESP_FAIL;
        }
        *token = token_item->valuestring;
        if (!wqn::IsValidAccessToken(*token)) {
            ESP_LOGW(kTag, "paired poll response has invalid access_token shape");
            token->clear();
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t PollPairingOnce(const std::string& mac_address, bool* paired)
{
    if (paired == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *paired = false;

    const std::string url = BuildUrl("/poll?mac_address=" + UrlEncode(mac_address));
    int status_code = 0;
    std::string body;
    ESP_RETURN_ON_ERROR(HttpRequest("GET", url, nullptr, nullptr, &status_code, &body), kTag, "HTTP poll");

    ESP_LOGI(kTag, "poll HTTP status=%d", status_code);
    if (status_code != 200) {
        return ESP_FAIL;
    }

    std::string status;
    std::string token;
    ESP_RETURN_ON_ERROR(ParsePollResponse(body, &status, &token), kTag, "parse poll response");

    if (status == "paired") {
        ESP_RETURN_ON_ERROR(wqn::SaveAccessToken(token), kTag, "save access token");
        ESP_LOGI(kTag, "pairing complete, token=%s", wqn::MaskTokenForLog(token).c_str());
        *paired = true;
    } else if (status == "no_pending" || status == "expired" || status == "already_paired") {
        // already_paired means a device row exists on the server but no fresh
        // pending pairing request has been issued from the web. The server
        // refuses to re-emit the token; the user must unpair from the web and
        // re-initiate pairing. Keep polling without saving anything.
        ESP_LOGI(kTag, "pairing status=%s", status.c_str());
    } else {
        ESP_LOGW(kTag, "unexpected pairing status=%s", status.c_str());
    }

    return ESP_OK;
}

}  // namespace

namespace wqn {

esp_err_t RunPairingFlowIfNeeded()
{
    std::string existing_token;
    ESP_RETURN_ON_ERROR(LoadAccessToken(&existing_token), kTag, "load access token");
    if (!existing_token.empty()) {
        if (IsValidAccessToken(existing_token)) {
            ESP_LOGI(kTag, "stored token present, pairing skipped: %s", MaskTokenForLog(existing_token).c_str());
            return ESP_OK;
        }

        ESP_LOGW(kTag, "stored token shape invalid, clearing");
        ESP_RETURN_ON_ERROR(ClearAccessToken(), kTag, "clear invalid token");
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for pairing");

    const std::string mac_address = BuildMacAddress();
    if (mac_address.empty()) {
        return ESP_FAIL;
    }

    ESP_LOGI(kTag, "pairing poll started for mac=%s", mac_address.c_str());
    for (int attempt = 1; attempt <= kPollAttempts; ++attempt) {
        FeedTaskWatchdogIfSubscribed();
        bool paired = false;
        const esp_err_t result = PollPairingOnce(mac_address, &paired);
        if (result == ESP_OK && paired) {
            return ESP_OK;
        }
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "poll attempt %d failed: %s", attempt, esp_err_to_name(result));
        }
        DelayAndFeedWatchdog(kPollDelay);
    }

    ESP_LOGW(kTag, "pairing poll timed out");
    return ESP_ERR_TIMEOUT;
}

esp_err_t StartDeviceClaimV3(
    const protocol::v3::RequestMetadata& metadata,
    const std::string& hardware_id,
    const std::string& device_public_key,
    protocol::v3::ClaimStartData* data,
    protocol::v3::Error* error)
{
    if (hardware_id.empty() || device_public_key.empty() || data == nullptr ||
        error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(
        WaitForNetworkReadyForHttps(), kTag, "prepare v3 claim start network");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::v3::BuildClaimStartRequest(
            metadata, hardware_id, device_public_key, &request_body),
        kTag,
        "build v3 claim start");
    int status_code = 0;
    std::string response_body;
    const esp_err_t request_result = HttpRequest(
        "POST",
        BuildUrl("/v3/claim/start"),
        nullptr,
        &request_body,
        &status_code,
        &response_body,
        protocol::v3::kProtocolHeader,
        &metadata.request_id);
    if (request_result != ESP_OK) {
        return request_result;
    }
    const esp_err_t parse_result = protocol::v3::ParseClaimStartResponse(
        response_body, metadata.request_id, data, error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "v3 claim start failed: status=%d code=%s retryable=%d",
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t PollDeviceClaimV3(
    const protocol::v3::RequestMetadata& metadata,
    const std::string& claim_id,
    protocol::v3::ClaimPollData* data,
    protocol::v3::Error* error)
{
    if (claim_id.empty() || data == nullptr || error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(
        WaitForNetworkReadyForHttps(), kTag, "prepare v3 claim poll network");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::v3::BuildClaimPollRequest(metadata, claim_id, &request_body),
        kTag,
        "build v3 claim poll");
    int status_code = 0;
    std::string response_body;
    const esp_err_t request_result = HttpRequest(
        "POST",
        BuildUrl("/v3/claim/poll"),
        nullptr,
        &request_body,
        &status_code,
        &response_body,
        protocol::v3::kProtocolHeader,
        &metadata.request_id);
    if (request_result != ESP_OK) {
        return request_result;
    }
    const esp_err_t parse_result = protocol::v3::ParseClaimPollResponse(
        response_body, metadata.request_id, data, error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "v3 claim poll failed: status=%d code=%s retryable=%d retry_after_ms=%u",
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0,
            static_cast<unsigned>(error->retry_after_ms));
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t BootstrapDeviceControlV3(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    protocol::v3::BootstrapData* data,
    protocol::v3::Error* error)
{
    if (token.empty() || data == nullptr || error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ValidateTokenOrClear(token, "v3 bootstrap"), kTag, "validate token");
    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare v3 bootstrap network");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::v3::BuildBootstrapRequest(metadata, &request_body),
        kTag,
        "build v3 bootstrap");
    int status_code = 0;
    std::string response_body;
    const std::string url = BuildUrl("/v3/bootstrap");
    const esp_err_t request_result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &response_body,
        protocol::v3::kProtocolHeader,
        &metadata.request_id);
    if (request_result != ESP_OK) {
        return request_result;
    }
    const esp_err_t parse_result = protocol::v3::ParseBootstrapResponse(
        response_body, metadata.request_id, data, error);
    if (status_code == 401) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ClearAccessToken());
        return ESP_ERR_INVALID_STATE;
    }
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "v3 bootstrap failed: status=%d code=%s retryable=%d",
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t SyncDeviceControlV3(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    protocol::v3::SyncData* data,
    protocol::v3::Error* error)
{
    if (token.empty() || data == nullptr || error == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ValidateTokenOrClear(token, "v3 sync"), kTag, "validate token");
    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare v3 sync network");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::v3::BuildSyncRequest(metadata, &request_body),
        kTag,
        "build v3 sync");
    int status_code = 0;
    std::string response_body;
    const std::string url = BuildUrl("/v3/sync");
    const esp_err_t request_result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &response_body,
        protocol::v3::kProtocolHeader,
        &metadata.request_id);
    if (request_result != ESP_OK) {
        return request_result;
    }
    const esp_err_t parse_result = protocol::v3::ParseSyncResponse(
        response_body, metadata.request_id, data, error);
    if (status_code == 401) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ClearAccessToken());
        return ESP_ERR_INVALID_STATE;
    }
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "v3 sync failed: status=%d code=%s retryable=%d retry_after_ms=%u",
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0,
            static_cast<unsigned>(error->retry_after_ms));
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t ProbeSyncAndClearTokenOnUnauthorized(const std::string& token)
{
    if (token.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "sync probe");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for sync");

    const std::string url = BuildUrl("/sync");
    const std::string request_body =
        "{\"firmware_version\":\"" WQN_FIRMWARE_VERSION "\",\"limit\":" + std::to_string(WQN_SYNC_LIMIT) + "}";
    int status_code = 0;
    std::string body;
    esp_err_t result = HttpRequest("POST", url, &token, &request_body, &status_code, &body);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "sync probe failed: %s", esp_err_to_name(result));
        return result;
    }

    if (status_code == 401) {
        return ClearTokenOnUnauthorized("sync probe");
    }
    if (status_code != 200) {
        ESP_LOGW(kTag, "sync probe HTTP status=%d", status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(kTag, "sync probe HTTP status=%d", status_code);
    return ESP_OK;
}

esp_err_t SyncDueProblemIds(const std::string& token, std::vector<std::string>* due_problem_ids, int* total)
{
    if (due_problem_ids == nullptr || total == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    due_problem_ids->clear();
    *total = 0;

    if (token.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "sync");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for sync");

    const std::string sync_url = BuildUrl("/sync");
    const std::string request_body =
        "{\"firmware_version\":\"" WQN_FIRMWARE_VERSION "\",\"limit\":" + std::to_string(WQN_SYNC_LIMIT) + "}";
    int status_code = 0;
    std::string body;
    esp_err_t result = HttpRequest("POST", sync_url, &token, &request_body, &status_code, &body);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "sync failed: %s", esp_err_to_name(result));
        return result;
    }

    if (status_code == 401) {
        return ClearTokenOnUnauthorized("sync");
    }
    if (status_code != 200) {
        ESP_LOGW(kTag, "sync HTTP status=%d", status_code);
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(ParseSyncResponse(body, due_problem_ids, total), kTag, "parse sync response");
    return ESP_OK;
}

esp_err_t FetchProblems(const std::string& token, const std::vector<std::string>& problem_ids, std::vector<WqnProblem>* problems)
{
    if (problems == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    problems->clear();

    if (token.empty() || problem_ids.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "problems");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for problems");

    const std::string problems_url = BuildUrl("/problems?ids=" + JoinProblemIdsForQuery(problem_ids));
    int status_code = 0;
    std::string body;
    esp_err_t result = HttpRequest("GET", problems_url, &token, nullptr, &status_code, &body);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "fetch problems failed: %s", esp_err_to_name(result));
        return result;
    }
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("problems");
    }
    if (status_code != 200) {
        ESP_LOGW(kTag, "problems HTTP status=%d", status_code);
        return ESP_FAIL;
    }

    return ParseProblemsResponse(body, problems);
}

esp_err_t FetchProblemIndex(const std::string& token, const WqnProblemIndexRequest& request, WqnProblemIndexPage* page)
{
    if (page == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    page->problems.clear();
    page->next_cursor.clear();
    page->has_more = false;
    page->total = 0;

    if (token.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "problem-index");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for problem-index");

    const std::string url = BuildUrl(BuildProblemIndexPath(request));
    int status_code = 0;
    std::string body;
    esp_err_t result = HttpRequest("GET", url, &token, nullptr, &status_code, &body);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "problem-index failed: %s", esp_err_to_name(result));
        return result;
    }
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("problem-index");
    }
    if (IsUnsupportedStatus(status_code)) {
        ESP_LOGI(kTag, "problem-index unsupported HTTP status=%d", status_code);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (status_code != 200) {
        ESP_LOGW(kTag, "problem-index HTTP status=%d", status_code);
        return ESP_FAIL;
    }

    return ParseProblemIndexResponse(body, page);
}

esp_err_t UploadReviewComplete(const std::string& token, const std::vector<WqnReviewResult>& results)
{
    if (token.empty() || results.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "review-complete");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for review-complete");

    std::string request_body;
    ESP_RETURN_ON_ERROR(BuildReviewCompleteBody(results, &request_body), kTag, "build review-complete request");

    const std::string url = BuildUrl("/review-complete");
    const std::string request_id = StableRequestIdForBody(request_body);
    if (request_id.empty()) {
        return ESP_FAIL;
    }
    int status_code = 0;
    std::string body;
    esp_err_t result = HttpRequest(
        "POST", url, &token, &request_body, &status_code, &body, nullptr, &request_id);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "review-complete failed: %s", esp_err_to_name(result));
        return result;
    }
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("review-complete");
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(kTag, "review-complete HTTP status=%d", status_code);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t FetchTodoTimeline(const std::string& token, const WqnTodoTimelineRequest& request, WqnTodoListPage* page)
{
    if (page == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    page->todos.clear();
    page->previous_cursor.clear();
    page->next_cursor.clear();
    page->has_earlier = false;
    page->has_later = false;
    page->has_more = false;
    page->total = 0;
    page->server_time.clear();
    page->selected_todo_id.clear();
    page->selected_index = -1;

    if (token.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "todo-list");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for todo-list");

    const std::string url = BuildUrl(BuildTodoListPath(request));
    int status_code = 0;
    std::string body;
    esp_err_t result = HttpRequest("GET", url, &token, nullptr, &status_code, &body);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "todo-list failed: %s", esp_err_to_name(result));
        return result;
    }
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("todo-list");
    }
    if (status_code != 200) {
        ESP_LOGW(kTag, "todo-list HTTP status=%d", status_code);
        return ESP_FAIL;
    }

    return ParseTodoListResponse(body, page);
}

esp_err_t FetchTodoTimeline(const std::string& token, WqnTodoListPage* page)
{
    WqnTodoTimelineRequest request;
    return FetchTodoTimeline(token, request, page);
}

esp_err_t FetchTodayPendingTodos(const std::string& token, WqnTodoListPage* page)
{
    return FetchTodoTimeline(token, page);
}

esp_err_t CompleteTodo(const std::string& token, const std::string& todo_id, WqnTodoItem* todo)
{
    if (todo_id.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (todo != nullptr) {
        *todo = WqnTodoItem{};
    }
    if (token.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "todo-complete");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for todo-complete");

    std::string request_body;
    ESP_RETURN_ON_ERROR(BuildTodoCompleteBody(todo_id, &request_body), kTag, "build todo-complete request");

    const std::string url = BuildUrl("/todos/complete");
    int status_code = 0;
    std::string body;
    esp_err_t result = HttpRequest("POST", url, &token, &request_body, &status_code, &body);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "todo-complete failed: %s", esp_err_to_name(result));
        return result;
    }
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("todo-complete");
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(kTag, "todo-complete HTTP status=%d", status_code);
        return ESP_FAIL;
    }

    WqnTodoItem parsed;
    result = ParseTodoCompleteResponse(body, &parsed);
    if (result != ESP_OK) {
        return result;
    }
    if (todo != nullptr) {
        *todo = std::move(parsed);
    }
    return ESP_OK;
}

esp_err_t SearchWords(const std::string& token, const WqnWordSearchRequest& request, WqnWordSearchResult* result)
{
    if (result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = WqnWordSearchResult{};
    if (request.query.empty() && request.prefix.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (token.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "word-search");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for word-search");

    const std::string url = BuildUrl(BuildWordSearchPath(request));
    int status_code = 0;
    std::string body;
    esp_err_t http_result = HttpRequest("GET", url, &token, nullptr, &status_code, &body);
    if (http_result != ESP_OK) {
        ESP_LOGW(kTag, "word-search failed: %s", esp_err_to_name(http_result));
        return http_result;
    }
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("word-search");
    }
    if (status_code != 200) {
        ESP_LOGW(kTag, "word-search HTTP status=%d", status_code);
        return ESP_FAIL;
    }

    return ParseWordSearchResponse(body, result);
}

esp_err_t FetchWordPackManifest(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    uint64_t cursor,
    WqnWordPackManifest* manifest)
{
    if (manifest == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest = WqnWordPackManifest{};
    if (token.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "word-pack-manifest");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for word-pack-manifest");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::word_study_v1::BuildManifestRequest(
            metadata, cursor, 100, &request_body),
        kTag,
        "build word-study manifest request");
    const std::string url = BuildUrl("/v3/words/manifest");
    int status_code = 0;
    std::string body;
    esp_err_t http_result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &body,
        protocol::v3::kProtocolHeader,
        &metadata.request_id);
    if (http_result != ESP_OK) {
        ESP_LOGW(kTag, "word-pack-manifest failed: %s", esp_err_to_name(http_result));
        return http_result;
    }
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("word-pack-manifest");
    }
    protocol::word_study_v1::ManifestData data;
    protocol::v3::Error error;
    const esp_err_t parse_result = protocol::word_study_v1::ParseManifestResponse(
        body, metadata.request_id, &data, &error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "word-study manifest failed: status=%d code=%s retryable=%d",
            status_code,
            error.code.c_str(),
            error.retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }

    manifest->cursor = data.cursor;
    manifest->has_more = data.has_more;
    manifest->packs.reserve(data.decks.size());
    for (const protocol::word_study_v1::ManifestDeck& deck : data.decks) {
        WqnWordPackManifestItem item;
        item.deck_id = deck.deck_id;
        item.title = deck.title;
        item.content_revision = deck.content_revision;
        item.change_sequence = deck.change_sequence;
        item.deleted = deck.deleted;
        if (!deck.deleted && deck.has_pack) {
            item.pack_id = deck.pack.pack_id;
            item.revision = deck.pack.pack_revision;
            item.pack_revision = deck.pack.pack_revision;
            item.schema_version = deck.pack.schema_version;
            item.format = "jsonl";
            item.compression = "none";
            item.sha256 = deck.pack.sha256;
            item.download_url = deck.pack.download_url;
            item.entry_count = deck.pack.entry_count;
            item.byte_size = deck.pack.byte_size;
        }
        manifest->packs.push_back(std::move(item));
    }
    ESP_LOGI(
        kTag,
        "word-study manifest ok: cursor=%llu changes=%u has_more=%d",
        static_cast<unsigned long long>(manifest->cursor),
        static_cast<unsigned>(manifest->packs.size()),
        manifest->has_more ? 1 : 0);
    return ESP_OK;
}

esp_err_t CreateWordStudySessionV1(
    const std::string& token,
    const protocol::word_study_v1::CreateSessionRequest& request,
    protocol::word_study_v1::SessionData* session,
    protocol::v3::Error* error)
{
    if (session == nullptr || error == nullptr || token.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    *session = {};
    *error = {};
    ESP_RETURN_ON_ERROR(
        ValidateTokenOrClear(token, "word-study-session"),
        kTag,
        "validate word-study token");
    ESP_RETURN_ON_ERROR(
        WaitForNetworkReadyForHttps(),
        kTag,
        "prepare network for word-study session");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::word_study_v1::BuildCreateSessionRequest(request, &request_body),
        kTag,
        "build word-study session request");
    const std::string url = BuildUrl("/v3/words/sessions");
    int status_code = 0;
    std::string body;
    const esp_err_t http_result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &body,
        protocol::v3::kProtocolHeader,
        &request.metadata.request_id,
        kWordSessionHttpTimeoutMs);
    if (http_result != ESP_OK) return http_result;
    if (status_code == 401) return ClearTokenOnUnauthorized("word-study-session");
    const esp_err_t parse_result = protocol::word_study_v1::ParseSessionResponse(
        body, request.metadata.request_id, session, error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "word-study session failed: status=%d code=%s retryable=%d",
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t FetchWordStudyCandidatePageV1(
    const std::string& token,
    const std::string& session_id,
    const protocol::word_study_v1::CandidatePageRequest& request,
    protocol::word_study_v1::CandidatePageData* page,
    protocol::v3::Error* error)
{
    if (page == nullptr || error == nullptr || token.empty() ||
        session_id.size() != 36) {
        return ESP_ERR_INVALID_ARG;
    }
    *page = {};
    *error = {};
    ESP_RETURN_ON_ERROR(
        ValidateTokenOrClear(token, "word-study-candidates"),
        kTag,
        "validate word candidate token");
    ESP_RETURN_ON_ERROR(
        WaitForNetworkReadyForHttps(),
        kTag,
        "prepare network for word candidates");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::word_study_v1::BuildCandidatePageRequest(
            request, &request_body),
        kTag,
        "build word candidate page request");
    const std::string url = BuildUrl(
        "/v3/words/sessions/" + session_id + "/candidates");
    int status_code = 0;
    std::string body;
    const esp_err_t http_result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &body,
        protocol::v3::kProtocolHeader,
        &request.metadata.request_id);
    if (http_result != ESP_OK) return http_result;
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("word-study-candidates");
    }
    const esp_err_t parse_result =
        protocol::word_study_v1::ParseCandidatePageResponse(
            body, request.metadata.request_id, page, error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "word candidate page failed: status=%d code=%s retryable=%d",
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t SubmitWordStudyObservationV1AtPath(
    const std::string& token,
    const protocol::word_study_v1::ObservationRequest& request,
    protocol::word_study_v1::ObservationData* observation,
    protocol::v3::Error* error,
    bool* transport_failure,
    const char* path,
    const char* operation)
{
    if (observation == nullptr || error == nullptr || transport_failure == nullptr ||
        token.empty() || path == nullptr || operation == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *observation = {};
    *error = {};
    *transport_failure = false;
    esp_err_t result = ValidateTokenOrClear(token, operation);
    if (result != ESP_OK) return result;
    result = WaitForNetworkReadyForHttps();
    if (result != ESP_OK) {
        *transport_failure = true;
        return result;
    }

    std::string request_body;
    result = protocol::word_study_v1::BuildObservationRequest(request, &request_body);
    if (result != ESP_OK) return result;
    const std::string url = BuildUrl(path);
    int status_code = 0;
    std::string body;
    result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &body,
        protocol::v3::kProtocolHeader,
        &request.metadata.request_id);
    if (result != ESP_OK) {
        *transport_failure = true;
        return result;
    }
    if (status_code == 401) return ClearTokenOnUnauthorized(operation);
    const esp_err_t parse_result = protocol::word_study_v1::ParseObservationResponse(
        body, request.metadata.request_id, observation, error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "%s failed: status=%d code=%s retryable=%d",
            operation,
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t SubmitWordStudyObservationV1(
    const std::string& token,
    const protocol::word_study_v1::ObservationRequest& request,
    protocol::word_study_v1::ObservationData* observation,
    protocol::v3::Error* error,
    bool* transport_failure)
{
    return SubmitWordStudyObservationV1AtPath(
        token,
        request,
        observation,
        error,
        transport_failure,
        "/v3/words/observations",
        "word-study-observation");
}

esp_err_t SkipWordStudyObservationV1(
    const std::string& token,
    const protocol::word_study_v1::ObservationRequest& request,
    protocol::word_study_v1::ObservationData* observation,
    protocol::v3::Error* error,
    bool* transport_failure)
{
    return SubmitWordStudyObservationV1AtPath(
        token,
        request,
        observation,
        error,
        transport_failure,
        "/v3/words/observations/skip",
        "word-study-observation-skip");
}

esp_err_t DownloadWordPackStream(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnWordPackManifestItem& item,
    WqnHttpChunkSink sink,
    void* context)
{
    if (sink == nullptr || metadata.request_id.empty() ||
        item.download_url.empty() || item.byte_size == 0 ||
        item.byte_size > protocol::word_study_v1::kMaxPackBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (token.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "word-pack-download");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for word-pack-download");

    const std::string url = BuildWordPackDownloadUrl(item.download_url);
    ESP_LOGI(kTag, "word-pack-download: pack_id=%s url=%s bytes_expected=%lu",
             item.pack_id.c_str(), url.c_str(), static_cast<unsigned long>(item.byte_size));
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const std::string authorization = "Bearer " + token;
    esp_err_t result = esp_http_client_set_header(
        client, "Accept", "application/x-ndjson");
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "Authorization", authorization.c_str());
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "X-WQN-Protocol", protocol::v3::kProtocolHeader);
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "X-WQN-Request-Id", metadata.request_id.c_str());
    }
    if (result == ESP_OK) {
        result = esp_http_client_open(client, 0);
    }
    int content_length = -1;
    int status_code = 0;
    if (result == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
    }
    if (result == ESP_OK && content_length >= 0 &&
        static_cast<uint64_t>(content_length) != item.byte_size) {
        ESP_LOGW(
            kTag,
            "word-pack-download Content-Length mismatch: expected=%lu actual=%d",
            static_cast<unsigned long>(item.byte_size),
            content_length);
        result = ESP_ERR_INVALID_SIZE;
    }
    size_t received = 0;
    std::array<uint8_t, 2048> buffer = {};
    while (result == ESP_OK && status_code == 200) {
        const int read = esp_http_client_read(
            client,
            reinterpret_cast<char*>(buffer.data()),
            buffer.size());
        FeedTaskWatchdogIfSubscribed();
        if (read < 0) {
            result = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        if (received + static_cast<size_t>(read) > item.byte_size ||
            received + static_cast<size_t>(read) >
                protocol::word_study_v1::kMaxPackBytes) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        result = sink(context, buffer.data(), static_cast<size_t>(read));
        if (result == ESP_OK) {
            received += static_cast<size_t>(read);
        }
    }
    if (result == ESP_OK && status_code == 200 &&
        (received != item.byte_size ||
         !esp_http_client_is_complete_data_received(client))) {
        result = ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code == 401) {
        return ClearTokenOnUnauthorized("word-pack-download");
    }
    if (status_code != 200) {
        ESP_LOGW(kTag, "word-pack-download HTTP status=%d", status_code);
        return ESP_FAIL;
    }
    if (result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "word-pack-download failed: %s url=%s received=%u",
            esp_err_to_name(result),
            url.c_str(),
            static_cast<unsigned>(received));
    }
    return result;
}

esp_err_t FetchNoteStudyManifest(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    uint64_t cursor,
    WqnNotePackManifest* manifest)
{
    if (manifest == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest = WqnNotePackManifest{};
    if (token.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "note-pack-manifest");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for note-pack-manifest");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::note_study_v1::BuildManifestRequest(
            metadata, cursor, 100, &request_body),
        kTag,
        "build note-study manifest request");
    const std::string url = BuildUrl("/v3/notes/manifest");
    int status_code = 0;
    std::string body;
    esp_err_t http_result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &body,
        protocol::v3::kProtocolHeader,
        &metadata.request_id);
    if (http_result != ESP_OK) {
        ESP_LOGW(kTag, "note-pack-manifest failed: %s", esp_err_to_name(http_result));
        return http_result;
    }
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("note-pack-manifest");
    }
    protocol::note_study_v1::ManifestData data;
    protocol::v3::Error error;
    const esp_err_t parse_result = protocol::note_study_v1::ParseManifestResponse(
        body, metadata.request_id, &data, &error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "note-study manifest failed: status=%d code=%s retryable=%d",
            status_code,
            error.code.c_str(),
            error.retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }

    manifest->cursor = data.cursor;
    manifest->has_more = data.has_more;
    manifest->notebooks.reserve(data.notebooks.size());
    for (const protocol::note_study_v1::ManifestNotebook& source : data.notebooks) {
        WqnNotePackManifestNotebook notebook;
        notebook.notebook_id = source.notebook_id;
        notebook.title = source.title;
        notebook.change_sequence = source.change_sequence;
        notebook.content_revision = source.content_revision;
        notebook.deleted = source.deleted;
        if (!source.deleted && source.has_pack) {
            notebook.has_pack = true;
            notebook.pack_id = source.pack.pack_id;
            notebook.pack_revision = source.pack.pack_revision;
            notebook.schema_version = source.pack.schema_version;
            notebook.entry_count = source.pack.entry_count;
            notebook.byte_size = source.pack.byte_size;
            notebook.sha256 = source.pack.sha256;
            notebook.download_url = source.pack.download_url;
        }
        manifest->notebooks.push_back(std::move(notebook));
    }
    ESP_LOGI(
        kTag,
        "note-study manifest ok: cursor=%llu changes=%u has_more=%d",
        static_cast<unsigned long long>(manifest->cursor),
        static_cast<unsigned>(manifest->notebooks.size()),
        manifest->has_more ? 1 : 0);
    return ESP_OK;
}

esp_err_t DownloadNotePackStream(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnNotePackManifestNotebook& notebook,
    WqnHttpChunkSink sink,
    void* context)
{
    if (sink == nullptr || metadata.request_id.empty() ||
        notebook.download_url.empty() || notebook.byte_size == 0 ||
        notebook.byte_size > protocol::note_study_v1::kMaxPackBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (token.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "note-pack-download");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for note-pack-download");

    const std::string url = BuildWordPackDownloadUrl(notebook.download_url);
    ESP_LOGI(kTag, "note-pack-download: pack_id=%s url=%s bytes_expected=%lu",
             notebook.pack_id.c_str(), url.c_str(),
             static_cast<unsigned long>(notebook.byte_size));
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const std::string authorization = "Bearer " + token;
    esp_err_t result = esp_http_client_set_header(
        client, "Accept", "application/x-ndjson");
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "Authorization", authorization.c_str());
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "X-WQN-Protocol", protocol::v3::kProtocolHeader);
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "X-WQN-Request-Id", metadata.request_id.c_str());
    }
    if (result == ESP_OK) {
        result = esp_http_client_open(client, 0);
    }
    int content_length = -1;
    int status_code = 0;
    if (result == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
    }
    if (result == ESP_OK && content_length >= 0 &&
        static_cast<uint64_t>(content_length) != notebook.byte_size) {
        ESP_LOGW(
            kTag,
            "note-pack-download Content-Length mismatch: expected=%lu actual=%d",
            static_cast<unsigned long>(notebook.byte_size),
            content_length);
        result = ESP_ERR_INVALID_SIZE;
    }
    size_t received = 0;
    std::array<uint8_t, 2048> buffer = {};
    while (result == ESP_OK && status_code == 200) {
        const int read = esp_http_client_read(
            client,
            reinterpret_cast<char*>(buffer.data()),
            buffer.size());
        FeedTaskWatchdogIfSubscribed();
        if (read < 0) {
            result = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        if (received + static_cast<size_t>(read) > notebook.byte_size ||
            received + static_cast<size_t>(read) >
                protocol::note_study_v1::kMaxPackBytes) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        result = sink(context, buffer.data(), static_cast<size_t>(read));
        if (result == ESP_OK) {
            received += static_cast<size_t>(read);
        }
    }
    if (result == ESP_OK && status_code == 200 &&
        (received != notebook.byte_size ||
         !esp_http_client_is_complete_data_received(client))) {
        result = ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code == 401) {
        return ClearTokenOnUnauthorized("note-pack-download");
    }
    if (status_code != 200) {
        ESP_LOGW(kTag, "note-pack-download HTTP status=%d", status_code);
        return ESP_FAIL;
    }
    if (result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "note-pack-download failed: %s url=%s received=%u",
            esp_err_to_name(result),
            url.c_str(),
            static_cast<unsigned>(received));
    }
    return result;
}

esp_err_t DownloadNoteImageV1(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const std::string& note_id,
    uint8_t image_index,
    const std::string& expected_image_id,
    std::vector<uint8_t>* wqni)
{
    // WQNI file size is fixed by the contract: 20-byte header + 400x300/8.
    constexpr size_t kWqniFileBytes = 20 + 15000;
    if (wqni == nullptr || metadata.request_id.empty() || note_id.size() != 36 ||
        image_index > 3 || expected_image_id.size() != 64) {
        return ESP_ERR_INVALID_ARG;
    }
    if (token.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "note-image-download");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for note-image-download");

    const std::string url = BuildUrl(
        "/v3/notes/images/" + note_id + "/" + std::to_string(image_index));
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const std::string authorization = "Bearer " + token;
    esp_err_t result = esp_http_client_set_header(
        client, "Accept", "application/octet-stream");
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "Authorization", authorization.c_str());
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "X-WQN-Protocol", protocol::v3::kProtocolHeader);
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "X-WQN-Request-Id", metadata.request_id.c_str());
    }
    if (result == ESP_OK) {
        result = esp_http_client_open(client, 0);
    }
    int content_length = -1;
    int status_code = 0;
    if (result == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
    }
    if (result == ESP_OK && status_code == 200 && content_length >= 0 &&
        static_cast<size_t>(content_length) != kWqniFileBytes) {
        ESP_LOGW(kTag, "note-image-download Content-Length mismatch: expected=%u actual=%d",
                 static_cast<unsigned>(kWqniFileBytes), content_length);
        result = ESP_ERR_INVALID_SIZE;
    }
    wqni->clear();
    wqni->reserve(kWqniFileBytes);
    std::array<uint8_t, 2048> buffer = {};
    while (result == ESP_OK && status_code == 200) {
        const int read = esp_http_client_read(
            client, reinterpret_cast<char*>(buffer.data()), buffer.size());
        FeedTaskWatchdogIfSubscribed();
        if (read < 0) {
            result = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        if (wqni->size() + static_cast<size_t>(read) > kWqniFileBytes) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        wqni->insert(wqni->end(), buffer.data(), buffer.data() + read);
    }
    if (result == ESP_OK && status_code == 200 &&
        (wqni->size() != kWqniFileBytes ||
         !esp_http_client_is_complete_data_received(client))) {
        result = ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code == 401) {
        wqni->clear();
        return ClearTokenOnUnauthorized("note-image-download");
    }
    if (status_code != 200) {
        ESP_LOGW(kTag, "note-image-download HTTP status=%d note=%s index=%u",
                 status_code, note_id.c_str(), static_cast<unsigned>(image_index));
        wqni->clear();
        return status_code == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (result != ESP_OK) {
        wqni->clear();
        return result;
    }

    // Content addressing: the pack line pinned the image id, so the bytes must
    // hash to it or the panel would show something the user never attached.
    std::array<unsigned char, 32> digest = {};
    if (mbedtls_sha256(wqni->data(), wqni->size(), digest.data(), 0) != 0) {
        wqni->clear();
        return ESP_FAIL;
    }
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string actual_id;
    actual_id.reserve(64);
    for (unsigned char byte : digest) {
        actual_id.push_back(kHexDigits[byte >> 4]);
        actual_id.push_back(kHexDigits[byte & 0x0f]);
    }
    if (actual_id != expected_image_id) {
        ESP_LOGW(kTag, "note-image-download hash mismatch: expected=%.12s actual=%.12s",
                 expected_image_id.c_str(), actual_id.c_str());
        wqni->clear();
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t CreateNoteStudySessionV1(
    const std::string& token,
    const protocol::note_study_v1::CreateSessionRequest& request,
    protocol::note_study_v1::SessionData* session,
    protocol::v3::Error* error)
{
    if (session == nullptr || error == nullptr || token.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    *session = {};
    *error = {};
    ESP_RETURN_ON_ERROR(
        ValidateTokenOrClear(token, "note-study-session"),
        kTag,
        "validate note-study token");
    ESP_RETURN_ON_ERROR(
        WaitForNetworkReadyForHttps(),
        kTag,
        "prepare network for note-study session");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::note_study_v1::BuildCreateSessionRequest(request, &request_body),
        kTag,
        "build note-study session request");
    const std::string url = BuildUrl("/v3/notes/sessions");
    int status_code = 0;
    std::string body;
    const esp_err_t http_result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &body,
        protocol::v3::kProtocolHeader,
        &request.metadata.request_id,
        kWordSessionHttpTimeoutMs);
    if (http_result != ESP_OK) return http_result;
    if (status_code == 401) return ClearTokenOnUnauthorized("note-study-session");
    const esp_err_t parse_result = protocol::note_study_v1::ParseSessionResponse(
        body, request.metadata.request_id, session, error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "note-study session failed: status=%d code=%s retryable=%d",
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t FetchNoteStudyCandidatePageV1(
    const std::string& token,
    const std::string& session_id,
    const protocol::note_study_v1::CandidatePageRequest& request,
    protocol::note_study_v1::CandidatePageData* page,
    protocol::v3::Error* error)
{
    if (page == nullptr || error == nullptr || token.empty() ||
        session_id.size() != 36) {
        return ESP_ERR_INVALID_ARG;
    }
    *page = {};
    *error = {};
    ESP_RETURN_ON_ERROR(
        ValidateTokenOrClear(token, "note-study-candidates"),
        kTag,
        "validate note candidate token");
    ESP_RETURN_ON_ERROR(
        WaitForNetworkReadyForHttps(),
        kTag,
        "prepare network for note candidates");

    std::string request_body;
    ESP_RETURN_ON_ERROR(
        protocol::note_study_v1::BuildCandidatePageRequest(
            request, &request_body),
        kTag,
        "build note candidate page request");
    const std::string url = BuildUrl(
        "/v3/notes/sessions/" + session_id + "/candidates");
    int status_code = 0;
    std::string body;
    const esp_err_t http_result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &body,
        protocol::v3::kProtocolHeader,
        &request.metadata.request_id);
    if (http_result != ESP_OK) return http_result;
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("note-study-candidates");
    }
    const esp_err_t parse_result =
        protocol::note_study_v1::ParseCandidatePageResponse(
            body, request.metadata.request_id, page, error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "note candidate page failed: status=%d code=%s retryable=%d",
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t SubmitNoteStudyObservationV1AtPath(
    const std::string& token,
    const protocol::note_study_v1::ObservationRequest& request,
    protocol::note_study_v1::ObservationData* observation,
    protocol::v3::Error* error,
    bool* transport_failure,
    const char* path,
    const char* operation)
{
    if (observation == nullptr || error == nullptr || transport_failure == nullptr ||
        token.empty() || path == nullptr || operation == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *observation = {};
    *error = {};
    *transport_failure = false;
    esp_err_t result = ValidateTokenOrClear(token, operation);
    if (result != ESP_OK) return result;
    result = WaitForNetworkReadyForHttps();
    if (result != ESP_OK) {
        *transport_failure = true;
        return result;
    }

    std::string request_body;
    result = protocol::note_study_v1::BuildObservationRequest(request, &request_body);
    if (result != ESP_OK) return result;
    const std::string url = BuildUrl(path);
    int status_code = 0;
    std::string body;
    result = HttpRequest(
        "POST",
        url,
        &token,
        &request_body,
        &status_code,
        &body,
        protocol::v3::kProtocolHeader,
        &request.metadata.request_id);
    if (result != ESP_OK) {
        *transport_failure = true;
        return result;
    }
    if (status_code == 401) return ClearTokenOnUnauthorized(operation);
    const esp_err_t parse_result = protocol::note_study_v1::ParseObservationResponse(
        body, request.metadata.request_id, observation, error);
    if (status_code != 200 || parse_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "%s failed: status=%d code=%s retryable=%d",
            operation,
            status_code,
            error->code.c_str(),
            error->retryable ? 1 : 0);
        return parse_result == ESP_OK ? ESP_FAIL : parse_result;
    }
    return ESP_OK;
}

esp_err_t SubmitNoteStudyObservationV1(
    const std::string& token,
    const protocol::note_study_v1::ObservationRequest& request,
    protocol::note_study_v1::ObservationData* observation,
    protocol::v3::Error* error,
    bool* transport_failure)
{
    return SubmitNoteStudyObservationV1AtPath(
        token,
        request,
        observation,
        error,
        transport_failure,
        "/v3/notes/observations",
        "note-study-observation");
}

esp_err_t SkipNoteStudyObservationV1(
    const std::string& token,
    const protocol::note_study_v1::ObservationRequest& request,
    protocol::note_study_v1::ObservationData* observation,
    protocol::v3::Error* error,
    bool* transport_failure)
{
    return SubmitNoteStudyObservationV1AtPath(
        token,
        request,
        observation,
        error,
        transport_failure,
        "/v3/notes/observations/skip",
        "note-study-observation-skip");
}

esp_err_t LookupWordWithAi(const std::string& token, const WqnWordAiLookupRequest& request, WqnWordAiLookupResult* result)
{
    if (result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = WqnWordAiLookupResult{};
    if (request.query.empty() && request.prefix.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (token.empty()) {
        return ESP_OK;
    }
    const esp_err_t token_result = ValidateTokenOrClear(token, "word-ai-lookup");
    if (token_result != ESP_OK) {
        return token_result;
    }

    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for word-ai-lookup");

    std::string request_body;
    ESP_RETURN_ON_ERROR(BuildWordAiLookupBody(request, &request_body), kTag, "build word-ai-lookup request");

    const std::string url = BuildUrl("/words/ai-lookup");
    int status_code = 0;
    std::string body;
    esp_err_t http_result = HttpRequest("POST", url, &token, &request_body, &status_code, &body);
    if (http_result != ESP_OK) {
        ESP_LOGW(kTag, "word-ai-lookup failed: %s", esp_err_to_name(http_result));
        return http_result;
    }
    if (status_code == 401) {
        return ClearTokenOnUnauthorized("word-ai-lookup");
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(kTag, "word-ai-lookup HTTP status=%d", status_code);
        return ESP_FAIL;
    }

    return ParseWordAiLookupResponse(body, result);
}

esp_err_t ParseTodoListResponse(const std::string& body, WqnTodoListPage* page)
{
    return ParseTodoListResponseImpl(body, page);
}

esp_err_t ParseTodoCompleteResponse(const std::string& body, WqnTodoItem* todo)
{
    return ParseTodoCompleteResponseImpl(body, todo);
}

esp_err_t ParseWordSearchResponse(const std::string& body, WqnWordSearchResult* result)
{
    return ParseWordSearchResponseImpl(body, result);
}

esp_err_t ParseWordPackManifestResponse(const std::string& body, WqnWordPackManifest* manifest)
{
    return ParseWordPackManifestResponseImpl(body, manifest);
}

esp_err_t ParseWordAiLookupResponse(const std::string& body, WqnWordAiLookupResult* result)
{
    return ParseWordAiLookupResponseImpl(body, result);
}

esp_err_t ParseAiChatResponseBody(const std::string& body, WqnAiChatResponse* response)
{
    if (response == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *response = WqnAiChatResponse{};

    JsonDocument document(body);
    if (!document.ok()) {
        ESP_LOGW(kTag, "AI response JSON parse failed");
        response->error_message = "AI response JSON parse failed";
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    if (!GetSuccess(document.root()) || !cJSON_IsObject(data)) {
        ESP_LOGW(kTag, "AI response missing success/data");
        response->error_message = BuildApiErrorMessage(document.root(), 200);
        return ESP_FAIL;
    }

    response->transcript = GetOptionalString(data, "transcript");
    response->reply_text = GetOptionalString(data, "reply_text");
    response->conversation_id = GetOptionalString(data, "conversation_id");
    response->latency_ms = GetOptionalInt(data, "latency_ms");
    ParseAiActions(cJSON_GetObjectItemCaseSensitive(data, "actions"), &response->actions);
    ParseAiStatusTrace(cJSON_GetObjectItemCaseSensitive(data, "status_trace"), &response->status_trace);
    ParseAiAsrSummary(cJSON_GetObjectItemCaseSensitive(data, "asr"), &response->asr);
    ParseAiFunctionCalls(cJSON_GetObjectItemCaseSensitive(data, "function_calls"), &response->function_calls);
    if (response->transcript.empty() && response->reply_text.empty()) {
        ESP_LOGW(kTag, "AI response missing transcript and reply_text");
        response->error_message = "AI response missing transcript and reply_text";
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t SyncDueProblemsAndLog(const std::string& token)
{
    std::vector<std::string> due_problem_ids;
    int total = 0;
    ESP_RETURN_ON_ERROR(SyncDueProblemIds(token, &due_problem_ids, &total), kTag, "sync due problem ids");
    ESP_LOGI(kTag, "sync due problems: returned=%u total=%d", static_cast<unsigned>(due_problem_ids.size()), total);
    if (due_problem_ids.empty()) {
        due_problem_ids = SplitDebugProblemIds();
        if (due_problem_ids.empty()) {
            return ESP_OK;
        }
        ESP_LOGW(kTag, "using debug problem ids because sync returned none: count=%u", static_cast<unsigned>(due_problem_ids.size()));
    }

    std::vector<WqnProblem> problems;
    ESP_RETURN_ON_ERROR(FetchProblems(token, due_problem_ids, &problems), kTag, "fetch due problems");
    ESP_LOGI(kTag, "fetched problem details: count=%u", static_cast<unsigned>(problems.size()));
    for (size_t i = 0; i < problems.size(); ++i) {
        LogProblemSummary(problems[i], static_cast<int>(i + 1), static_cast<int>(problems.size()));
    }

    return ESP_OK;
}

esp_err_t UploadAiAudioChat(
    const std::string& token,
    const uint8_t* pcm_data,
    size_t pcm_size,
    int duration_ms,
    const std::string& conversation_id,
    const std::string& tier,
    WqnAiChatResponse* response)
{
    if (pcm_data == nullptr || pcm_size == 0 || response == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ValidateTokenOrClear(token, "AI audio"), kTag, "validate AI token");
    ESP_RETURN_ON_ERROR(WaitForNetworkReadyForHttps(), kTag, "prepare network for AI audio");

    const std::string url = BuildUrl("/ai/transcribe-chat");
    int status_code = 0;
    std::string body;
    const esp_err_t result =
        HttpBinaryPost(url, token, pcm_data, pcm_size, duration_ms, conversation_id, tier, &status_code, &body);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "AI audio upload failed: %s", esp_err_to_name(result));
        return result;
    }
    ESP_LOGI(kTag, "AI audio response: status=%d bytes=%u", status_code, static_cast<unsigned>(body.size()));

    if (status_code == 401) {
        response->error_code = "unauthorized";
        response->error_message = "设备授权已失效";
        return ClearTokenOnUnauthorized("AI audio");
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(kTag, "AI audio request rejected: status=%d body=%s", status_code, TruncateForLog(body, 160).c_str());
        JsonDocument error_document(body);
        response->error_message = error_document.ok() ? BuildApiErrorMessage(error_document.root(), status_code) : "";
        if (error_document.ok()) {
            cJSON* error = cJSON_GetObjectItemCaseSensitive(error_document.root(), "error");
            if (cJSON_IsObject(error)) {
                response->error_code = GetOptionalString(error, "code");
            }
        }
        if (response->error_message.empty()) {
            char fallback[48] = {};
            std::snprintf(fallback, sizeof(fallback), "HTTP %d", status_code);
            response->error_message = fallback;
        }
        return ESP_FAIL;
    }

    return ParseAiChatResponseBody(body, response);
}

}  // namespace wqn
