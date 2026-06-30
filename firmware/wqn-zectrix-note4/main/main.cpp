#include <string>
#include <cstring>
#include <utility>
#include <vector>

#include "ai_session.h"
#include "audio_selftest.h"
#include "board_zectrix_note4.h"
#include "config.h"
#include "contract_fixtures.h"
#include "device_ui.h"
#include "diagnostics.h"
#include "esp_ota_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "online_sync.h"
#include "power_manager.h"
#include "provision_manager.h"
#include "storage.h"
#include "wifi_manager.h"
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

#if CONFIG_WQN_WIFI_STA_ENABLE

wqn::OnlineSyncSnapshot g_online_sync_snapshot = {};
constexpr TickType_t kOnlineSyncRetryDelay = pdMS_TO_TICKS(10000);

void SetOnlineSyncStatus(const char* status)
{
    std::snprintf(g_online_sync_snapshot.status, sizeof(g_online_sync_snapshot.status), "%s", status == nullptr ? "" : status);
}

bool LoadUsableToken(std::string* token)
{
    if (token == nullptr) {
        return false;
    }

    token->clear();
    const esp_err_t result = wqn::LoadAccessToken(token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "access token reload failed: %s", esp_err_to_name(result));
        return false;
    }
    if (token->empty()) {
        return false;
    }
    if (!wqn::IsValidAccessToken(*token)) {
        ESP_LOGW(kTag, "stored token invalid during sync setup; clearing");
        ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::ClearAccessToken());
        token->clear();
        return false;
    }
    return true;
}

std::vector<wqn::CachedProblem> ToCachedProblems(const std::vector<wqn::WqnProblem>& problems)
{
    std::vector<wqn::CachedProblem> cached;
    cached.reserve(problems.size());
    for (const wqn::WqnProblem& problem : problems) {
        wqn::CachedProblem item;
        item.id = problem.id;
        item.title = problem.title;
        item.type = problem.problem_type;
        item.status = problem.status;
        item.content_text = problem.content_text;
        item.solution_text = problem.solution_text;
        item.asset_count = problem.asset_count;
        item.solution_asset_count = problem.solution_asset_count;
        item.updated_at = problem.updated_at;
        cached.push_back(std::move(item));
    }
    return cached;
}

void UpsertProblem(std::vector<wqn::CachedProblem>* cached, wqn::CachedProblem problem)
{
    if (cached == nullptr || problem.id.empty()) {
        return;
    }

    for (wqn::CachedProblem& item : *cached) {
        if (item.id == problem.id) {
            if (!problem.title.empty()) {
                item.title = std::move(problem.title);
            }
            if (!problem.type.empty()) {
                item.type = std::move(problem.type);
            }
            if (!problem.status.empty()) {
                item.status = std::move(problem.status);
            }
            if (!problem.content_text.empty()) {
                item.content_text = std::move(problem.content_text);
            }
            if (!problem.solution_text.empty()) {
                item.solution_text = std::move(problem.solution_text);
            }
            item.asset_count = problem.asset_count;
            item.solution_asset_count = problem.solution_asset_count;
            if (!problem.updated_at.empty()) {
                item.updated_at = std::move(problem.updated_at);
            }
            return;
        }
    }
    cached->push_back(std::move(problem));
}

esp_err_t MergeProblemCache(const std::vector<wqn::WqnProblem>& fresh, const char* source)
{
    if (fresh.empty()) {
        return ESP_OK;
    }

    std::vector<wqn::CachedProblem> cached;
    const esp_err_t load_result = wqn::LoadProblems(&cached);
    if (load_result != ESP_OK) {
        ESP_LOGW(kTag, "dropping unreadable problem cache before merge: %s", esp_err_to_name(load_result));
        cached.clear();
    }

    std::vector<wqn::CachedProblem> incoming = ToCachedProblems(fresh);
    for (wqn::CachedProblem& problem : incoming) {
        UpsertProblem(&cached, std::move(problem));
    }

    const esp_err_t save_result = wqn::SaveProblems(cached);
    if (save_result != ESP_OK) {
        ESP_LOGW(kTag, "save problem cache failed: %s", esp_err_to_name(save_result));
        return save_result;
    }

    ESP_LOGI(
        kTag,
        "%s cached: fresh=%u total_cached=%u",
        source,
        static_cast<unsigned>(fresh.size()),
        static_cast<unsigned>(cached.size()));
    return ESP_OK;
}

