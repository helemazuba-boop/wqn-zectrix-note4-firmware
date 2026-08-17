#include "wifi_provision_portal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"

namespace wqn::provision {
namespace {

constexpr char kTag[] = "wqn_prov_portal";
constexpr uint64_t kScanIntervalUs = 10ULL * 1000 * 1000;
constexpr size_t kMaxSubmitBodySize = 512;
constexpr char kPortalUrl[] = "http://192.168.4.1/";

extern const char kIndexHtmlStart[] asm("_binary_wifi_configuration_html_start");
extern const char kDoneHtmlStart[] asm("_binary_wifi_configuration_done_html_start");

esp_err_t SendEmbeddedHtml(httpd_req_t* req, const char* html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t RegisterHandler(httpd_handle_t server, const httpd_uri_t& handler)
{
    const esp_err_t result = httpd_register_uri_handler(server, &handler);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "register URI %s failed: %s", handler.uri, esp_err_to_name(result));
    }
    return result;
}

}  // namespace

WifiProvisionPortal::WifiProvisionPortal() = default;

WifiProvisionPortal::~WifiProvisionPortal()
{
    const esp_err_t result = Stop();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "portal teardown failed: %s", esp_err_to_name(result));
    }
}

void WifiProvisionPortal::SetSsidPrefix(std::string prefix)
{
    ssid_prefix_ = std::move(prefix);
}

void WifiProvisionPortal::SetStoredNetworks(
    std::string primary_ssid,
    std::string backup_ssid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    primary_ssid_ = std::move(primary_ssid);
    backup_ssid_ = std::move(backup_ssid);
}

void WifiProvisionPortal::SetCredentialsCallback(CredentialsCallback callback)
{
    credentials_callback_ = std::move(callback);
}

void WifiProvisionPortal::SetExitCallback(ExitCallback callback)
{
    exit_callback_ = std::move(callback);
}

std::string WifiProvisionPortal::GetSsid() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ap_ssid_;
}

esp_err_t WifiProvisionPortal::Start()
{
    if (running_.exchange(true)) {
        return ESP_ERR_INVALID_STATE;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // An existing primary is enough to leave the portal without changing
        // it. This is needed when the user only adds or edits the backup.
        credentials_saved_ = !primary_ssid_.empty();
    }
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        running_ = false;
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        running_ = false;
        return result;
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&wifi_config);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        running_ = false;
        return result;
    }

    result = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &WifiProvisionPortal::WifiEventHandler,
        this,
        &wifi_event_instance_);
    if (result != ESP_OK) {
        running_ = false;
        return result;
    }

    result = StartAccessPoint();
    if (result == ESP_OK) {
        result = StartWebServer();
    }
    if (result == ESP_OK && scan_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = &WifiProvisionPortal::ScanTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wqn_prov_scan",
            .skip_unhandled_events = true,
        };
        result = esp_timer_create(&timer_args, &scan_timer_);
    }
    if (result == ESP_OK) {
        const esp_err_t scan_result = esp_wifi_scan_start(nullptr, false);
        if (scan_result != ESP_OK) {
            ESP_LOGW(kTag, "initial async WiFi scan failed: %s", esp_err_to_name(scan_result));
            ScheduleNextScan();
        }
        ESP_LOGI(kTag, "provisioning portal started: SSID=%s", GetSsid().c_str());
        return ESP_OK;
    }

    ESP_LOGE(kTag, "portal startup failed: %s", esp_err_to_name(result));
    Stop();
    return result;
}

