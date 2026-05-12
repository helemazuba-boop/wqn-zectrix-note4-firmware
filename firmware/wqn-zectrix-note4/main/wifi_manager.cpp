#include "wifi_manager.h"

#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

namespace {

constexpr char kTag[] = "wqn_wifi";
constexpr TickType_t kReconnectDelay = pdMS_TO_TICKS(5000);

#if CONFIG_WQN_WIFI_STA_ENABLE
bool g_initialized = false;
bool g_reconnect_pending = false;
bool g_wifi_connected = false;

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
        ESP_LOGW(kTag, "WiFi disconnected: reason=%d", event ? event->reason : -1);
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

esp_err_t StartWifiStationIfEnabled()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (g_initialized) {
        return ESP_OK;
    }

    if (std::strlen(CONFIG_WQN_WIFI_SSID) == 0) {
        ESP_LOGW(kTag, "WiFi station enabled but CONFIG_WQN_WIFI_SSID is empty; not starting WiFi");
        return ESP_OK;
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
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), CONFIG_WQN_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(
        reinterpret_cast<char*>(wifi_config.sta.password),
        CONFIG_WQN_WIFI_PASSWORD,
        sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), kTag, "set WiFi STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start WiFi");

    g_initialized = true;
    ESP_LOGI(kTag, "WiFi station flow enabled");
    return ESP_OK;
#else
    ESP_LOGI(kTag, "WiFi station disabled by CONFIG_WQN_WIFI_STA_ENABLE");
    return ESP_OK;
#endif
}

}  // namespace wqn