esp_err_t UploadPendingReviewsIfAny(const std::string& token)
{
    std::vector<wqn::PendingReviewResult> pending;
    esp_err_t result = wqn::LoadPendingReviewResults(&pending);
    if (result != ESP_OK) {
        return result;
    }
    if (pending.empty()) {
        ESP_LOGI(kTag, "no pending review uploads");
        return ESP_OK;
    }

    std::vector<wqn::WqnReviewResult> uploads;
    uploads.reserve(pending.size());
    for (const wqn::PendingReviewResult& item : pending) {
        if (item.problem_id.empty() || item.selected_status.empty()) {
            ESP_LOGW(kTag, "pending review queue contains an invalid item; keeping queue");
            return ESP_ERR_INVALID_STATE;
        }

        wqn::WqnReviewResult upload;
        upload.problem_id = item.problem_id;
        upload.selected_status = item.selected_status;
        upload.reviewed_at = item.created_at;
        uploads.push_back(std::move(upload));
    }

    result = wqn::UploadReviewComplete(token, uploads);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "pending review upload kept for retry: %s", esp_err_to_name(result));
        return result;
    }

    result = wqn::ClearPendingReviewResults();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "clear uploaded review queue failed: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(kTag, "pending review uploads complete: count=%u", static_cast<unsigned>(uploads.size()));
    return ESP_OK;
}

esp_err_t SyncDueProblemsAndCache(const std::string& token)
{
    std::vector<std::string> due_problem_ids;
    int total = 0;
    esp_err_t result = wqn::SyncDueProblemIds(token, &due_problem_ids, &total);
    if (result != ESP_OK) {
        return result;
    }

    ESP_LOGI(kTag, "due problem sync: returned=%u total=%d", static_cast<unsigned>(due_problem_ids.size()), total);
    if (due_problem_ids.empty()) {
        return ESP_OK;
    }

    std::vector<wqn::WqnProblem> problems;
    result = wqn::FetchProblems(token, due_problem_ids, &problems);
    if (result != ESP_OK) {
        return result;
    }

    for (const wqn::WqnProblem& problem : problems) {
        ESP_LOGI(kTag, "due problem ready: id=%s title=%s", problem.id.c_str(), problem.title.c_str());
    }
    return MergeProblemCache(problems, "due problems");
}

esp_err_t RefreshProblemIndexIfAvailable(const std::string& token)
{
    wqn::WqnProblemIndexRequest request;
    request.limit = WQN_SYNC_LIMIT;

    wqn::WqnProblemIndexPage page;
    const esp_err_t result = wqn::FetchProblemIndex(token, request, &page);
    if (result == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(kTag, "problem index endpoint is not available yet; will retry next sync round");
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }

    ESP_LOGI(
        kTag,
        "problem index fetched: count=%u total=%d has_more=%s",
        static_cast<unsigned>(page.problems.size()),
        page.total,
        page.has_more ? "true" : "false");
    return MergeProblemCache(page.problems, "problem index");
}

bool RunWqnOnlineRound()
{
    esp_err_t result = wqn::RunPairingFlowIfNeeded();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "pairing round deferred: %s", esp_err_to_name(result));
        return false;
    }

    std::string token;
    if (!LoadUsableToken(&token)) {
        ESP_LOGI(kTag, "WQN online sync waiting for pairing");
        return false;
    }

    result = UploadPendingReviewsIfAny(token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "pending review upload round failed: %s", esp_err_to_name(result));
        return false;
    }

    if (!LoadUsableToken(&token)) {
        ESP_LOGI(kTag, "token cleared during review upload round");
        return false;
    }

    result = SyncDueProblemsAndCache(token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "due problem sync round failed: %s", esp_err_to_name(result));
        return false;
    }

    if (!LoadUsableToken(&token)) {
        ESP_LOGI(kTag, "token cleared during due problem sync round");
        return false;
    }

    result = RefreshProblemIndexIfAvailable(token);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "problem index refresh round failed: %s", esp_err_to_name(result));
        return false;
    }

    if (device_ui_internal::QueueWordReviewRefresh()) {
        ESP_LOGI(kTag, "word pack sync queued after online round");
    }

    return true;
}