esp_err_t WifiProvisionPortal::StartAccessPoint()
{
    ap_netif_ = esp_netif_create_default_wifi_ap();
    if (ap_netif_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif_);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif_, &ip_info), kTag, "set AP IP");
    esp_netif_dns_info_t dns_info = {};
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4 = ip_info.ip;
    ESP_RETURN_ON_ERROR(
        esp_netif_set_dns_info(ap_netif_, ESP_NETIF_DNS_MAIN, &dns_info),
        kTag,
        "set AP DNS");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif_), kTag, "start DHCP server");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_hostname(ap_netif_, "WQN-NOTE4"));

    uint8_t mac[6] = {};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), kTag, "read SoftAP MAC");
    char ssid[33] = {};
    std::snprintf(ssid, sizeof(ssid), "%s%02X%02X", ssid_prefix_.c_str(), mac[3], mac[4]);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ap_ssid_ = ssid;
    }

    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "set APSTA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), kTag, "set AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), kTag, "disable WiFi power save during provisioning");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start APSTA WiFi");

    if (!dns_server_) {
        dns_server_.reset(new (std::nothrow) DnsServer());
    }
    if (!dns_server_) {
        return ESP_ERR_NO_MEM;
    }
    return dns_server_->Start(ip_info.gw);
}

esp_err_t WifiProvisionPortal::StartWebServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    ESP_RETURN_ON_ERROR(httpd_start(&server_, &config), kTag, "start HTTP server");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) { return SendEmbeddedHtml(req, kIndexHtmlStart); },
        .user_ctx = this,
    };
    const httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) {
            return static_cast<WifiProvisionPortal*>(req->user_ctx)->HandleScan(req);
        },
        .user_ctx = this,
    };
    const httpd_uri_t networks = {
        .uri = "/networks",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) {
            return static_cast<WifiProvisionPortal*>(req->user_ctx)->HandleNetworks(req);
        },
        .user_ctx = this,
    };
    const httpd_uri_t submit = {
        .uri = "/submit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t* req) {
            return static_cast<WifiProvisionPortal*>(req->user_ctx)->HandleSubmit(req);
        },
        .user_ctx = this,
    };
    const httpd_uri_t done = {
        .uri = "/done.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) { return SendEmbeddedHtml(req, kDoneHtmlStart); },
        .user_ctx = this,
    };
    const httpd_uri_t exit = {
        .uri = "/exit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t* req) {
            return static_cast<WifiProvisionPortal*>(req->user_ctx)->HandleExit(req);
        },
        .user_ctx = this,
    };
    const httpd_uri_t captive = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) {
            return static_cast<WifiProvisionPortal*>(req->user_ctx)->HandleCaptiveRedirect(req);
        },
        .user_ctx = this,
    };

    for (const httpd_uri_t* handler : {&root, &scan, &networks, &submit, &done, &exit, &captive}) {
        const esp_err_t result = RegisterHandler(server_, *handler);
        if (result != ESP_OK) {
            httpd_stop(server_);
            server_ = nullptr;
            return result;
        }
    }
    return ESP_OK;
}

esp_err_t WifiProvisionPortal::Stop()
{
    if (!running_.exchange(false) && server_ == nullptr && ap_netif_ == nullptr) {
        return ESP_OK;
    }

    if (scan_timer_ != nullptr) {
        // The portal object and timer persist for the firmware lifetime. Do not
        // delete here: a task-dispatched callback may already be running after
        // esp_timer_stop() returns. running_=false plus esp_wifi_scan_stop()
        // makes such a callback harmless and prevents it from re-arming.
        esp_timer_stop(scan_timer_);
    }
    esp_wifi_scan_stop();

    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }

    esp_err_t result = ESP_OK;
    if (dns_server_ != nullptr) {
        // recvfrom is bounded by SO_RCVTIMEO, so cooperative exit is guaranteed.
        // Never release WiFi/netif resources while the DNS task can still use lwIP.
        result = dns_server_->Stop(portMAX_DELAY);
    }

    if (wifi_event_instance_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_instance_);
        wifi_event_instance_ = nullptr;
    }

    const esp_err_t stop_result = esp_wifi_stop();
    if (stop_result != ESP_OK && stop_result != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(kTag, "esp_wifi_stop failed: %s", esp_err_to_name(stop_result));
        result = stop_result;
    }
    if (ap_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ap_count_ = 0;
        ap_ssid_.clear();
    }
    ESP_LOGI(kTag, "provisioning portal stopped");
    return result;
}

