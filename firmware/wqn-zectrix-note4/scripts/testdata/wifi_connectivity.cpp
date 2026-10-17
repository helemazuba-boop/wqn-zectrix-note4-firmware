#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#define CONFIG_WQN_WIFI_STA_ENABLE 1
#define CONFIG_WQN_PROVISION_ENABLE 1
#define RTC_DATA_ATTR
using esp_err_t=int;
using TickType_t=uint32_t;
using UBaseType_t=uint32_t;
using EventBits_t=uint32_t;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NOT_FOUND=3,ESP_ERR_NOT_SUPPORTED=4;
#define BIT0 1
#define BIT1 2
#define pdMS_TO_TICKS(value) (value)
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
void Log(const char*,const char*,...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expression,...) do {const auto error=(expression);if(error!=ESP_OK)return error;}while(0)
const char* esp_err_to_name(int) {return "host-port";}
int64_t now_us=0;
int64_t esp_timer_get_time() {return now_us;}
namespace wqn::power {struct PrepareSleepCommand {};}
@@DEFINITIONS@@
namespace wqn {
enum class ProvisionState:uint8_t {kIdle,kStarting,kScanning,kConnecting,kConnected,kFailed};
}
std::atomic<wqn::ProvisionState> g_prov_state{wqn::ProvisionState::kIdle};
wqn::WifiCredentialStore durable{};
int writes=0;
bool read_failure=false,write_failure=false,driver_connected=false,lease_allowed=true;
int configure_result=ESP_OK,connect_result=ESP_OK,connect_calls=0,stop_calls=0,notifications=0;
std::vector<std::string> configured;
namespace wqn {
constexpr size_t kWifiSsidMaxLen=32,kWifiPasswordMaxLen=64;
constexpr uint8_t kWifiCredentialStoreVersion=1;
esp_err_t LoadWifiCredentialStore(WifiCredentialStore* value) {*value=durable;return read_failure?ESP_FAIL:ESP_OK;}
esp_err_t SaveWifiCredentialStore(const WifiCredentialStore& value) {if(write_failure)return ESP_FAIL;durable=value;++writes;return ESP_OK;}
ProvisionState GetProvisioningState() {return g_prov_state.load();}
@@REPORT@@
@@STORAGE@@
esp_err_t StartWifiWithCredentials(const char* ssid,const char*) {configured.emplace_back(ssid);driver_connected=false;return configure_result;}
esp_err_t ConnectWifiStationNow() {++connect_calls;return connect_result;}
esp_err_t StopWifiStationRadio() {++stop_calls;driver_connected=false;return ESP_OK;}
bool IsWifiStationConnected() {return driver_connected;}
int GetWifiRssi() {return -55;}
bool IsWifiCredentialFailureReason(int reason) {return reason==99;}
}
namespace wqn::services {void NotifySyncConnectivityAvailable() {++notifications;}}
@@CONSTANTS@@
std::atomic<wqn::services::ConnectivityState> g_state{wqn::services::ConnectivityState::kOff};
std::atomic<uint8_t> g_demand_count{0},g_backoff_rounds_view{0};
std::atomic<bool> g_backoff_suspended_view{false};
wqn::WifiCredentialStore g_cred_store{};
uint8_t g_active_slot=0,g_slot_fail_count[2]={0,0};
bool g_attempt_active=false,online=false;
int g_snapshot_lock=0;
char g_snapshot_active_ssid[33]={},g_snapshot_backup_ssid[33]={};
bool g_snapshot_has_backup=false;
int64_t g_next_action_us=0;
ScheduledAction g_scheduled_action=ScheduledAction::kNone;
wqn::services::ConnectivityWaitResult last_failure=wqn::services::ConnectivityWaitResult::kCancelled;
void CopyCredential(char* out,size_t size,const char* value) {std::snprintf(out,size,"%s",value);}
void SetOnline(bool value,int=0) {online=value;}
void SetState(wqn::services::ConnectivityState value) {g_state=value;}
bool HoldConnectivityLease() {return lease_allowed;}
void ClearFailure() {}
void PublishFailure(wqn::services::ConnectivityWaitResult value) {last_failure=value;}
void RetryLegacyWifiMigrationIfNeeded() {}
void ReloadCredStoreFromNvs() {wqn::LoadWifiCredentialStore(&g_cred_store);}
void ClearScheduledAction();
void ScheduleAction(ScheduledAction,int64_t);
esp_err_t BeginConnectionAttempt();
void ScheduleBackoff(wqn::services::ConnectivityWaitResult failure=wqn::services::ConnectivityWaitResult::kUnavailable);
esp_err_t BeginProvisioning() {return ESP_OK;}
esp_err_t PrepareForSleep(const wqn::power::PrepareSleepCommand&) {return ESP_OK;}
esp_err_t RollbackAfterSleepAbort() {return ESP_OK;}
esp_err_t StopRadioAndSetOff(const char*) {g_state=wqn::services::ConnectivityState::kOff;return wqn::StopWifiStationRadio();}
@@FUNCTIONS@@
int checks=0;
#define CHECK(value) do {++checks;if(!(value)){std::printf("FAIL line %d: %s\n",__LINE__,#value);return 1;}}while(0)
int main() {
    using Role=wqn::WifiCredentialRole;
    using State=wqn::services::ConnectivityState;
    CHECK(wqn::SetWifiCredentialForRole(Role::kBackup,"Backup","synthetic",false)==ESP_ERR_INVALID_STATE);
    CHECK(wqn::SetWifiCredentialForRole(Role::kPrimary,"Primary","synthetic",false)==ESP_OK);
    CHECK(wqn::SetWifiCredentialForRole(Role::kBackup,"Backup","synthetic",false)==ESP_OK);
    CHECK(durable.count==2&&durable.preferred==0);
    CHECK(wqn::SetWifiCredentialForRole(Role::kPrimary,"Backup","",false)==ESP_ERR_INVALID_ARG);
    CHECK(wqn::SetWifiCredentialForRole(Role::kBackup,"Changed","",true)==ESP_ERR_INVALID_ARG);
    const int saved=writes;
    CHECK(wqn::SetWifiCredentialForRole(Role::kPrimary,"Primary","",true)==ESP_OK&&writes==saved);
    g_cred_store=durable;g_prov_state=wqn::ProvisionState::kConnecting;
    CHECK(KickConnectFromStore()==ESP_OK&&configured.back()=="Primary");
    CHECK(BeginConnectionAttempt()==ESP_OK);
    ConnectivityCommand disconnected;disconnected.type=CommandType::kWifiDisconnected;disconnected.reason=99;
    CHECK(HandleCommand(disconnected)==ESP_OK&&configured.back()=="Backup");
    CHECK(g_active_slot==1&&g_state==State::kConnecting&&g_prov_state==wqn::ProvisionState::kConnecting);
    ConnectivityCommand ip;ip.type=CommandType::kWifiGotIp;
    CHECK(HandleCommand(ip)==ESP_OK&&!online&&g_prov_state==wqn::ProvisionState::kConnecting);
    driver_connected=true;
    CHECK(HandleCommand(ip)==ESP_OK&&online&&g_prov_state==wqn::ProvisionState::kConnected);
    CHECK(durable.preferred==0&&writes==saved&&std::string(durable.slots[0].ssid)=="Primary");
    CHECK(g_state==State::kOnlineIdleTail);
    g_prov_state=wqn::ProvisionState::kScanning;g_state=State::kProvisioning;
    CHECK(HandleCommand(ip)==ESP_OK&&g_prov_state==wqn::ProvisionState::kScanning);
    g_prov_state=wqn::ProvisionState::kConnecting;g_state=State::kConnecting;g_cred_store=durable;
    g_slot_fail_count[0]=g_slot_fail_count[1]=0;g_active_slot=0;
    RecordAttemptFailure("auth",true);CHECK(g_active_slot==1);
    RecordAttemptFailure("auth",true);
    CHECK(g_state==State::kBackoff&&g_prov_state==wqn::ProvisionState::kFailed&&stop_calls>0);
    CHECK(durable.preferred==0&&writes==saved);
    g_cred_store=durable;g_prov_state=wqn::ProvisionState::kConnecting;
    CHECK(KickConnectFromStore()==ESP_OK&&configured.back()=="Primary");
    g_state=State::kConnecting;g_demand_count=1;g_slot_fail_count[0]=g_slot_fail_count[1]=0;
    RecordAttemptFailure("transient",false);CHECK(g_active_slot==0&&g_next_action_us==5000000);
    RecordAttemptFailure("transient",false);CHECK(g_active_slot==0&&g_next_action_us==10000000);
    RecordAttemptFailure("transient",false);CHECK(g_active_slot==1);
    std::printf("WiFi roles/connectivity: %d checks PASS (production paths; host ports only)\n",checks);
}