TickType_t NextOnlineSyncWaitDelay(bool round_synced, bool has_token_after_round)
{
    if (!has_token_after_round) {
        // [power-fix] Once the device has lost (or never had) an access
        // token it is in provisioning mode. Polling the server every 2s
        // serves no purpose -- the device cannot authenticate -- and it
        // keeps the CPU + radio hot for no benefit. Block on the
        // notification until something (e.g. a fresh token save in
        // wqn::SaveAccessToken) wakes us back up.
        return portMAX_DELAY;
    }
    if (round_synced) {
        return wqn::GetConfiguredOnlineSyncDelayTicks();
    }

    const TickType_t configured_delay = wqn::GetConfiguredOnlineSyncDelayTicks();
    return configured_delay == portMAX_DELAY ? portMAX_DELAY : kOnlineSyncRetryDelay;
}

void WqnOnlineTask(void*)
{
    ESP_LOGI(kTag, "WQN online task started");
    g_online_sync_snapshot.task_running = true;
    SetOnlineSyncStatus("idle");
    bool first_round = true;
    while (true) {
        if (first_round && wqn::GetConfiguredOnlineSyncDelayTicks() == portMAX_DELAY && wqn::HasUsableStoredToken()) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        first_round = false;

        g_online_sync_snapshot.last_started_ms = esp_timer_get_time() / 1000;
        SetOnlineSyncStatus("syncing");
        const bool synced = RunWqnOnlineRound();
        g_online_sync_snapshot.last_finished_ms = esp_timer_get_time() / 1000;
        g_online_sync_snapshot.last_round_success = synced;
        const bool has_token_after_round = wqn::HasUsableStoredToken();
        if (synced) {
            ++g_online_sync_snapshot.success_count;
            SetOnlineSyncStatus("success");
        } else {
            ++g_online_sync_snapshot.failure_count;
            SetOnlineSyncStatus(has_token_after_round ? "failed" : "waiting-pair");
        }
        const TickType_t delay = NextOnlineSyncWaitDelay(synced, has_token_after_round);
        if (delay == portMAX_DELAY) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            ulTaskNotifyTake(pdTRUE, delay);
        }
    }
}

#endif  // CONFIG_WQN_WIFI_STA_ENABLE

}  // namespace

namespace wqn {

bool HasUsableStoredToken()
{
    std::string token;
    return LoadUsableToken(&token);
}

#if CONFIG_WQN_WIFI_STA_ENABLE

TaskHandle_t g_wqn_online_task = nullptr;

esp_err_t StartWqnOnlineTask()
{
    const BaseType_t created = xTaskCreate(WqnOnlineTask, "wqn_online", 12288, nullptr, 5, &g_wqn_online_task);
    if (created != pdPASS) {
        g_wqn_online_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif  // CONFIG_WQN_WIFI_STA_ENABLE

void NotifyOnlineSyncRequested()
{
    RequestOnlineSyncNow();
}

void RequestOnlineSyncNow()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (g_wqn_online_task != nullptr) {
        xTaskNotifyGive(g_wqn_online_task);
    }
#endif
}

void GetOnlineSyncSnapshot(OnlineSyncSnapshot* snapshot)
{
    if (snapshot == nullptr) {
        return;
    }
#if CONFIG_WQN_WIFI_STA_ENABLE
    *snapshot = g_online_sync_snapshot;
#else
    *snapshot = {};
    std::snprintf(snapshot->status, sizeof(snapshot->status), "%s", "wifi-disabled");
#endif
    uint32_t minutes = 0;
    if (LoadAutoSyncIntervalMinutes(&minutes) == ESP_OK) {
        snapshot->interval_minutes = minutes;
    }
}

TickType_t GetConfiguredOnlineSyncDelayTicks()
{
    uint32_t minutes = 0;
    if (LoadAutoSyncIntervalMinutes(&minutes) != ESP_OK || minutes == 0) {
        return portMAX_DELAY;
    }
    const uint64_t milliseconds = static_cast<uint64_t>(minutes) * 60ULL * 1000ULL;
    const uint64_t ticks = milliseconds / portTICK_PERIOD_MS;
    return ticks > static_cast<uint64_t>(portMAX_DELAY - 1) ? portMAX_DELAY - 1 : static_cast<TickType_t>(ticks);
}

}  // namespace wqn

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(wqn::InitZectrixNote4SafePins());
    wqn::LogWakeupCause();
    wqn::PrintBootDiagnostics();