void WifiProvisionPortal::WifiEventHandler(
    void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void*)
{
    if (event_base != WIFI_EVENT) {
        return;
    }
    auto* self = static_cast<WifiProvisionPortal*>(arg);
    if (!self->running_) {
        return;
    }
    if (event_id == WIFI_EVENT_SCAN_DONE) {
        self->HandleScanDone();
    }
}

void WifiProvisionPortal::ScanTimerCallback(void* arg)
{
    auto* self = static_cast<WifiProvisionPortal*>(arg);
    if (!self->running_) {
        return;
    }
    const esp_err_t result = esp_wifi_scan_start(nullptr, false);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "periodic async WiFi scan failed: %s", esp_err_to_name(result));
        self->ScheduleNextScan();
    }
}

void WifiProvisionPortal::HandleScanDone()
{
    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK) {
        ScheduleNextScan();
        return;
    }

    uint16_t count = std::min<uint16_t>(found, ap_records_.size());
    esp_err_t result = ESP_OK;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ap_records_.fill({});
        result = count == 0
            ? ESP_OK
            : esp_wifi_scan_get_ap_records(&count, ap_records_.data());
        if (result == ESP_OK) {
            std::sort(ap_records_.begin(), ap_records_.begin() + count, [](const auto& a, const auto& b) {
                return a.rssi > b.rssi;
            });
            ap_count_ = count;
        } else {
            ap_count_ = 0;
        }
    }
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "read WiFi scan records failed: %s", esp_err_to_name(result));
        esp_wifi_clear_ap_list();
        ScheduleNextScan();
        return;
    }
    if (count == 0) {
        esp_wifi_clear_ap_list();
    }
    ESP_LOGI(kTag, "async WiFi scan cached: %u APs", static_cast<unsigned>(count));
    ScheduleNextScan();
}

void WifiProvisionPortal::ScheduleNextScan()
{
    if (running_ && scan_timer_ != nullptr) {
        esp_timer_stop(scan_timer_);
        esp_timer_start_once(scan_timer_, kScanIntervalUs);
    }
}

esp_err_t WifiProvisionPortal::HandleScan(httpd_req_t* req)
{
    auto snapshot = std::unique_ptr<wifi_ap_record_t[]>(
        new (std::nothrow) wifi_ap_record_t[ap_records_.size()]);
    if (!snapshot) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    size_t snapshot_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_count = ap_count_;
        std::copy_n(ap_records_.begin(), snapshot_count, snapshot.get());
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    if (httpd_resp_sendstr_chunk(req, "{\"support_5g\":false,\"aps\":[") != ESP_OK) {
        return ESP_FAIL;
    }

    bool emitted = false;
    for (size_t i = 0; i < snapshot_count; ++i) {
        const auto& record = snapshot[i];
        if (record.ssid[0] == '\0') {
            continue;
        }

        char ssid[33] = {};
        std::memcpy(ssid, record.ssid, sizeof(record.ssid));
        cJSON* object = cJSON_CreateObject();
        if (object == nullptr ||
            !cJSON_AddStringToObject(object, "ssid", ssid) ||
            !cJSON_AddNumberToObject(object, "rssi", record.rssi) ||
            !cJSON_AddNumberToObject(object, "authmode", record.authmode)) {
            cJSON_Delete(object);
            return ESP_FAIL;
        }
        char* rendered = cJSON_PrintUnformatted(object);
        cJSON_Delete(object);
        if (rendered == nullptr) {
            return ESP_FAIL;
        }

        esp_err_t result = ESP_OK;
        if (emitted) {
            result = httpd_resp_sendstr_chunk(req, ",");
        }
        if (result == ESP_OK) {
            result = httpd_resp_sendstr_chunk(req, rendered);
        }
        cJSON_free(rendered);
        if (result != ESP_OK) {
            return result;
        }
        emitted = true;
    }

    if (httpd_resp_sendstr_chunk(req, "]}") != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_sendstr_chunk(req, nullptr);
}

esp_err_t WifiProvisionPortal::HandleNetworks(httpd_req_t* req)
{
    std::string primary_ssid;
    std::string backup_ssid;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        primary_ssid = primary_ssid_;
        backup_ssid = backup_ssid_;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr ||
        !cJSON_AddStringToObject(root, "primary_ssid", primary_ssid.c_str()) ||
        !cJSON_AddStringToObject(root, "backup_ssid", backup_ssid.c_str())) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    const esp_err_t result = httpd_resp_send(req, rendered, HTTPD_RESP_USE_STRLEN);
    cJSON_free(rendered);
    return result;
}

