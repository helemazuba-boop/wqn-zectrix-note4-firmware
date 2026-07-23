#include "wifi_manager.h"

#include <atomic>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include <ctime>

#include "storage.h"

namespace {

constexpr char kTag[] = "wqn_wifi";
#if CONFIG_WQN_WIFI_STA_ENABLE
constexpr EventBits_t kWifiConnectedBit = BIT0;

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_wifi_connected{false};
bool g_sntp_started = false;
std::atomic<bool> g_sleep_quiescing{false};
std::atomic<bool> g_resume_after_sleep_abort{false};
std::atomic<wqn::WifiStationEventSink> g_event_sink{nullptr};
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

void PublishStationEvent(wqn::WifiStationEvent event, int reason = 0, int rssi = 0)
{
    const wqn::WifiStationEventSink sink =
        g_event_sink.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(event, reason, rssi);
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
        ESP_LOGI(kTag, "WiFi station started");
        PublishStationEvent(wqn::WifiStationEvent::kStarted);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        g_wifi_connected.store(false, std::memory_order_release);
        if (g_wifi_event_group != nullptr) {
            xEventGroupClearBits(g_wifi_event_group, kWifiConnectedBit);
        }
        ESP_LOGW(
            kTag,
            "WiFi disconnected: reason=%d (%s) rssi=%d",
            event ? event->reason : -1,
            event ? DisconnectReasonName(event->reason) : "NO_EVENT",
            event ? event->rssi : 0);
        PublishStationEvent(
            wqn::WifiStationEvent::kDisconnected,
            event ? event->reason : 0,
            event ? event->rssi : 0);
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
        g_wifi_connected.store(true, std::memory_order_release);
        if (g_wifi_event_group != nullptr) {
            xEventGroupSetBits(g_wifi_event_group, kWifiConnectedBit);
        }
        StartSntpOnce();
        PublishStationEvent(wqn::WifiStationEvent::kConnected);
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
    if (g_sleep_quiescing.load(std::memory_order_acquire)) {
        ESP_LOGW(kTag, "WiFi start rejected during sleep quiesce");
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == nullptr || ssid[0] == '\0') {
        ESP_LOGW(kTag, "cannot start WiFi: empty SSID");
        return ESP_ERR_INVALID_ARG;
    }

    if (g_initialized.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "WiFi already initialized, switching to new credentials");
        ESP_RETURN_ON_ERROR(esp_wifi_disconnect(), kTag, "disconnect before reconfigure");
        // [reconfig-fix] delete old event group to avoid leak on recreate below
        if (g_wifi_event_group != nullptr) {
            vEventGroupDelete(g_wifi_event_group);
            g_wifi_event_group = nullptr;
        }
    } else {
        // [reconfig-fix] first-time-only init. esp_netif_init/esp_wifi_init
        // return INVALID_STATE on second call; esp_netif_create_default_wifi_sta
        // would leak a duplicate netif. Reconfigs skip this block.
        ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "init esp-netif");
        esp_err_t event_loop_result = esp_event_loop_create_default();
        if (event_loop_result != ESP_OK && event_loop_result != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(event_loop_result, kTag, "create default event loop");
        }
        if (esp_netif_create_default_wifi_sta() == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        // [prov-handoff] WiFi 驱动全程只 init 一次、常驻不 deinit（官方
        // esp-wifi-connect 范式）。配网（provision_manager::StartSoftAp）已
        // 在 APSTA 模式下调过 esp_wifi_init()，这里会返回
        // ESP_ERR_INVALID_STATE —— 复用活着的驱动即可，不要重 init。
        // 无配网路径下返回 ESP_OK，行为不变。
        const esp_err_t init_ret = esp_wifi_init(&init_config);
        if (init_ret != ESP_OK && init_ret != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(init_ret, kTag, "init WiFi");
        }
        ESP_RETURN_ON_ERROR(RegisterHandlers(), kTag, "register WiFi handlers");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set WiFi STA mode");
    }

    if (g_wifi_event_group == nullptr) {
        g_wifi_event_group = xEventGroupCreate();
        if (g_wifi_event_group == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != nullptr && password[0] != '\0') {
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode =
        (password == nullptr || password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), kTag, "set WiFi STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), kTag, "set WiFi power save");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start WiFi");

    g_initialized.store(true, std::memory_order_release);
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
    if (g_sleep_quiescing.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_initialized.load(std::memory_order_acquire)) {
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

esp_err_t ConnectWifiStationNow()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (g_sleep_quiescing.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_initialized.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = esp_wifi_connect();
    return result == ESP_ERR_WIFI_CONN ? ESP_OK : result;
#else
    return ESP_ERR_INVALID_STATE;
#endif
}

esp_err_t StopWifiStationRadio()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (!g_initialized.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    const esp_err_t result = esp_wifi_stop();
    if (result == ESP_OK || result == ESP_ERR_WIFI_NOT_STARTED ||
        result == ESP_ERR_WIFI_NOT_INIT) {
        g_wifi_connected.store(false, std::memory_order_release);
        if (g_wifi_event_group != nullptr) {
            xEventGroupClearBits(g_wifi_event_group, kWifiConnectedBit);
        }
        return ESP_OK;
    }
    return result;
#else
    return ESP_OK;
#endif
}

esp_err_t StartWifiStationRadio()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (g_sleep_quiescing.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_initialized.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_wifi_start();
#else
    return ESP_ERR_INVALID_STATE;
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
    return g_wifi_connected.load(std::memory_order_acquire);
#else
    return false;
#endif
}

bool IsWifiStationInitialized()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    return g_initialized.load(std::memory_order_acquire);
#else
    return false;
#endif
}

void SetWifiStationEventSink(WifiStationEventSink sink)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    g_event_sink.store(sink, std::memory_order_release);
#else
    (void)sink;
#endif
}

int GetWifiRssi()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (!g_wifi_connected.load(std::memory_order_acquire)) {
        return 0;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
#else
    return 0;
#endif
}

esp_err_t PrepareConnectivityForSleep(const power::PrepareSleepCommand& command)
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (command.deadline_us > 0 && esp_timer_get_time() >= command.deadline_us) {
        return ESP_ERR_TIMEOUT;
    }

    g_sleep_quiescing.store(true, std::memory_order_release);
    g_resume_after_sleep_abort.store(
        g_initialized.load(std::memory_order_acquire),
        std::memory_order_release);
    if (!g_initialized.load(std::memory_order_acquire)) {
        return ESP_OK;
    }

    const esp_err_t result = esp_wifi_stop();
    if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_STARTED &&
        result != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(kTag, "WiFi stop for sleep failed: %s", esp_err_to_name(result));
        return result;
    }
    g_wifi_connected.store(false, std::memory_order_release);
    if (g_wifi_event_group != nullptr) {
        xEventGroupClearBits(g_wifi_event_group, kWifiConnectedBit);
    }
    ESP_LOGI(kTag, "connectivity prepared for sleep: generation=%u",
             static_cast<unsigned>(command.generation));
    return ESP_OK;
#else
    (void)command;
    return ESP_OK;
#endif
}

void RollbackConnectivityAfterSleepAbort()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    const bool resume = g_resume_after_sleep_abort.exchange(false, std::memory_order_acq_rel);
    g_sleep_quiescing.store(false, std::memory_order_release);
    if (!resume || !g_initialized.load(std::memory_order_acquire)) {
        return;
    }
    const esp_err_t result = esp_wifi_start();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "WiFi restart after sleep rollback failed: %s", esp_err_to_name(result));
        return;
    }
    ESP_LOGI(kTag, "connectivity sleep preparation rolled back");
#endif
}

}  // namespace wqn
