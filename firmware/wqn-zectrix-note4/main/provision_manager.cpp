#include "provision_manager.h"

#include <cstring>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "freertos/task.h"

#include "storage.h"

#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)

using wqn::ProvisionState;
using wqn::ProvisionDoneCallback;

namespace {

constexpr char kTag[] = "wqn_prov";

constexpr char kApSsidPrefix[] = "WQN_N4_";
constexpr int kApChannel = 6;
constexpr int kApMaxConnections = 4;

EventGroupHandle_t g_prov_event_group = nullptr;
constexpr EventBits_t kProvDoneBit = BIT0;
constexpr EventBits_t kProvFailBit = BIT1;

volatile ProvisionState g_prov_state = ProvisionState::kIdle;
std::string g_prov_ssid;
std::string g_prov_password;
std::string g_prov_ap_ssid;

httpd_handle_t g_http_server = nullptr;
TaskHandle_t g_prov_task_handle = nullptr;

ProvisionDoneCallback g_prov_done_callback;

const char kHtmlPage[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>WQN NOTE 4</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f5f5f5;min-height:100vh;display:flex;align-items:center;justify-content:center}"
    ".card{background:#fff;border-radius:12px;box-shadow:0 2px 12px rgba(0,0,0,.1);padding:32px;width:100%;max-width:420px}"
    "h1{font-size:20px;color:#1a1a1a;margin-bottom:8px;text-align:center}"
    ".subtitle{font-size:13px;color:#888;margin-bottom:24px;text-align:center}"
    ".field{margin-bottom:16px}"
    "label{font-size:13px;color:#555;display:block;margin-bottom:6px;font-weight:500}"
    "select,input{width:100%;padding:10px 12px;border:1px solid #ddd;border-radius:8px;font-size:15px;outline:none;transition:border-color .2s}"
    "select:focus,input:focus{border-color:#4f46e5}"
    ".info{font-size:12px;color:#888;margin-top:4px}"
    ".btn{width:100%;padding:12px;background:#4f46e5;color:#fff;border:none;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;margin-top:8px;transition:background .2s}"
    ".btn:hover{background:#4338ca}"
    ".btn:disabled{background:#a5a5a5;cursor:not-allowed}"
    ".btn-secondary{background:#f0f0f0;color:#333;margin-top:8px}"
    ".btn-secondary:hover{background:#e0e0e0}"
    ".msg{padding:10px 12px;border-radius:8px;margin-bottom:16px;font-size:13px;display:none}"
    ".msg-error{background:#fee2e2;color:#dc2626;display:block}"
    ".msg-success{background:#dcfce7;color:#16a34a;display:block}"
    ".spinner{display:inline-block;width:14px;height:14px;border:2px solid #fff;border-top-color:transparent;border-radius:50%;animation:spin .8s linear infinite;vertical-align:middle;margin-right:6px}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    "#loading{display:none}"
    "</style></head><body>"
    "<div class=\"card\">"
    "<h1>WQN NOTE 4</h1>"
    "<p class=\"subtitle\">Select a WiFi network to connect</p>"
    "<div class=\"msg\" id=\"msg\"></div>"
    "<form id=\"form\" action=\"/connect\" method=\"POST\">"
    "<div class=\"field\">"
    "<label for=\"ssid\">WiFi (SSID)</label>"
    "<select id=\"ssid\" name=\"ssid\" required>"
    "<option value=\"\">Scanning...</option>"
    "</select>"
    "<p class=\"info\" id=\"ssid_info\"></p>"
    "</div>"
    "<div class=\"field\">"
    "<label for=\"password\">Password (optional)</label>"
    "<input type=\"password\" id=\"password\" name=\"password\" placeholder=\"Leave blank for open networks\" autocomplete=\"off\">"
    "</div>"
    "<div id=\"submit_area\">"
    "<button type=\"submit\" class=\"btn\">Connect</button>"
    "<button type=\"button\" class=\"btn btn-secondary\" onclick=\"skip()\">Skip (offline mode)</button>"
    "</div>"
    "<div id=\"loading\">"
    "<button type=\"button\" class=\"btn\" disabled><span class=\"spinner\"></span>Connecting...</button>"
    "</div>"
    "</form>"
    "</div>"
    "<script>"
    "let scannedSsids = [];"
    "function showMsg(text, type) {"
    "  const el = document.getElementById('msg');"
    "  el.textContent = text;"
    "  el.className = 'msg msg-' + (type ? type : '');"
    "}"
    "function populateSsids(networks) {"
    "  scannedSsids = networks || [];"
    "  const sel = document.getElementById('ssid');"
    "  sel.innerHTML = '';"
    "  if (!scannedSsids.length) {"
    "    sel.innerHTML = '<option value=\"\">No networks found</option>';"
    "    return;"
    "  }"
    "  scannedSsids.forEach(function(n) {"
    "    var opt = document.createElement('option');"
    "    opt.value = n.ssid;"
    "    var label = n.ssid + (!n.secure ? ' (open)' : '') + ' sig:' + n.rssi + 'dBm';"
    "    opt.textContent = label;"
    "    sel.appendChild(opt);"
    "  });"
    "}"
    "document.getElementById('ssid').addEventListener('change', function() {"
    "  var n = scannedSsids.find(function(x){return x.ssid === this.value}, this);"
    "  document.getElementById('ssid_info').textContent ="
    "    (n && !n.secure) ? 'Open network, no password needed' : 'Enter WiFi password';"
    "}.bind(document.getElementById('ssid')));"
    "function skip() {"
    "  if (!confirm('Skip provisioning and enter offline mode?')) return;"
    "  window.location.href = '/skip';"
    "}"
    "document.getElementById('form').addEventListener('submit', function(e) {"
    "  e.preventDefault();"
    "  var ssid = document.getElementById('ssid').value;"
    "  if (!ssid) { showMsg('Please select a WiFi network', 'error'); return; }"
    "  document.getElementById('submit_area').style.display = 'none';"
    "  document.getElementById('loading').style.display = 'block';"
    "  showMsg('', '');"
    "  var fd = new FormData();"
    "  fd.append('ssid', ssid);"
    "  var pw = document.getElementById('password').value;"
    "  if (pw) fd.append('password', pw);"
    "  var xhr = new XMLHttpRequest();"
    "  xhr.open('POST', '/connect', true);"
    "  xhr.onload = function() {"
    "    if (xhr.status === 200) {"
    "      showMsg('Connecting... Device will switch networks.', 'success');"
    "    } else {"
    "      showMsg('Connection failed, please try again', 'error');"
    "      document.getElementById('submit_area').style.display = 'block';"
    "      document.getElementById('loading').style.display = 'none';"
    "    }"
    "  };"
    "  xhr.onerror = function() {"
    "    showMsg('Network error, please try again', 'error');"
    "    document.getElementById('submit_area').style.display = 'block';"
    "    document.getElementById('loading').style.display = 'none';"
    "  };"
    "  xhr.send(fd);"
    "});"
    "function pollStatus() {"
    "  var xhr = new XMLHttpRequest();"
    "  xhr.open('GET', '/status', true);"
    "  xhr.onload = function() {"
    "    if (xhr.status === 200) {"
    "      try {"
    "        var d = JSON.parse(xhr.responseText);"
    "        if (d.state === 'connected') {"
    "          showMsg('WiFi connected! Device is ready.', 'success');"
    "          document.getElementById('submit_area').style.display = 'none';"
    "          document.getElementById('loading').style.display = 'block';"
    "          document.getElementById('loading').querySelector('.btn').innerHTML = 'Connected';"
    "          return;"
    "        } else if (d.state === 'failed') {"
    "          showMsg('Connection failed. Check password and retry.', 'error');"
    "          document.getElementById('submit_area').style.display = 'block';"
    "          document.getElementById('loading').style.display = 'none';"
    "          return;"
    "        }"
    "      } catch(e) {}"
    "    }"
    "    setTimeout(pollStatus, 2000);"
    "  };"
    "  xhr.send();"
    "}"
    "setTimeout(pollStatus, 3000);"
    "(function() {"
    "  var xhr = new XMLHttpRequest();"
    "  xhr.open('GET', '/api/networks', true);"
    "  xhr.onload = function() {"
    "    if (xhr.status === 200) {"
    "      try { populateSsids(JSON.parse(xhr.responseText)); }"
    "      catch(e) { populateSsids([]); }"
    "    } else { populateSsids([]); }"
    "  };"
    "  xhr.onerror = function() { populateSsids([]); };"
    "  xhr.send();"
    "})();"
    "</script></body></html>";

esp_err_t HttpSendResponse(httpd_req_t* req, const char* data, size_t len)
{
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, data, len);
}

