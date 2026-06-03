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
#include "esp_ota_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "online_sync.h"
#include "power_manager.h"
#include "storage.h"
#include "wifi_manager.h"
#include "wqn_api.h"

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

TaskHandle_t g_wqn_online_task = nullptr;

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

    return true;
}

void WqnOnlineTask(void*)
{
    ESP_LOGI(kTag, "WQN online task started");
    while (true) {
        const bool synced = RunWqnOnlineRound();
        ulTaskNotifyTake(pdTRUE, synced ? pdMS_TO_TICKS(60000) : pdMS_TO_TICKS(10000));
    }
}

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

}  // namespace

namespace wqn {

void NotifyOnlineSyncRequested()
{
#if CONFIG_WQN_WIFI_STA_ENABLE
    if (g_wqn_online_task != nullptr) {
        xTaskNotifyGive(g_wqn_online_task);
    }
#endif
}

}  // namespace wqn

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(wqn::InitZectrixNote4SafePins());
    wqn::LogWakeupCause();
    wqn::PrintBootDiagnostics();

    ESP_ERROR_CHECK(wqn::InitStorage());
    LogTokenState();
    LogCachedProblemState();
    LogPendingReviewState();

    const bool contract_ok = wqn::RunContractFixtureSelfTest();
    if (!contract_ok) {
        ESP_LOGE(kTag, "contract fixture self-test failed; network flow remains disabled");
    }

    ConfirmRunningApp();
    ESP_ERROR_CHECK(wqn::InitAiSession());
    ESP_ERROR_CHECK(wqn::StartWifiStationIfEnabled());
    ESP_ERROR_CHECK(wqn::StartDeviceUiIfEnabled());
    ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::RunAudioSelfTestIfEnabled());

#if CONFIG_WQN_WIFI_STA_ENABLE
    if (contract_ok) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(StartWqnOnlineTask());
    }
#else
    ESP_LOGI(kTag, "pairing flow disabled because WiFi STA is disabled");
#endif

    ESP_LOGI(kTag, "firmware initialization complete");

    while (true) {
        wqn::EnterDeepSleepIfEnabled();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
