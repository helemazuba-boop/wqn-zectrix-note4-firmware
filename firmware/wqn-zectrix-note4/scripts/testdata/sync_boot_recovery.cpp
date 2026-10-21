#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>

#define CONFIG_WQN_RTC_TIMEKEEP_ENABLE 0
#define CONFIG_PM_ENABLE 0
#define CONFIG_ESP_CONSOLE_UART_NUM 0
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101;
constexpr int ESP_ERR_INVALID_STATE = 0x103, ESP_ERR_INVALID_CRC = 0x109;
constexpr int pdPASS = 1, pdFALSE = 0;
#define pdMS_TO_TICKS(value) (value)
using uart_port_t = int;
using TaskHandle_t = void*;
using TimerHandle_t = void*;
using BaseType_t = int;
struct StaticTimer_t {};
struct StaticSemaphore_t {};
struct RecoveryStop {};
struct RestartStop {};
struct Abort { int error; };
const char* kTag = "host-boot";
const char* esp_err_to_name(int) { return "host-error"; }
void Log(const char*, const char*, ...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define ESP_ERROR_CHECK(value) do { const int e = (value); if (e != ESP_OK) throw Abort{e}; } while (0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(value) do { (void)(value); } while (0)
#define ESP_RETURN_ON_ERROR(value, ...) do { const int e = (value); if (e != ESP_OK) return e; } while (0)

std::vector<std::string> calls;
int journal_error = ESP_OK, recovery_error = ESP_OK, storage_error = ESP_OK;
int journal_reads = 0, mutex_creations = 0;
bool journal_loaded = false, fail_mutex = false, fail_timer = false, fail_task = false;
void* g_sync_journal_mutex = nullptr;
StaticSemaphore_t g_sync_journal_mutex_storage;
StaticTimer_t g_word_outbox_timer_storage;
TimerHandle_t g_word_outbox_timer = nullptr;
TaskHandle_t g_sync_service_task = nullptr;
std::atomic<bool> g_boot_policy_evaluated{false}, g_boot_outbox_pending{false};
std::atomic<bool> g_word_outbox_sync_requested{false};
std::atomic<uint32_t> g_full_sync_reasons{0};
constexpr uint32_t kFullSyncBoot = 2, kWordOutboxQuietPeriodMs = 5000;
void* xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
    ++mutex_creations;
    return fail_mutex ? nullptr : storage;
}
esp_err_t EnsureSyncJournalLoaded() {
    if (journal_loaded) return ESP_OK;
    calls.push_back("journal");
    ++journal_reads;
    if (journal_error == ESP_OK) journal_loaded = true;
    return journal_error;
}
bool BootFullSyncDue() { return false; }
void WordOutboxTimerCallback(TimerHandle_t) {}
TimerHandle_t xTimerCreateStatic(const char*, int, int, void*,
                                void (*)(TimerHandle_t), StaticTimer_t* storage) {
    calls.push_back("timer");
    return fail_timer ? nullptr : storage;
}
void SyncServiceTask(void*) {}
int xTaskCreate(void (*)(void*), const char*, int, void*, int, TaskHandle_t* handle) {
    calls.push_back("sync-task");
    *handle = fail_task ? nullptr : &calls;
    return fail_task ? 0 : pdPASS;
}
[[noreturn]] void vTaskDelay(int) { throw RecoveryStop{}; }
void esp_restart() { calls.push_back("restart"); throw RestartStop{}; }
void uart_wait_tx_idle_polling(int) {}
void LogTokenState() {}
void LogWifiCredentialState() {}
void ConfirmRunningApp() { calls.push_back("confirm-app"); }

namespace wqn {
constexpr int kNote4I2cPort = 0, kNote4I2cSda = 47, kNote4I2cScl = 48, kNote4I2cClockHz = 100000;
esp_err_t InitZectrixNote4SafePins() { return ESP_OK; }
esp_err_t InitPowerHardware(int, int, int, int) { return ESP_OK; }
void LogWakeupCause() {}
void PrintBootDiagnostics() {}
void RefreshUsbPowerSleepPolicy() {}
esp_err_t InitStorage() { calls.push_back("storage"); return storage_error; }
esp_err_t LoadVolumePercent(int*) { return ESP_OK; }
void SetPlaybackVolumeCache(int) {}
bool RunContractFixtureSelfTest() { return true; }
esp_err_t InitAiSession() { calls.push_back("ai"); return ESP_OK; }
esp_err_t InitOpenCodeSession() { calls.push_back("agent"); return ESP_OK; }
esp_err_t StartDeviceUiIfEnabled() { calls.push_back("ui"); return ESP_OK; }
esp_err_t RunAudioSelfTestIfEnabled() { return ESP_OK; }
esp_err_t StartPowerCoordinator() { calls.push_back("power"); return ESP_OK; }
esp_err_t ShowStorageRecoveryUi(int error) {
    calls.push_back("recovery-ui"); recovery_error = error; return ESP_OK;
}
namespace runtime {
constexpr uint32_t kStorageSchemaGeneration = 3;
enum class StorageSchemaBootAction { kReady, kRestartRequired, kRecoveryRequired };
struct StorageSchemaBootResult {
    StorageSchemaBootAction action = StorageSchemaBootAction::kReady;
    int error = ESP_OK;
    uint32_t observed_generation = 3;
    const char* reset_id = "host";
};
StorageSchemaBootResult schema_result;
StorageSchemaBootResult EnsureStorageSchema() { calls.push_back("schema"); return schema_result; }
esp_err_t InitSleepCoordinator() { return ESP_OK; }
// Diagnostics is an independent startup dependency; this fixture exercises
// journal/startup gates rather than the diagnostic worker implementation.
void InitMemoryDiagnostics() {}
struct WakeContext { bool deep_sleep_resume = false; };
const WakeContext& GetWakeContext() { static WakeContext wake; return wake; }
}
namespace power {
void CaptureWakeContext() {}
namespace timekeep { void SeedSystemTimeFromBuildTimeIfNeeded() {} }
}
namespace services {
esp_err_t StartAudioService() { calls.push_back("audio"); return ESP_OK; }
bool EvaluateSyncWorkAtBoot() { g_boot_policy_evaluated.store(true); return true; }
@@PREPARE@@
#if CONFIG_WQN_WIFI_STA_ENABLE
@@START_SYNC@@
#endif
}
}

@@RECOVERY@@
@@APP_MAIN@@

int checks = 0, failures = 0;
void Check(bool result, const char* label) {
    ++checks; failures += !result;
    std::printf("%s: %s\n", result ? "PASS" : "FAIL", label);
}
bool Called(const char* name) { return std::find(calls.begin(), calls.end(), name) != calls.end(); }
void Reset() {
    calls.clear(); journal_reads = mutex_creations = 0;
    journal_error = recovery_error = storage_error = ESP_OK;
    journal_loaded = fail_mutex = fail_timer = fail_task = false;
    g_sync_journal_mutex = g_sync_service_task = g_word_outbox_timer = nullptr;
    g_boot_policy_evaluated.store(false);
    g_boot_outbox_pending.store(false);
    g_word_outbox_sync_requested.store(false);
    g_full_sync_reasons.store(0);
    wqn::runtime::schema_result = {};
}
bool BootRecovers() {
    try { app_main(); } catch (RecoveryStop&) { return true; }
    return false;
}
bool NoBusiness() {
    return !Called("audio") && !Called("ai") && !Called("agent") &&
        !Called("ui") && !Called("sync-task") && !Called("power");
}
int main() {
    for (int error : {ESP_ERR_INVALID_CRC, ESP_ERR_INVALID_STATE}) {
        Reset();
        wqn::runtime::schema_result.action = wqn::runtime::StorageSchemaBootAction::kRecoveryRequired;
        wqn::runtime::schema_result.error = error;
        Check(BootRecovers() && NoBusiness() && !Called("storage") && !Called("journal"),
              "schema preservation/replay failure blocks storage and all business startup");
        Check(Called("recovery-ui") && recovery_error == error,
              "schema failure reaches recovery UI with original error");
    }
    Reset();
    wqn::runtime::schema_result.action = wqn::runtime::StorageSchemaBootAction::kRestartRequired;
    bool restarted = false;
    try { app_main(); } catch (RestartStop&) { restarted = true; }
    Check(restarted && NoBusiness() && !Called("storage") && !Called("journal") &&
          std::find(calls.begin(), calls.end(), "confirm-app") <
          std::find(calls.begin(), calls.end(), "restart"),
          "completed authorized reset confirms app and restarts before business");
#if CONFIG_WQN_WIFI_STA_ENABLE
    for (int error : {ESP_ERR_INVALID_CRC, ESP_FAIL}) {
        Reset(); journal_error = error;
        Check(BootRecovers() && NoBusiness(), "invalid journal/recovery-write failure blocks all business tasks");
        Check(Called("recovery-ui") && recovery_error == error, "recovery UI receives original storage failure");
    }
    Reset(); fail_mutex = true;
    Check(BootRecovers() && NoBusiness() && journal_reads == 0 && recovery_error == ESP_ERR_NO_MEM,
          "journal mutex allocation failure enters pre-business recovery");
    Reset();
    app_main();
    Check(Called("ui") && Called("sync-task") && Called("power"), "valid or recovered journal starts normal services");
    Check(journal_reads == 1 && mutex_creations == 1, "task startup reuses boot-validated journal and mutex");
    Check(std::find(calls.begin(), calls.end(), "journal") < std::find(calls.begin(), calls.end(), "audio"),
          "journal gate completes before first business task");
    Reset(); journal_error = ESP_ERR_INVALID_CRC;
    Check(wqn::services::StartSyncService() == ESP_ERR_INVALID_CRC && !Called("timer") && !Called("sync-task"),
          "direct sync startup cannot bypass journal validation");
    for (bool timer_failure : {false, true}) {
        Reset(); fail_timer = timer_failure; fail_task = !timer_failure;
        bool aborted = false;
        try { app_main(); } catch (Abort& abort) { aborted = abort.error == ESP_ERR_NO_MEM; }
        Check(aborted && !Called("power"), "sync task/timer startup failure cannot continue to power coordinator");
    }
#else
    Reset(); journal_error = ESP_ERR_INVALID_CRC;
    app_main();
    Check(Called("ui") && Called("power") && !Called("sync-task"), "WiFi-disabled build still boots normally");
    Check(journal_reads == 0 && mutex_creations == 0, "WiFi-disabled preparation needs no sync journal resources");
#endif
    Reset(); storage_error = ESP_FAIL;
    Check(BootRecovers() && NoBusiness() && journal_reads == 0, "existing storage initialization gate is preserved");
    std::printf("%d PASS / %d FAIL (WiFi=%d Agent=%d; host startup seams, NOT HIL)\n",
                checks - failures, failures, CONFIG_WQN_WIFI_STA_ENABLE, CONFIG_WQN_AGENT_ENABLE);
    return failures ? 1 : 0;
}
