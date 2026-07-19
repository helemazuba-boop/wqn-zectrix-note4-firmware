#include "provision_manager.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "storage.h"
#include "runtime/sleep_coordinator.h"
#include "wifi_provision_portal.h"

#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)

namespace {

constexpr char kTag[] = "wqn_prov";
constexpr char kApSsidPrefix[] = "WQN_N4_";
constexpr EventBits_t kCredentialsSavedBit = BIT0;
constexpr EventBits_t kExitRequestedBit = BIT1;
constexpr EventBits_t kCancelRequestedBit = BIT2;
constexpr EventBits_t kStoppedBit = BIT3;
constexpr EventBits_t kStartRequestedBit = BIT4;

std::mutex g_prov_mutex;
EventGroupHandle_t g_prov_events = nullptr;
TaskHandle_t g_prov_task = nullptr;
std::unique_ptr<wqn::provision::WifiProvisionPortal> g_portal;
std::atomic<wqn::ProvisionState> g_prov_state{wqn::ProvisionState::kIdle};
std::atomic<bool> g_session_active{false};
wqn::runtime::SleepLease g_provision_sleep_lease;
std::string g_prov_ssid;
std::string g_prov_password;
char g_prov_ap_ssid[33] = {};
wqn::ProvisionDoneCallback g_prov_done_callback;

void PublishStopped()
{
    std::lock_guard<std::mutex> lock(g_prov_mutex);
    g_provision_sleep_lease.Reset();
    g_session_active = false;
    xEventGroupSetBits(g_prov_events, kStoppedBit);
}

void ProvisionTask(void*)
{
    ESP_LOGI(kTag, "provisioning owner task started");
    while (true) {
        xEventGroupWaitBits(
            g_prov_events,
            kStartRequestedBit,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY);
        g_prov_state = wqn::ProvisionState::kStarting;

        wqn::provision::WifiProvisionPortal* portal_ptr = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_prov_mutex);
            if (!g_portal) {
                g_portal.reset(new (std::nothrow) wqn::provision::WifiProvisionPortal());
            }
            portal_ptr = g_portal.get();
        }
        if (portal_ptr == nullptr) {
            ESP_LOGE(kTag, "allocate provisioning portal failed");
            g_prov_state = wqn::ProvisionState::kFailed;
            PublishStopped();
            continue;
        }

        portal_ptr->SetSsidPrefix(kApSsidPrefix);
        portal_ptr->SetCredentialsCallback([](const std::string& ssid, const std::string& password) {
            const esp_err_t result = wqn::SaveWifiCredentials(ssid, password);
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "save WiFi credentials failed: %s", esp_err_to_name(result));
                return result;
            }
            {
                std::lock_guard<std::mutex> lock(g_prov_mutex);
                g_prov_ssid = ssid;
                g_prov_password = password;
            }
            g_prov_state = wqn::ProvisionState::kConnecting;
            xEventGroupSetBits(g_prov_events, kCredentialsSavedBit);
            ESP_LOGI(kTag, "WiFi credentials saved: SSID=%s", ssid.c_str());
            return ESP_OK;
        });
        portal_ptr->SetExitCallback([]() {
            // HTTP handler only signals the owner task; it never tears down itself.
            xEventGroupSetBits(g_prov_events, kExitRequestedBit);
        });

        const esp_err_t start_result = portal_ptr->Start();
        if (start_result != ESP_OK) {
            ESP_LOGE(kTag, "provisioning portal start failed: %s", esp_err_to_name(start_result));
            g_prov_state = wqn::ProvisionState::kFailed;
            PublishStopped();
            continue;
        }
        {
            const std::string ap_ssid = portal_ptr->GetSsid();
            std::lock_guard<std::mutex> lock(g_prov_mutex);
            std::snprintf(g_prov_ap_ssid, sizeof(g_prov_ap_ssid), "%s", ap_ssid.c_str());
        }

        g_prov_state = wqn::ProvisionState::kScanning;
        ESP_LOGI(kTag, "provisioning server ready: SSID=%s", portal_ptr->GetSsid().c_str());

        const EventBits_t bits = xEventGroupWaitBits(
            g_prov_events,
            kExitRequestedBit | kCancelRequestedBit,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

        ESP_LOGI(kTag, "stopping provisioning portal");
        const esp_err_t stop_result = portal_ptr->Stop();
        {
            std::lock_guard<std::mutex> lock(g_prov_mutex);
            g_prov_ap_ssid[0] = '\0';
        }
        if (stop_result != ESP_OK) {
            ESP_LOGE(kTag, "provisioning portal stop failed: %s", esp_err_to_name(stop_result));
            g_prov_state = wqn::ProvisionState::kFailed;
        }

        const bool cancelled = (bits & kCancelRequestedBit) != 0;
        const bool completed = !cancelled && (bits & kExitRequestedBit) != 0 &&
            (xEventGroupGetBits(g_prov_events) & kCredentialsSavedBit) != 0;
        if (completed && stop_result == ESP_OK) {
            wqn::ProvisionDoneCallback callback;
            std::string ssid;
            std::string password;
            {
                std::lock_guard<std::mutex> lock(g_prov_mutex);
                callback = g_prov_done_callback;
                ssid = g_prov_ssid;
                password = g_prov_password;
            }
            g_prov_state = wqn::ProvisionState::kConnected;
            ESP_LOGI(kTag, "provisioning completed: SSID=%s", ssid.c_str());
            if (callback && !ssid.empty()) {
                callback(ssid, password);  // AP/DNS/httpd are stopped before STA handoff.
            }
        } else if (cancelled) {
            ESP_LOGI(kTag, "provisioning cancelled");
            g_prov_state = wqn::ProvisionState::kIdle;
        } else {
            ESP_LOGW(kTag, "provisioning stopped without saved credentials");
            g_prov_state = wqn::ProvisionState::kFailed;
        }

        PublishStopped();
    }
}

}  // namespace