esp_err_t HttpHandleRoot(httpd_req_t* req)
{
    return HttpSendResponse(req, kHtmlPage, sizeof(kHtmlPage) - 1);
}

esp_err_t HttpHandleConnect(httpd_req_t* req)
{
    char buf[512] = {};
    const int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char parsed_ssid[33] = {};
    char parsed_password[65] = {};

    char* body = buf;
    while (*body) {
        while (*body == '&' || *body == '\r' || *body == '\n') {
            ++body;
        }
        char* eq = std::strchr(body, '=');
        if (!eq) {
            break;
        }
        *eq = '\0';
        char* key = body;
        char* value = eq + 1;
        char* amp = std::strchr(value, '&');
        if (amp) {
            *amp = '\0';
        }

        char decoded_key[33] = {};
        char decoded_value[65] = {};
        int ki = 0, vi = 0;
        for (const char* p = key; *p && ki < 32; ++p) {
            if (*p == '%' && p[1] && p[2]) {
                char hi[3] = {p[1], p[2], '\0'};
                decoded_key[ki++] = static_cast<char>(std::strtol(hi, nullptr, 16));
                p += 2;
            } else if (*p == '+') {
                decoded_key[ki++] = ' ';
            } else {
                decoded_key[ki++] = *p;
            }
        }
        for (const char* p = value; *p && vi < 64; ++p) {
            if (*p == '%' && p[1] && p[2]) {
                char hi[3] = {p[1], p[2], '\0'};
                decoded_value[vi++] = static_cast<char>(std::strtol(hi, nullptr, 16));
                p += 2;
            } else if (*p == '+') {
                decoded_value[vi++] = ' ';
            } else {
                decoded_value[vi++] = *p;
            }
        }

        if (std::strcmp(decoded_key, "ssid") == 0) {
            std::strncpy(parsed_ssid, decoded_value, sizeof(parsed_ssid) - 1);
        } else if (std::strcmp(decoded_key, "password") == 0) {
            std::strncpy(parsed_password, decoded_value, sizeof(parsed_password) - 1);
        }

        body = amp ? amp + 1 : nullptr;
        if (!body) {
            break;
        }
    }

    if (parsed_ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    g_prov_ssid = parsed_ssid;
    g_prov_password = parsed_password;
    g_prov_state = ProvisionState::kConnecting;

    ESP_LOGI(kTag, "user selected SSID: %s", parsed_ssid);

    const esp_err_t save_ret = wqn::SaveWifiCredentials(parsed_ssid, parsed_password);
    if (save_ret != ESP_OK) {
        ESP_LOGW(kTag, "WiFi credential save failed: %s", esp_err_to_name(save_ret));
    }

    if (g_prov_event_group != nullptr) {
        xEventGroupSetBits(g_prov_event_group, kProvDoneBit);
    }

    httpd_resp_set_hdr(req, "Content-Type", "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

esp_err_t HttpHandleSkip(httpd_req_t* req)
{
    ESP_LOGI(kTag, "user skipped provisioning");
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<html><head><meta charset=\"utf-8\"><title>Offline</title></head>"
        "<body style='font-family:sans-serif;text-align:center;padding-top:80px'>"
        "<h2>Offline mode</h2><p>Restart device to re-provision</p>"
        "<script>setTimeout(function(){location.href='/restart'},2000);</script>"
        "</body></html>",
        HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t HttpHandleStatus(httpd_req_t* req)
{
    const char* state_str = "idle";
    switch (g_prov_state) {
        case ProvisionState::kConnecting:
            state_str = "connecting";
            break;
        case ProvisionState::kConnected:
            state_str = "connected";
            break;
        case ProvisionState::kFailed:
            state_str = "failed";
            break;
        default:
            state_str = "waiting";
            break;
    }
    char response[64] = {};
    std::snprintf(response, sizeof(response), "{\"state\":\"%s\"}", state_str);
    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    return httpd_resp_send(req, response, std::strlen(response));
}

esp_err_t HttpHandleNetworks(httpd_req_t* req)
{
    if (g_prov_state != ProvisionState::kScanning) {
        httpd_resp_set_hdr(req, "Content-Type", "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    const esp_err_t scan_ret = esp_wifi_scan_start(&scan_config, true);
    if (scan_ret != ESP_OK) {
        ESP_LOGW(kTag, "WiFi scan failed: %s", esp_err_to_name(scan_ret));
        httpd_resp_set_hdr(req, "Content-Type", "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    uint16_t ap_count = 32;
    wifi_ap_record_t ap_info[32] = {};
    const esp_err_t list_ret = esp_wifi_scan_get_ap_records(&ap_count, ap_info);
    if (list_ret != ESP_OK) {
        ESP_LOGW(kTag, "failed to get AP records: %s", esp_err_to_name(list_ret));
        httpd_resp_set_hdr(req, "Content-Type", "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    char json[4096] = {};
    int json_len = std::snprintf(json, sizeof(json), "[");
    for (uint16_t i = 0; i < ap_count && json_len < static_cast<int>(sizeof(json)) - 128; ++i) {
        if (ap_info[i].ssid[0] == '\0') {
            continue;
        }
        const bool secure = ap_info[i].authmode != WIFI_AUTH_OPEN;
        if (i > 0) {
            json_len += std::snprintf(json + json_len, sizeof(json) - json_len, ",");
        }
        json_len += std::snprintf(
            json + json_len,
            sizeof(json) - json_len,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
            ap_info[i].ssid,
            static_cast<int>(ap_info[i].rssi),
            secure ? "true" : "false");
    }
    json_len += std::snprintf(json + json_len, sizeof(json) - json_len, "]");

    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    return httpd_resp_send(req, json, json_len);
}

esp_err_t HttpHandleRestart(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Content-Type", "text/plain");
    httpd_resp_send(req, "OK", 2);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t HttpHandleCpdRedirect(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t HttpHandleAppleCaptive(httpd_req_t* req)
{
    const char body[] =
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
        "<BODY>Success</BODY></HTML>";
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    return httpd_resp_send(req, body, sizeof(body) - 1);
}

esp_err_t HttpHandleAndroidCaptive(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t HttpHandleMicrosoftCaptive(httpd_req_t* req)
{
    const char body[] = "Microsoft Connect Test";
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Content-Type", "text/plain");
    return httpd_resp_send(req, body, sizeof(body) - 1);
}

esp_err_t StartHttpServer()
{
    if (g_http_server != nullptr) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 4096;
    config.max_uri_handlers = 12;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = HttpHandleRoot, .user_ctx = nullptr},
        {.uri = "/connect", .method = HTTP_POST, .handler = HttpHandleConnect, .user_ctx = nullptr},
        {.uri = "/skip", .method = HTTP_GET, .handler = HttpHandleSkip, .user_ctx = nullptr},
        {.uri = "/status", .method = HTTP_GET, .handler = HttpHandleStatus, .user_ctx = nullptr},
        {.uri = "/api/networks", .method = HTTP_GET, .handler = HttpHandleNetworks, .user_ctx = nullptr},
        {.uri = "/restart", .method = HTTP_GET, .handler = HttpHandleRestart, .user_ctx = nullptr},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = HttpHandleAndroidCaptive, .user_ctx = nullptr},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = HttpHandleAppleCaptive, .user_ctx = nullptr},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = HttpHandleMicrosoftCaptive, .user_ctx = nullptr},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = HttpHandleMicrosoftCaptive, .user_ctx = nullptr},
        {.uri = "/fwlink/", .method = HTTP_GET, .handler = HttpHandleCpdRedirect, .user_ctx = nullptr},
    };

    esp_err_t ret = httpd_start(&g_http_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (const auto& h : handlers) {
        if (httpd_register_uri_handler(g_http_server, &h) != ESP_OK) {
            ESP_LOGW(kTag, "failed to register URI handler: %s", h.uri);
        }
    }

    ESP_LOGI(kTag, "HTTP server started on port 80");
    return ESP_OK;
}

void StopHttpServer()
{
    if (g_http_server != nullptr) {
        httpd_stop(g_http_server);
        g_http_server = nullptr;
        ESP_LOGI(kTag, "HTTP server stopped");
    }
}

esp_err_t StartSoftAp()
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_netif_set_hostname(ap_netif, "WQN-NOTE4");
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "set hostname failed: %s", esp_err_to_name(ret));
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_config);
    if (ret != ESP_OK) {
        return ret;
    }

    // [fix] Read the AP MAC from the wifi driver, not the netif. The netif MAC
    // is all-zero before esp_wifi_start() fills it, which made every device
    // advertise the same SSID (WQN_N4_0000). esp_wifi_get_mac() is valid after
    // esp_wifi_init() and returns the real MAC the AP will use.
    uint8_t mac[6] = {};
    ret = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (ret != ESP_OK) {
        mac[3] = 0x12;
        mac[4] = 0x34;
    }

    char ap_ssid[16] = {};
    std::snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X", kApSsidPrefix,
                  static_cast<unsigned>(mac[3]), static_cast<unsigned>(mac[4]));
    g_prov_ap_ssid = ap_ssid;

    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(ap_ssid));
    ap_config.ap.channel = kApChannel;
    ap_config.ap.max_connection = kApMaxConnections;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }

    esp_netif_ip_info_t softap_ip = {};
    ret = esp_netif_get_ip_info(ap_netif, &softap_ip);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "get SoftAP IP failed: %s", esp_err_to_name(ret));
        softap_ip.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    }

    ret = esp_netif_dhcps_stop(ap_netif);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "stop DHCP server failed: %s", esp_err_to_name(ret));
    }

    esp_netif_dns_info_t dns_info = {};
    dns_info.ip.type = ESP_NETIF_DNS_MAIN;
    dns_info.ip.u_addr.ip4.addr = softap_ip.ip.addr;
    ret = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "set DNS info failed: %s", esp_err_to_name(ret));
    }

    ret = esp_netif_dhcps_start(ap_netif);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "start DHCP server failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(kTag, "SoftAP started: SSID=%s IP=" IPSTR, ap_ssid, IP2STR(&softap_ip.ip));

    TaskHandle_t dns_task = nullptr;
    uint32_t* dns_task_arg = new uint32_t(softap_ip.ip.addr);
    xTaskCreatePinnedToCore(
        [](void* arg) {
            const uint32_t esp_ip = *static_cast<uint32_t*>(arg);
            delete static_cast<uint32_t*>(arg);

            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock < 0) {
                ESP_LOGW("wqn_prov", "DNS proxy: socket create failed");
                vTaskDelete(nullptr);
                return;
            }

            int reuse = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            struct sockaddr_in bind_addr = {};
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_addr.s_addr = INADDR_ANY;
            bind_addr.sin_port = htons(53);
            if (bind(sock, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
                ESP_LOGW("wqn_prov", "DNS proxy: bind failed, closesocket=%d", sock);
                close(sock);
                vTaskDelete(nullptr);
                return;
            }

            struct in_addr esp_ip_addr = {};
            esp_ip_addr.s_addr = esp_ip;
            ESP_LOGI("wqn_prov", "DNS proxy listening on UDP:53, redirecting to %s", inet_ntoa(esp_ip_addr));

            uint8_t buf[512];
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            while (true) {
                ssize_t n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                      reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
                if (n <= 0) {
                    if (n < 0) ESP_LOGW("wqn_prov", "DNS proxy: recv error");
                    break;
                }

                if (n < 12) continue;

                uint8_t response[512] = {};
                std::memcpy(response, buf, n);

                uint8_t txn_id[2] = {buf[0], buf[1]};
                uint8_t flags[2] = {0x81, 0x80};
                uint8_t question_count[2] = {0x00, 0x01};
                uint8_t answer_count[2] = {0x00, 0x01};
                uint8_t authority[4] = {0x00, 0x00, 0x00, 0x00};

                response[0] = txn_id[0];
                response[1] = txn_id[1];
                response[2] = flags[0];
                response[3] = flags[1];
                response[4] = question_count[0];
                response[5] = question_count[1];
                // [dns-fix] Correct DNS header field mapping: bytes 6-7 =
                // ANCOUNT (answer count), 8-9 = NSCOUNT (authority), 10-11 =
                // ARCOUNT (additional). Was swapped -> ANCOUNT=0 so phones
                // dropped the response and never popped the captive portal.
                response[6] = answer_count[0];
                response[7] = answer_count[1];
                response[8] = authority[0];
                response[9] = authority[1];
                response[10] = authority[2];
                response[11] = authority[3];

                int resp_len = n;
                uint8_t* ptr = response + n;

                *ptr++ = 0xC0;
                *ptr++ = 0x0C;
                *ptr++ = 0x00;
                *ptr++ = 0x01;
                *ptr++ = 0x00;
                *ptr++ = 0x01;
                *ptr++ = 0x00;
                *ptr++ = 0x00;
                *ptr++ = 0x00;
                *ptr++ = 0xF0;
                *ptr++ = 0x00;
                *ptr++ = 0x04;
                *ptr++ = static_cast<uint8_t>((esp_ip >> 0) & 0xFF);
                *ptr++ = static_cast<uint8_t>((esp_ip >> 8) & 0xFF);
                *ptr++ = static_cast<uint8_t>((esp_ip >> 16) & 0xFF);
                *ptr++ = static_cast<uint8_t>((esp_ip >> 24) & 0xFF);

                resp_len = ptr - response;
                sendto(sock, reinterpret_cast<char*>(response), resp_len, 0,
                       reinterpret_cast<struct sockaddr*>(&client_addr), client_len);
            }
            close(sock);
            vTaskDelete(nullptr);
        },
        "wqn_dns", 8192, dns_task_arg, 1, &dns_task, 1);
    if (dns_task == nullptr) {
        delete dns_task_arg;  // [leak-fix] free arg on task create failure (lambda only deletes on success)
        ESP_LOGW(kTag, "DNS proxy task create failed");
    }

    return ESP_OK;
}

