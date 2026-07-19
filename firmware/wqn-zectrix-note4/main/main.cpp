#include <string>
#include <utility>
#include <vector>

#include "ai_session.h"
#include "audio_selftest.h"
#include "board_zectrix_note4.h"
#include "config.h"
#include "contract_fixtures.h"
#include "device_ui.h"
#include "diagnostics.h"
#include "driver/uart.h"
#include "esp_ota_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "online_sync.h"
#include "power_manager.h"
#include "power/wake_controller.h"
#include "runtime/sleep_coordinator.h"
#include "runtime/storage_schema.h"
#include "runtime/wake_context.h"
#include "services/audio_service.h"
#include "services/connectivity_service.h"
#include "storage.h"
#include "wqn_api.h"
#include "ui/ui_internal.h"

namespace {

constexpr char kTag[] = "wqn_main";

void LogCachedProblemState()
{
    std::vector<wqn::CachedProblem> problems;
    const esp_err_t result = wqn::LoadProblems(&problems);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "cached problems: count=%u", static_cast<unsigned>(problems.size()));
    } else {
        ESP_LOGW(kTag, "problem cache load failed: %s", esp_err_to_name(result));
    }
}

void LogPendingReviewState()
{
    std::vector<wqn::PendingReviewResult> reviews;
    const esp_err_t result = wqn::LoadPendingReviewResults(&reviews);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "pending review uploads: count=%u", static_cast<unsigned>(reviews.size()));
    } else {
        ESP_LOGW(kTag, "pending review queue load failed: %s", esp_err_to_name(result));
    }
}

void LogTokenState()
{
    std::string token;
    const esp_err_t result = wqn::LoadAccessToken(&token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "access token load failed: %s", esp_err_to_name(result));
        return;
    }

    if (token.empty()) {
        ESP_LOGI(kTag, "not paired: no stored access token");
    } else if (!wqn::IsValidAccessToken(token)) {
        ESP_LOGW(kTag, "stored access token has invalid shape");
    } else {
        ESP_LOGI(kTag, "paired token present: %s", wqn::MaskTokenForLog(token).c_str());
    }
}

void LogWifiCredentialState()
{
    std::string ssid;
    std::string password;
    const esp_err_t result = wqn::LoadWifiCredentials(&ssid, &password);
    if (result == ESP_OK && !ssid.empty()) {
        ESP_LOGI(kTag, "WiFi credentials stored: SSID=%s", ssid.c_str());
    } else {
        ESP_LOGI(kTag, "WiFi credentials: none stored (first boot or factory reset)");
    }
}

void ConfirmRunningApp()
{
    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "running app marked valid");
    } else {
        ESP_LOGW(kTag, "running app valid mark skipped: %s", esp_err_to_name(result));
    }
}

[[noreturn]] void EnterStorageRecoveryMode(
    esp_err_t error,
    uint32_t observed_generation)
{
    ESP_LOGE(
        kTag,
        "storage recovery mode: generation_observed=%lu error=%s; business startup blocked",
        static_cast<unsigned long>(observed_generation),
        esp_err_to_name(error));
    const esp_err_t recovery_ui = wqn::ShowStorageRecoveryUi(error);
    if (recovery_ui != ESP_OK) {
        ESP_LOGE(kTag, "storage recovery UI failed: %s", esp_err_to_name(recovery_ui));
    }
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(wqn::InitZectrixNote4SafePins());
    wqn::power::CaptureWakeContext();
    wqn::LogWakeupCause();
    wqn::PrintBootDiagnostics();

    // The M3 deep-sleep HIL gate passed. Active SleepLeases hold a shared
    // ESP_PM_NO_LIGHT_SLEEP lock; GPIO17 sleep-mode selection is disabled by
    // the Note4 HAL, so tickless idle may now enter automatic light sleep.
#if CONFIG_PM_ENABLE
    {
        esp_pm_config_t pm_config = {
            .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = 40,
            .light_sleep_enable = true,
        };
        ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
        ESP_LOGI(kTag, "automatic light sleep enabled after M3 deep-sleep HIL gate");
    }
#endif
    ESP_ERROR_CHECK(wqn::runtime::InitSleepCoordinator());
    wqn::RefreshUsbPowerSleepPolicy();

    // M7 boot gate: destructive local migration is complete (including its
    // durable generation marker) before any audio, storage, network, UI or
    // synchronization business task can start.
    const wqn::runtime::StorageSchemaBootResult storage_schema =
        wqn::runtime::EnsureStorageSchema();
    if (storage_schema.action ==
        wqn::runtime::StorageSchemaBootAction::kRestartRequired) {
        ESP_LOGW(
            kTag,
            "storage schema migration complete: generation=%lu reset_id=%s; restarting",
            static_cast<unsigned long>(wqn::runtime::kStorageSchemaGeneration),
            storage_schema.reset_id);
        // A deliberate migration restart must not make the bootloader treat a
        // newly installed OTA image as a failed first boot.
        ConfirmRunningApp();
        ESP_LOGW(kTag, "storage schema restart now");
        uart_wait_tx_idle_polling(
            static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM));
        esp_restart();
        return;
    }
    if (storage_schema.action ==
        wqn::runtime::StorageSchemaBootAction::kRecoveryRequired) {
        EnterStorageRecoveryMode(
            storage_schema.error,
            storage_schema.observed_generation);
    }

    const esp_err_t storage_init_result = wqn::InitStorage();
    if (storage_init_result != ESP_OK) {
        EnterStorageRecoveryMode(
            storage_init_result,
            wqn::runtime::kStorageSchemaGeneration);
    }
    ESP_ERROR_CHECK(wqn::services::StartAudioService());
    LogTokenState();
    LogWifiCredentialState();
    LogCachedProblemState();
    LogPendingReviewState();

    // [volume] Restore persisted playback volume into the process-wide cache so
    // the first playback after boot uses the user's level, not the 100% default.
    {
        int boot_volume = 100;
        if (wqn::LoadVolumePercent(&boot_volume) == ESP_OK) {
            ESP_LOGI(kTag, "playback volume restored: %d%%", boot_volume);
        }
    }

    const bool contract_ok = wqn::RunContractFixtureSelfTest();
    if (!contract_ok) {
        ESP_LOGE(kTag, "contract fixture self-test failed; network flow will continue for pairing");
    }

    ConfirmRunningApp();
    ESP_ERROR_CHECK(wqn::InitAiSession());

    // ConnectivityService is the sole owner of station, provisioning and
    // reconnect policy. app_main only starts the service.
    ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::services::StartConnectivity());

    ESP_ERROR_CHECK(wqn::StartDeviceUiIfEnabled());
    ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::RunAudioSelfTestIfEnabled());

#if CONFIG_WQN_WIFI_STA_ENABLE
    ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::StartWqnOnlineTask());
#else
    ESP_LOGI(kTag, "pairing flow disabled because WiFi STA is disabled");
#endif

    ESP_LOGI(kTag, "firmware initialization complete");
    ESP_ERROR_CHECK(wqn::StartPowerCoordinator());
}
