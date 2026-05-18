#include "wqn_api.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "config.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage.h"
#include "text_render.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "wqn_api";
constexpr int kHttpTimeoutMs = 10000;
constexpr TickType_t kWifiConnectTimeout = pdMS_TO_TICKS(30000);
constexpr TickType_t kSntpSyncTimeout = pdMS_TO_TICKS(15000);
constexpr TickType_t kPollDelay = pdMS_TO_TICKS(2000);
constexpr int kPollAttempts = 150;
constexpr size_t kProblemPreviewChars = 240;
constexpr std::time_t kMinReasonableUnixTime = 1704067200;  // 2024-01-01 UTC

class JsonDocument {
public:
    explicit JsonDocument(const std::string& payload) : root_(cJSON_Parse(payload.c_str())) {}
    ~JsonDocument() { cJSON_Delete(root_); }

    cJSON* root() const { return root_; }
    bool ok() const { return root_ != nullptr; }

private:
    cJSON* root_ = nullptr;
};

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

    result = esp_netif_sntp_sync_wait(kSntpSyncTimeout);
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
    ESP_RETURN_ON_ERROR(wqn::WaitForWifiStationConnected(kWifiConnectTimeout), kTag, "wait for WiFi");
    ESP_RETURN_ON_ERROR(EnsureClockSyncedForHttps(), kTag, "sync clock for HTTPS");
    return ESP_OK;
}

esp_err_t HttpRequest(
    const char* method,
    const std::string& url,
    const std::string* bearer_token,
    const std::string* request_body,
    int* status_code,
    std::string* response_body)
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
    config.timeout_ms = kHttpTimeoutMs;
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
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }

    result = esp_http_client_open(client, is_post ? post_body.size() : 0);
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }
    if (is_post) {
        const int written = esp_http_client_write(client, post_body.c_str(), post_body.size());
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
    parsed.solution_text = wqn::HtmlToPlainText(GetOptionalString(item, "solution_text"));
    ParseAssetManifest(cJSON_GetObjectItemCaseSensitive(item, "assets"), &parsed.assets);
    if (parsed.assets.empty()) {
        ParseAssetManifest(cJSON_GetObjectItemCaseSensitive(item, "attachments"), &parsed.assets);
    }
    ParseAssetManifest(cJSON_GetObjectItemCaseSensitive(item, "solution_assets"), &parsed.solution_assets);

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
    } else if (status == "no_pending" || status == "expired") {
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
        bool paired = false;
        const esp_err_t result = PollPairingOnce(mac_address, &paired);
        if (result == ESP_OK && paired) {
            return ESP_OK;
        }
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "poll attempt %d failed: %s", attempt, esp_err_to_name(result));
        }
        vTaskDelay(kPollDelay);
    }

    ESP_LOGW(kTag, "pairing poll timed out");
    return ESP_ERR_TIMEOUT;
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
        ESP_LOGW(kTag, "problem-index unsupported HTTP status=%d", status_code);
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
    int status_code = 0;
    std::string body;
    esp_err_t result = HttpRequest("POST", url, &token, &request_body, &status_code, &body);
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

}  // namespace wqn
