#include "wifi_manager.h"

#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include <ctime>

#include "storage.h"

namespace {

constexpr char kTag[] = "wqn_wifi";
constexpr TickType_t kReconnectDelay = pdMS_TO_TICKS(5000);

#if CONFIG_WQN_WIFI_STA_ENABLE
constexpr EventBits_t kWifiConnectedBit = BIT0;

bool g_initialized = false;
bool g_reconnect_pending = false;
bool g_wifi_connected = false;
bool g_sntp_started = false;
EventGroupHandle_t g_wifi_event_group = nullptr;

const char* DisconnectReasonName(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
            return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_FAIL:
            return "AUTH_FAIL";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND:
            return "NO_AP_FOUND";
        case WIFI_REASON_CONNECTION_FAIL:
            return "CONNECTION_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            return "NO_AP_FOUND_COMPATIBLE_SECURITY";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "NO_AP_FOUND_AUTHMODE_THRESHOLD";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "NO_AP_FOUND_RSSI_THRESHOLD";
        default:
            return "OTHER";
    }
}

void WifiReconnectTask(void*)
{
    while (!g_wifi_connected) {
        vTaskDelay(kReconnectDelay);
        if (g_wifi_connected) {
            break;
        }

        ESP_LOGI(kTag, "retrying WiFi station connection");
        const esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(kTag, "WiFi reconnect request failed: %s", esp_err_to_name(result));
        }
    }

    g_reconnect_pending = false;
    vTaskDelete(nullptr);
}

void QueueReconnect()
{
    if (g_reconnect_pending) {
        return;
    }

    g_reconnect_pending = true;
    const BaseType_t created = xTaskCreate(WifiReconnectTask, "wqn_wifi_reconnect", 2048, nullptr, 4, nullptr);
    if (created != pdPASS) {
        g_reconnect_pending = false;
        ESP_LOGW(kTag, "failed to create WiFi reconnect task");
    }
}

void TimeSyncCallback(struct timeval*)
{
    std::time_t now = 0;
    std::time(&now);
    char buffer[32] = {};
    std::tm time_info = {};
    localtime_r(&now, &time_info);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &time_info);
    ESP_LOGI(kTag, "SNTP time synced: %s", buffer);
}

void StartSntpOnce()
{
    if (g_sntp_started) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.sync_cb = TimeSyncCallback;
    const esp_err_t result = esp_netif_sntp_init(&config);
    if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
        g_sntp_started = true;
        ESP_LOGI(kTag, "SNTP started");
    } else {
        ESP_LOGW(kTag, "SNTP start failed: %s", esp_err_to_name(result));
    }
}

void WifiEventHandler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(kTag, "WiFi station started, connecting to configured SSID");
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        g_wifi_connected = false;
        if (g_wifi_event_group != nullptr) {
            xEventGroupClearBits(g_wifi_event_group, kWifiConnectedBit);
        }
        ESP_LOGW(
            kTag,
            "WiFi disconnected: reason=%d (%s) rssi=%d",
            event ? event->reason : -1,
            event ? DisconnectReasonName(event->reason) : "NO_EVENT",
            event ? event->rssi : 0);
        QueueReconnect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        if (event == nullptr) {
            ESP_LOGW(kTag, "got IP event without payload");
            return;
        }

        ESP_LOGI(
            kTag,
            "WiFi got IP: ip=" IPSTR " netmask=" IPSTR " gateway=" IPSTR,
            IP2STR(&event->ip_info.ip),
            IP2STR(&event->ip_info.netmask),
            IP2STR(&event->ip_info.gw));
        g_wifi_connected = true;
        if (g_wifi_event_group != nullptr) {
            xEventGroupSetBits(g_wifi_event_group, kWifiConnectedBit);
        }
        StartSntpOnce();
    }
}

esp_err_t RegisterHandlers()
{
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, nullptr, nullptr),
        kTag,
        "register WiFi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, nullptr, nullptr),
        kTag,
        "register IP event handler");
    return ESP_OK;
}
#endif

}  // namespace

namespace wqn {

esp_err_t StartWifiWithCredentials(const char* ssid, const char* password)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (ssid == nullptr || ssid[0] == '\0') {
        ESP_LOGW(kTag, "cannot start WiFi: empty SSID");
        return ESP_ERR_INVALID_ARG;
    }

    if (g_initialized) {
        ESP_LOGI(kTag, "WiFi already initialized, switching to new credentials");
        ESP_RETURN_ON_ERROR(esp_wifi_disconnect(), kTag, "disconnect before reconfigure");
    }

    g_wifi_event_group = xEventGroupCreate();
    if (g_wifi_event_group == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "init esp-netif");
    esp_err_t event_loop_result = esp_event_loop_create_default();
    if (event_loop_result != ESP_OK && event_loop_result != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(event_loop_result, kTag, "create default event loop");
    }

    if (esp_netif_create_default_wifi_sta() == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), kTag, "init WiFi");
    ESP_RETURN_ON_ERROR(RegisterHandlers(), kTag, "register WiFi handlers");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set WiFi STA mode");

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != nullptr && password[0] != '\0') {
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode =
        (password == nullptr || password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), kTag, "set WiFi STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), kTag, "set WiFi power save");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start WiFi");

    g_initialized = true;
    ESP_LOGI(kTag, "WiFi station started with SSID: %s", ssid);
    return ESP_OK;
#else
    (void)ssid;
    (void)password;
    ESP_LOGI(kTag, "WiFi station disabled by CONFIG_WQN_WIFI_STA_ENABLE");
    return ESP_OK;
#endif
}

esp_err_t StartWifiStationIfEnabled()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (g_initialized) {
        return ESP_OK;
    }

    std::string ssid;
    std::string password;

    // Layer 1: Try runtime NVS credentials first (from provisioning or settings page)
    if (wqn::LoadWifiCredentials(&ssid, &password) == ESP_OK && !ssid.empty()) {
        ESP_LOGI(kTag, "using runtime WiFi credentials from NVS (SSID=%s)", ssid.c_str());
        return StartWifiWithCredentials(ssid.c_str(), password.c_str());
    }

    // Layer 2: Fall back to compile-time sdkconfig credentials (developer-only, local-only)
    if (std::strlen(CONFIG_WQN_WIFI_SSID) > 0) {
        ESP_LOGI(kTag, "using compile-time WiFi credentials from sdkconfig (SSID=%s)", CONFIG_WQN_WIFI_SSID);
        return StartWifiWithCredentials(CONFIG_WQN_WIFI_SSID, CONFIG_WQN_WIFI_PASSWORD);
    }

    // Layer 3: No credentials available; caller should start provisioning mode
    ESP_LOGI(kTag, "no WiFi credentials available; provisioning mode required");
    return ESP_ERR_NOT_FOUND;
#else
    ESP_LOGI(kTag, "WiFi station disabled by CONFIG_WQN_WIFI_STA_ENABLE");
    return ESP_OK;
#endif
}

esp_err_t WaitForWifiStationConnected(TickType_t timeout)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (g_wifi_event_group == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group, kWifiConnectedBit, pdFALSE, pdFALSE, timeout);
    return (bits & kWifiConnectedBit) ? ESP_OK : ESP_ERR_TIMEOUT;
#else
    (void)timeout;
    return ESP_ERR_INVALID_STATE;
#endif
}

bool IsWifiStationConnected()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    return g_wifi_connected;
#else
    return false;
#endif
}

}  // namespace wqn