#endif  // CONFIG_WQN_WIFI_STA_ENABLE && CONFIG_WQN_PROVISION_ENABLE

namespace wqn {

esp_err_t StartProvisioningMode()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    std::lock_guard<std::mutex> lock(g_prov_mutex);
    if (g_session_active) {
        ESP_LOGW(kTag, "provisioning already active (state=%s)", ProvisionStateLabel(g_prov_state.load()));
        return ESP_OK;
    }

    wqn::runtime::SleepLease lease =
        wqn::runtime::SleepLease::TryAcquire(
            wqn::runtime::SleepBlocker::kProvisioning, "provisioning", __FILE__, __LINE__);
    if (!lease) {
        ESP_LOGW(kTag, "provisioning start rejected: sleep quiesce in progress");
        return ESP_ERR_INVALID_STATE;
    }
    g_provision_sleep_lease = std::move(lease);

    if (g_prov_events == nullptr) {
        g_prov_events = xEventGroupCreate();
        if (g_prov_events == nullptr) {
            g_provision_sleep_lease.Reset();
            return ESP_ERR_NO_MEM;
        }
    }
    if (g_prov_task == nullptr) {
        const BaseType_t created = xTaskCreate(ProvisionTask, "wqn_prov", 8192, nullptr, 5, &g_prov_task);
        if (created != pdPASS) {
            g_prov_task = nullptr;
            g_prov_state = ProvisionState::kFailed;
            g_provision_sleep_lease.Reset();
            return ESP_ERR_NO_MEM;
        }
    }

    xEventGroupClearBits(
        g_prov_events,
        kCredentialsSavedBit | kExitRequestedBit | kCancelRequestedBit | kStoppedBit);
    g_prov_ssid.clear();
    g_prov_password.clear();
    g_prov_ap_ssid[0] = '\0';
    g_prov_state = ProvisionState::kStarting;
    g_session_active = true;
    xEventGroupSetBits(g_prov_events, kStartRequestedBit);
    return ESP_OK;
#else
    ESP_LOGI("wqn_prov", "provisioning disabled by config");
    return ESP_OK;
#endif
}

esp_err_t StopProvisioningMode()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    {
        std::lock_guard<std::mutex> lock(g_prov_mutex);
        if (!g_session_active) {
            g_prov_state = ProvisionState::kIdle;
            return ESP_OK;
        }
    }

    xEventGroupSetBits(g_prov_events, kCancelRequestedBit);
    const EventBits_t bits = xEventGroupWaitBits(
        g_prov_events, kStoppedBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if ((bits & kStoppedBit) == 0) {
        ESP_LOGE(kTag, "timed out waiting for provisioning task to stop");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

ProvisionState GetProvisioningState()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    return g_prov_state.load();
#else
    return ProvisionState::kIdle;
#endif
}

bool IsProvisioningActive()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    return g_session_active.load(std::memory_order_acquire);
#else
    return false;
#endif
}

std::string GetProvisioningApSsid()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    std::lock_guard<std::mutex> lock(g_prov_mutex);
    return g_prov_ap_ssid;
#else
    return {};
#endif
}

const char* ProvisionStateLabel(ProvisionState state)
{
    switch (state) {
        case ProvisionState::kIdle:
            return "idle";
        case ProvisionState::kStarting:
            return "starting";
        case ProvisionState::kScanning:
            return "scanning";
        case ProvisionState::kConnecting:
            return "connecting";
        case ProvisionState::kConnected:
            return "connected";
        case ProvisionState::kFailed:
            return "failed";
        default:
            return "unknown";
    }
}

void SetProvisionDoneCallback(ProvisionDoneCallback callback)
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    std::lock_guard<std::mutex> lock(g_prov_mutex);
    g_prov_done_callback = std::move(callback);
#else
    (void)callback;
#endif
}

}  // namespace wqn
