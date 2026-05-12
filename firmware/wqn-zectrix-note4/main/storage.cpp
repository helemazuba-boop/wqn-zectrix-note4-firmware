#include "storage.h"

#include <vector>

#include "config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "wqn_storage";
constexpr size_t kAccessTokenLength = 64;

bool IsValidAccessTokenShape(const std::string& token)
{
    if (token.size() != kAccessTokenLength) {
        return false;
    }
    for (const char c : token) {
        const bool hex =
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

}  // namespace

namespace wqn {

esp_err_t InitStorage()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "NVS requires erase before init: %s", esp_err_to_name(result));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "erase NVS");
        result = nvs_flash_init();
    }

    ESP_RETURN_ON_ERROR(result, kTag, "init NVS");
    ESP_LOGI(kTag, "NVS ready");
    return ESP_OK;
}

esp_err_t LoadAccessToken(std::string* token)
{
    if (token == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(WQN_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        token->clear();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open NVS namespace");

    size_t required_size = 0;
    result = nvs_get_str(handle, WQN_NVS_ACCESS_TOKEN_KEY, nullptr, &required_size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        token->clear();
        return ESP_OK;
    }
    if (result != ESP_OK) {
        nvs_close(handle);
        return result;
    }

    std::vector<char> buffer(required_size);
    result = nvs_get_str(handle, WQN_NVS_ACCESS_TOKEN_KEY, buffer.data(), &required_size);
    nvs_close(handle);
    if (result != ESP_OK) {
        return result;
    }

    *token = std::string(buffer.data());
    return ESP_OK;
}

esp_err_t SaveAccessToken(const std::string& token)
{
    if (!IsValidAccessTokenShape(token)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &handle), kTag, "open NVS namespace");

    esp_err_t result = nvs_set_str(handle, WQN_NVS_ACCESS_TOKEN_KEY, token.c_str());
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t ClearAccessToken()
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(WQN_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open NVS namespace");

    result = nvs_erase_key(handle, WQN_NVS_ACCESS_TOKEN_KEY);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

std::string MaskTokenForLog(const std::string& token)
{
    if (token.size() < 12) {
        return "<invalid-token>";
    }
    return token.substr(0, 4) + "..." + token.substr(token.size() - 4);
}

}  // namespace wqn