esp_err_t WifiProvisionPortal::HandleSubmit(httpd_req_t* req)
{
    if (req->content_len == 0 || req->content_len > kMaxSubmitBodySize) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload size");
    }

    char* body = static_cast<char*>(std::malloc(req->content_len + 1));
    if (body == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    size_t offset = 0;
    while (offset < req->content_len) {
        const int received = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (received <= 0) {
            std::free(body);
            return received == HTTPD_SOCK_ERR_TIMEOUT
                ? httpd_resp_send_408(req)
                : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
        }
        offset += static_cast<size_t>(received);
    }
    body[offset] = '\0';

    cJSON* json = cJSON_Parse(body);
    std::free(body);
    if (json == nullptr) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    cJSON* ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON* password_item = cJSON_GetObjectItemCaseSensitive(json, "password");
    cJSON* role_item = cJSON_GetObjectItemCaseSensitive(json, "role");
    cJSON* keep_password_item = cJSON_GetObjectItemCaseSensitive(json, "keep_password");
    const bool ssid_valid = cJSON_IsString(ssid_item) && ssid_item->valuestring != nullptr &&
        std::strlen(ssid_item->valuestring) > 0 && std::strlen(ssid_item->valuestring) <= 32;
    const bool password_valid = password_item == nullptr ||
        (cJSON_IsString(password_item) && password_item->valuestring != nullptr &&
         std::strlen(password_item->valuestring) <= 64);
    const bool role_valid = cJSON_IsString(role_item) && role_item->valuestring != nullptr &&
        (std::strcmp(role_item->valuestring, "primary") == 0 ||
         std::strcmp(role_item->valuestring, "backup") == 0);
    const bool keep_password_valid = keep_password_item == nullptr || cJSON_IsBool(keep_password_item);
    if (!ssid_valid || !password_valid || !role_valid || !keep_password_valid) {
        cJSON_Delete(json);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid credentials\"}", HTTPD_RESP_USE_STRLEN);
    }

    const std::string ssid = ssid_item->valuestring;
    const std::string password = password_item == nullptr ? "" : password_item->valuestring;
    const NetworkRole role = std::strcmp(role_item->valuestring, "primary") == 0
        ? NetworkRole::kPrimary
        : NetworkRole::kBackup;
    const bool keep_existing_password = cJSON_IsTrue(keep_password_item);
    cJSON_Delete(json);

    const esp_err_t save_result = credentials_callback_
        ? credentials_callback_(role, ssid, password, keep_existing_password)
        : ESP_ERR_INVALID_STATE;
    if (save_result != ESP_OK) {
        ESP_LOGE(kTag, "credential callback failed: %s", esp_err_to_name(save_result));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"error\":\"Save failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (role == NetworkRole::kPrimary) {
            primary_ssid_ = ssid;
        } else {
            backup_ssid_ = ssid;
        }
        credentials_saved_ = !primary_ssid_.empty();
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(
        req,
        role == NetworkRole::kPrimary
            ? "{\"success\":true,\"connect\":true}"
            : "{\"success\":true,\"connect\":false}",
        HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiProvisionPortal::HandleExit(httpd_req_t* req)
{
    bool can_exit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        can_exit = credentials_saved_;
    }
    if (!can_exit) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"error\":\"No credentials saved\"}", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    const esp_err_t result = httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    if (result == ESP_OK && exit_callback_) {
        exit_callback_();  // The bridge only signals its owner task; teardown never runs here.
    }
    return result;
}

esp_err_t WifiProvisionPortal::HandleCaptiveRedirect(httpd_req_t* req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", kPortalUrl);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, nullptr, 0);
}

}  // namespace wqn::provision