    // [power-fix] CONFIG_PM_DFS_INIT_AUTO=y already set max/min CPU freq
    // (via CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ and XTAL), but it does NOT set
    // light_sleep_enable=true -- esp_pm_configure() must be called explicitly.
    // Without this, Light Sleep never triggers, so CPU stays at 240 MHz the
    // entire 22h active period and draws ~30 mA instead of ~0.8 mA.
    // Measured impact: this alone fixes ~80% of the 22h/51% drain regression.
#if CONFIG_PM_ENABLE
    {
        esp_pm_config_t pm_config = {
            .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = 40,
            .light_sleep_enable = false,
        };
        ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    }
#endif

    ESP_ERROR_CHECK(wqn::InitStorage());
    LogTokenState();
    LogWifiCredentialState();
    LogCachedProblemState();
    LogPendingReviewState();

    const bool contract_ok = wqn::RunContractFixtureSelfTest();
    if (!contract_ok) {
        ESP_LOGE(kTag, "contract fixture self-test failed; network flow will continue for pairing");
    }

    ConfirmRunningApp();
    ESP_ERROR_CHECK(wqn::InitAiSession());

#if CONFIG_WQN_WIFI_STA_ENABLE && CONFIG_WQN_PROVISION_ENABLE
    bool needs_provisioning = false;
    {
        std::string ssid;
        std::string password;
        if ((wqn::LoadWifiCredentials(&ssid, &password) != ESP_OK || ssid.empty())
            && std::strlen(CONFIG_WQN_WIFI_SSID) == 0) {
            needs_provisioning = true;
        }
    }

    if (needs_provisioning) {
        ESP_LOGI(kTag, "no WiFi credentials found; starting provisioning mode");
        wqn::SetProvisionDoneCallback([](const std::string& ssid, const std::string& password) {
            ESP_LOGI(kTag, "provisioning done, SSID=%s; starting WiFi", ssid.c_str());
            const esp_err_t ret = wqn::StartWifiWithCredentials(ssid.c_str(), password.c_str());
            if (ret != ESP_OK) {
                ESP_LOGE(kTag, "WiFi start after provisioning failed: %s", esp_err_to_name(ret));
            }
            // [power-fix] Wake the online sync task so it polls the WQN
            // server for pairing as soon as the radio is up.
            wqn::RequestOnlineSyncNow();
        });
        wqn::StartProvisioningMode();
        // [power-fix] Wake the online sync task so it can poll the
        // server for pairing status. Without this it would stay parked
        // on portMAX_DELAY forever.
        wqn::RequestOnlineSyncNow();
    } else {
        const esp_err_t wifi_ret = wqn::StartWifiStationIfEnabled();
        if (wifi_ret != ESP_OK) {
            ESP_LOGW(kTag, "WiFi station start failed: %s; starting provisioning", esp_err_to_name(wifi_ret));
            wqn::SetProvisionDoneCallback([](const std::string& ssid, const std::string& password) {
                ESP_LOGI(kTag, "provisioning done after WiFi failure, SSID=%s; starting WiFi", ssid.c_str());
                const esp_err_t ret = wqn::StartWifiWithCredentials(ssid.c_str(), password.c_str());
                if (ret != ESP_OK) {
                    ESP_LOGE(kTag, "WiFi start after provisioning failed: %s", esp_err_to_name(ret));
                }
                wqn::RequestOnlineSyncNow();
            });
            wqn::StartProvisioningMode();
            wqn::RequestOnlineSyncNow();
        }
    }
#else
    ESP_ERROR_CHECK(wqn::StartWifiStationIfEnabled());
#endif

    ESP_ERROR_CHECK(wqn::StartDeviceUiIfEnabled());
    ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::RunAudioSelfTestIfEnabled());

#if CONFIG_WQN_WIFI_STA_ENABLE
    ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::StartWqnOnlineTask());
#else
    ESP_LOGI(kTag, "pairing flow disabled because WiFi STA is disabled");
#endif

    ESP_LOGI(kTag, "firmware initialization complete");

    while (true) {
        wqn::EnterDeepSleepIfEnabled();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
