// Problem cloud task: pack content sync and WQNI image fetch. Mirrors
// ui/note_cloud.cpp for the problem domain (no sessions, no candidate paging:
// packs are browsed offline in fixed order). kPackSync rides the bulk lane;
// image fetches ride the interactive lane. The verdict commit runs on the
// dedicated persist worker (c3), not on a cloud lane.

#include "ui_internal.h"
#include "ui_runtime.h"
#include "persist_worker.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "esp_log.h"
#include "note_app.h"
#include "note_pack.h"
#include "problem_app.h"
#include "problem_cloud.h"
#include "problem_pack.h"
#include "problem_store.h"
#include "runtime/sleep_coordinator.h"
#include "services/sync_service.h"
#include "wqn_api.h"

namespace device_ui_internal {

namespace {
constexpr char kProblemTag[] = "wqn_problem_ui";
}

static std::atomic<bool> g_problem_cloud_busy{false};
wqn::runtime::SleepLease g_problem_sleep_lease;
ProblemCloudResult g_problem_result_slot;
uint32_t g_problem_result_generation = 0;

void FinishProblemCloudRequest()
{
    g_problem_sleep_lease.Reset();
    ClearCloudDomainBusyWatch(CloudDomain::kProblem);
    g_problem_cloud_busy.store(false, std::memory_order_release);
}

bool IsProblemCloudBusy()
{
    return g_problem_cloud_busy.load(std::memory_order_acquire);
}

bool QueueProblemCloudRequest(const ProblemCloudRequest& request)
{
    bool expected = false;
    if (!g_problem_cloud_busy.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    // The problem domain shares the note-cloud sleep blocker: both feed the
    // same screen and one in-flight request per domain keeps the blocker
    // accounting simple without growing the power_runtime enum.
    wqn::runtime::SleepLease lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kNoteCloud, "problem-cloud", __FILE__, __LINE__);
    if (!lease) {
        g_problem_cloud_busy.store(false, std::memory_order_release);
        return false;
    }
    g_problem_sleep_lease = std::move(lease);
    CloudJob job;
    job.domain = CloudDomain::kProblem;
    job.problem = request;
    if (!EnqueueCloudJob(job)) {
        FinishProblemCloudRequest();
        return false;
    }
    return true;
}

bool QueueProblemPackSync()
{
    ProblemCloudRequest request;
    request.op = ProblemCloudOp::kPackSync;
    return QueueProblemCloudRequest(request);
}

bool QueueProblemImageFetch(
    const std::string& problem_id,
    bool is_solution,
    uint8_t image_index,
    const std::string& image_id)
{
    if (problem_id.size() != 36 || image_index > 7 || image_id.size() != 64) {
        return false;
    }
    ProblemCloudRequest request;
    request.op = ProblemCloudOp::kFetchImage;
    std::snprintf(
        request.problem_id, sizeof(request.problem_id), "%s", problem_id.c_str());
    std::snprintf(request.image_id, sizeof(request.image_id), "%s", image_id.c_str());
    request.image_index = image_index;
    request.image_kind = is_solution ? 1 : 0;
    return QueueProblemCloudRequest(request);
}

void PumpProblemImageFetch(UiRuntime* runtime)
{
    if (runtime == nullptr || IsProblemCloudBusy()) return;
    // Only fetch while the note screen (which hosts the problem layer) is
    // visible; see PumpNoteImageFetch for the invisible-page rationale.
    if (runtime->state().screen != wqn::UiScreen::kNote) return;
    std::string problem_id;
    bool is_solution = false;
    uint8_t image_index = 0;
    std::string image_id;
    if (!runtime->TakeProblemImageRequest(
            &problem_id, &is_solution, &image_index, &image_id)) {
        return;
    }
    if (!QueueProblemImageFetch(problem_id, is_solution, image_index, image_id)) {
        ESP_LOGW(kProblemTag, "problem image queue rejected (busy=%d); will retry",
                 IsProblemCloudBusy() ? 1 : 0);
        runtime->RestoreProblemImageRequest();
    }
}

// [persist-worker] Verdict commit (durable outbox append). Moved off the cloud
// lane to the dedicated persist worker (c3): the user stays on the problem view
// ("正在记录…") behind the kPersisting gate, and the NEXT problem only shows
// once the durable result is applied and acked on the UI task. Two-phase
// reserve so a failed reservation never costs the armed verdict (mirrors
// word/note). The verdict carries no session snapshot, so unlike note there is
// no candidate-page rollback to guard -- problem cloud results are never
// deferred.
RefreshSchedule PumpProblemVerdictCommit(UiRuntime* runtime)
{
    if (runtime == nullptr) {
        return RefreshSchedule::kNone;
    }
    if (IsPersistKindBusy(PersistKind::kProblemVerdict)) {
        return RefreshSchedule::kNone;
    }
    if (!runtime->state().problem_app.verdict_effect_ready) {
        return RefreshSchedule::kNone;
    }
    PersistTicket ticket = TryReservePersist(PersistKind::kProblemVerdict);
    if (!ticket.valid()) {
        return RefreshSchedule::kNone;
    }
    const auto metadata = wqn::services::MakeDeviceRequestMetadata();
    std::string occurred_at = CurrentIsoTimestamp();
    if (occurred_at.empty()) {
        occurred_at = "2024-01-01T00:00:00Z";
    }
    wqn::DurableProblemObservation observation;
    if (!runtime->TakeProblemVerdictEffect(
            metadata.request_id, occurred_at, ticket.operation_id, &observation)) {
        CancelPersistReservation(ticket);
        return runtime->DispatchProblemVerdictTakeFailed().refresh;
    }
    EnqueueReservedProblemVerdict(ticket, std::move(observation));
    return RefreshSchedule::kNone;
}

ProblemCloudResult* PeekProblemCloudResult(uint32_t generation)
{
    if (generation == 0 || generation != g_problem_result_generation) {
        return nullptr;
    }
    return &g_problem_result_slot;
}

void SendProblemCloudResult()
{
    CloudResultReady ready;
    ready.domain = CloudDomain::kProblem;
    ready.generation = g_problem_result_generation;
    PublishCloudResult(CloudDomain::kProblem, g_problem_result_generation);
    (void)ready;
}

// Mirrors the problem sets into the note screen's mixed notebook list.
std::vector<wqn::NoteProblemSetRow> BuildProblemSetRows(
    const wqn::ProblemPackIndex& index)
{
    std::vector<wqn::NoteProblemSetRow> rows;
    rows.reserve(index.sets.size());
    for (const wqn::ProblemPackSet& set : index.sets) {
        wqn::NoteProblemSetRow row;
        row.set_id = set.set_id;
        row.name = set.name;
        row.entry_count = set.entry_count;
        rows.push_back(std::move(row));
    }
    return rows;
}

bool ApplyProblemCloudResult(wqn::UiState* state, ProblemCloudResult& result)
{
    if (state == nullptr) {
        return false;
    }
    if (result.op == ProblemCloudOp::kPackSync) {
        if (result.result == ESP_OK) {
            if (!result.pack_index_ready) {
                // No content changed; keep the current index and do not
                // manufacture a visible refresh.
                state->problem_app.cloud_sync_failed = false;
                state->problem_app.cloud_loaded_once = true;
                state->problem_app.cloud_sync_requested = false;
                return false;
            }
            wqn::ApplyNoteProblemSetRows(
                &state->note_app, BuildProblemSetRows(result.pack_index));
            wqn::ApplyProblemPackIndex(
                &state->problem_app, std::move(result.pack_index), result.message);
        } else {
            state->problem_app.cloud_sync_failed = true;
            state->problem_app.cloud_loaded_once = true;
            state->problem_app.cloud_sync_requested = false;
            state->problem_app.message =
                result.auth_required ? "请重新配对" : "错题同步失败";
        }
        // The mixed list only repaints when the note screen shows it.
        return state->screen == wqn::UiScreen::kNote;
    }
    if (result.op == ProblemCloudOp::kFetchImage) {
        wqn::ApplyProblemImageResult(
            &state->problem_app, result.result, result.image_id,
            std::move(result.image_wqni));
        // Repaint only when an image segment is actually on screen.
        return state->screen == wqn::UiScreen::kNote &&
               state->problem_app.active &&
               state->problem_app.mode == wqn::ProblemAppMode::kProblemView;
    }
    return false;
}

void ExecuteProblemCloudRequest(const ProblemCloudRequest& request)
{
    g_problem_result_slot = ProblemCloudResult{};
    ++g_problem_result_generation;
    if (g_problem_result_generation == 0) {
        ++g_problem_result_generation;
    }
    ProblemCloudResult& result = g_problem_result_slot;
    result.op = request.op;
    result.message.clear();

    std::string token;
    if (!LoadValidTokenForTodo(&token)) {
        result.auth_required = true;
        result.result = ESP_ERR_INVALID_STATE;
        SendProblemCloudResult();
        return;
    }

    if (request.op == ProblemCloudOp::kPackSync) {
        wqn::ProblemPackSyncResult sync;
        result.result = wqn::SyncProblemPacks(token, &sync);
        result.pack_index = std::move(sync.index);
        result.pack_index_ready = sync.index_ready;
        result.message = sync.message;
        result.auth_required = sync.auth_required;
    } else if (request.op == ProblemCloudOp::kFetchImage) {
        // Cache first: image ids are content hashes shared with the note
        // domain's ni_ pool, so a hit needs no network at all.
        const std::string image_id = request.image_id;
        result.image_id = image_id;
        auto wqni = std::make_shared<std::vector<uint8_t>>();
        result.result = wqn::LoadCachedNoteImage(image_id, wqni.get());
        if (result.result == ESP_OK) {
            ESP_LOGI(kProblemTag, "problem image cache hit: %.12s", image_id.c_str());
        } else {
            ESP_LOGI(kProblemTag,
                     "problem image cache miss (%s), downloading %.12s kind=%u index=%u",
                     esp_err_to_name(result.result), image_id.c_str(),
                     static_cast<unsigned>(request.image_kind),
                     static_cast<unsigned>(request.image_index));
            result.result = wqn::DownloadProblemImageV1(
                token,
                wqn::services::MakeDeviceRequestMetadata(),
                request.problem_id,
                request.image_kind == 1 ? wqn::WqnProblemImageKind::kSolution
                                        : wqn::WqnProblemImageKind::kAssets,
                request.image_index,
                image_id,
                wqni.get());
            if (result.result == ESP_OK) {
                result.result =
                    wqn::ValidateNoteImageWqni(wqni->data(), wqni->size());
            }
            ESP_LOGI(kProblemTag, "problem image download result: %s bytes=%u",
                     esp_err_to_name(result.result),
                     static_cast<unsigned>(wqni->size()));
            if (result.result == ESP_OK &&
                wqn::StoreCachedNoteImage(image_id, wqni->data(), wqni->size()) !=
                    ESP_OK) {
                // Cache persistence is best-effort; showing the image wins.
                ESP_LOGW(kProblemTag, "problem image cache store failed: %.12s",
                         image_id.c_str());
            }
        }
        if (result.result == ESP_OK) {
            result.image_wqni = std::move(wqni);
        }
    } else {
        result.result = ESP_ERR_INVALID_ARG;
    }

    if (result.result != ESP_OK) {
        std::string after_token;
        result.auth_required = !LoadValidTokenForTodo(&after_token);
    }
    SendProblemCloudResult();
}

}  // namespace device_ui_internal
