#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi_types_generic.h"

#include "dns_server.h"

namespace wqn::provision {

enum class NetworkRole : uint8_t {
    kPrimary = 0,
    kBackup,
};

class WifiProvisionPortal {
public:
    using CredentialsCallback = std::function<esp_err_t(
        NetworkRole role,
        const std::string& ssid,
        const std::string& password,
        bool keep_existing_password)>;
    using ExitCallback = std::function<void()>;

    WifiProvisionPortal();
    ~WifiProvisionPortal();

    WifiProvisionPortal(const WifiProvisionPortal&) = delete;
    WifiProvisionPortal& operator=(const WifiProvisionPortal&) = delete;

    void SetSsidPrefix(std::string prefix);
    // Passwords deliberately never cross this component boundary. The portal
    // only needs SSID labels to render the editable primary/backup roles.
    void SetStoredNetworks(std::string primary_ssid, std::string backup_ssid);
    void SetCredentialsCallback(CredentialsCallback callback);
    void SetExitCallback(ExitCallback callback);

    esp_err_t Start();
    esp_err_t Stop();

    std::string GetSsid() const;
    bool IsRunning() const { return running_.load(); }

private:
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void ScanTimerCallback(void* arg);

    esp_err_t StartAccessPoint();
    esp_err_t StartWebServer();
    void HandleScanDone();
    void ScheduleNextScan();

    esp_err_t HandleScan(httpd_req_t* req);
    esp_err_t HandleNetworks(httpd_req_t* req);
    esp_err_t HandleSubmit(httpd_req_t* req);
    esp_err_t HandleExit(httpd_req_t* req);
    esp_err_t HandleCaptiveRedirect(httpd_req_t* req);

    mutable std::mutex mutex_;
    std::unique_ptr<DnsServer> dns_server_;
    httpd_handle_t server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    esp_event_handler_instance_t wifi_event_instance_ = nullptr;
    esp_timer_handle_t scan_timer_ = nullptr;
    std::array<wifi_ap_record_t, 32> ap_records_ = {};
    size_t ap_count_ = 0;
    std::string ssid_prefix_ = "WQN_N4_";
    std::string ap_ssid_;
    std::string primary_ssid_;
    std::string backup_ssid_;
    CredentialsCallback credentials_callback_;
    ExitCallback exit_callback_;
    bool credentials_saved_ = false;
    std::atomic<bool> running_{false};
};

}  // namespace wqn::provision
