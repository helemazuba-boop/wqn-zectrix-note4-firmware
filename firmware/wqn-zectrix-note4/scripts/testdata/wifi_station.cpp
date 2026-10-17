#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#define CONFIG_WQN_WIFI_STA_ENABLE 1
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1,ESP_ERR_INVALID_ARG=2,ESP_ERR_WIFI_NOT_STARTED=3,ESP_ERR_WIFI_NOT_CONNECT=4,ESP_ERR_NO_MEM=5;
constexpr int WIFI_MODE_STA=1,WIFI_MODE_APSTA=3,WIFI_IF_STA=0,WIFI_AUTH_OPEN=0,WIFI_AUTH_WPA2_PSK=2,WPA3_SAE_PWE_BOTH=3;
enum wifi_ps_type_t {WIFI_PS_MIN_MODEM,WIFI_PS_NONE};
struct wifi_config_t {struct {uint8_t ssid[32]={},password[64]={};struct {int authmode=0;} threshold;int sae_pwe_h2e=0;} sta;};
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
#define ESP_RETURN_ON_ERROR(expression,...) do {const auto error=(expression);if(error!=ESP_OK)return error;}while(0)
void Log(const char*,const char*,...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
const char* kTag="station-host";
std::atomic<bool> g_sleep_quiescing{false},g_initialized{false},g_reconfigure_disconnect_pending{false},g_wifi_associated{false},g_wifi_connected{false},g_power_save_enabled{true};
void* g_wifi_event_group=nullptr;
constexpr int kWifiConnectedBit=1;
wifi_config_t configured{};
int mode=WIFI_MODE_APSTA,initializations=0,netifs=0,disconnects=0,starts=0;
int esp_netif_init() {return ESP_OK;}
int esp_event_loop_create_default() {return ESP_OK;}
void* esp_netif_create_default_wifi_sta() {++netifs;return reinterpret_cast<void*>(1);}
int esp_wifi_init(wifi_init_config_t*) {++initializations;return ESP_ERR_INVALID_STATE;}
int RegisterHandlers() {return ESP_OK;}
int esp_wifi_disconnect() {++disconnects;return ESP_ERR_WIFI_NOT_STARTED;}
void xEventGroupClearBits(void*,int) {}
void* xEventGroupCreate() {return reinterpret_cast<void*>(1);}
int esp_wifi_set_mode(int value) {mode=value;return ESP_OK;}
int esp_wifi_set_config(int,wifi_config_t* value) {configured=*value;return ESP_OK;}
int esp_wifi_set_ps(wifi_ps_type_t) {return ESP_OK;}
int esp_wifi_start() {++starts;return ESP_OK;}
void NoteWifiRadioOn() {}
@@STATION@@
int checks=0;
#define CHECK(value) do {++checks;if(!(value)){std::printf("FAIL station line %d: %s\n",__LINE__,#value);return 1;}}while(0)
int main() {
    const std::string ssid(32,'s'),password(64,'a');
    CHECK(StartWifiWithCredentials(ssid.c_str(),password.c_str())==ESP_OK);
    CHECK(mode==WIFI_MODE_STA&&initializations==1&&netifs==1);
    CHECK(std::memcmp(configured.sta.ssid,ssid.data(),32)==0&&std::memcmp(configured.sta.password,password.data(),64)==0);
    mode=WIFI_MODE_APSTA;
    CHECK(StartWifiWithCredentials("Second","")==ESP_OK);
    CHECK(mode==WIFI_MODE_STA&&initializations==1&&netifs==1&&disconnects==1);
    CHECK(configured.sta.threshold.authmode==WIFI_AUTH_OPEN&&configured.sta.password[0]==0);
    CHECK(StartWifiWithCredentials(std::string(33,'s').c_str(),"")==ESP_ERR_INVALID_ARG);
    CHECK(StartWifiWithCredentials("Second",std::string(65,'a').c_str())==ESP_ERR_INVALID_ARG);
    CHECK(starts==2&&disconnects==1);
    g_sleep_quiescing=true;
    CHECK(StartWifiWithCredentials("Second","")==ESP_ERR_INVALID_STATE&&starts==2);
    std::printf("WiFi station handoff: %d checks PASS (production path; driver ports only)\n",checks);
}