void ProvTask(void*)
{
    g_prov_state = ProvisionState::kStarting;
    ESP_LOGI(kTag, "provisioning task started");

    const esp_err_t ap_ret = StartSoftAp();
    if (ap_ret != ESP_OK) {
        ESP_LOGE(kTag, "SoftAP start failed: %s", esp_err_to_name(ap_ret));
        g_prov_state = ProvisionState::kFailed;
        if (g_prov_event_group != nullptr) {
            xEventGroupSetBits(g_prov_event_group, kProvFailBit);
        }
        g_prov_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const esp_err_t http_ret = StartHttpServer();
    if (http_ret != ESP_OK) {
        ESP_LOGE(kTag, "HTTP server start failed: %s", esp_err_to_name(http_ret));
        g_prov_state = ProvisionState::kFailed;
        if (g_prov_event_group != nullptr) {
            xEventGroupSetBits(g_prov_event_group, kProvFailBit);
        }
        g_prov_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    g_prov_state = ProvisionState::kScanning;
    ESP_LOGI(kTag, "provisioning server ready");

    if (g_prov_event_group == nullptr) {
        g_prov_event_group = xEventGroupCreate();
    }

    EventBits_t bits = xEventGroupWaitBits(
        g_prov_event_group, kProvDoneBit | kProvFailBit, pdFALSE, pdFALSE, portMAX_DELAY);

    StopHttpServer();

    if (bits & kProvDoneBit) {
        ESP_LOGI(kTag, "provisioning completed");
        if (g_prov_done_callback && !g_prov_ssid.empty()) {
            g_prov_done_callback(g_prov_ssid, g_prov_password);
        }
    } else {
        ESP_LOGW(kTag, "provisioning ended (failure or timeout)");
    }

    g_prov_task_handle = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

#endif  // CONFIG_WQN_WIFI_STA_ENABLE && CONFIG_WQN_PROVISION_ENABLE

namespace wqn {

esp_err_t StartProvisioningMode()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    if (g_prov_state != ProvisionState::kIdle && g_prov_state != ProvisionState::kFailed) {
        ESP_LOGW(kTag, "provisioning already active (state=%d)", static_cast<int>(g_prov_state));
        return ESP_OK;
    }

    g_prov_state = ProvisionState::kIdle;
    g_prov_ssid.clear();
    g_prov_password.clear();

    const BaseType_t created = xTaskCreate(ProvTask, "wqn_prov", 8192, nullptr, 5, &g_prov_task_handle);
    if (created != pdPASS) {
        g_prov_task_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    ESP_LOGI("wqn_prov", "provisioning disabled by config");
    return ESP_OK;
#endif
}

esp_err_t StopProvisioningMode()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    if (g_prov_event_group != nullptr) {
        xEventGroupSetBits(g_prov_event_group, kProvDoneBit);
    }
    StopHttpServer();
    g_prov_state = ProvisionState::kIdle;
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

ProvisionState GetProvisioningState()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    return g_prov_state;
#else
    return ProvisionState::kIdle;
#endif
}

const char* GetProvisioningApSsid()
{
#if defined(CONFIG_WQN_WIFI_STA_ENABLE) && defined(CONFIG_WQN_PROVISION_ENABLE)
    return g_prov_ap_ssid.c_str();
#else
    return "";
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
    g_prov_done_callback = std::move(callback);
#else
    (void)callback;
#endif
}

}  // namespace wqn
