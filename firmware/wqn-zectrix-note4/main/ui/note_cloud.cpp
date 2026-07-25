// Note cloud task: pack content sync, session start, candidate page fetch.
// Mirrors ui/word_cloud.cpp for the note domain (no search / AI lookup). The
// pack-sync op delegates to the pure wqn::SyncNotePacks orchestration.

#include "ui_internal.h"
#include "ui_runtime.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "esp_log.h"
#include "note_app.h"
#include "note_cloud.h"
#include "runtime/sleep_coordinator.h"
#include "services/sync_service.h"
#include "wqn_api.h"

namespace device_ui_internal {

namespace {
constexpr char kNoteTag[] = "wqn_note_ui";
}

QueueHandle_t g_note_request_queue = nullptr;
QueueHandle_t g_note_result_queue = nullptr;
TaskHandle_t g_note_task = nullptr;
static std::atomic<bool> g_note_cloud_busy{false};
wqn::runtime::SleepLease g_note_sleep_lease;
NoteCloudResult g_note_result_slot;
uint32_t g_note_result_generation = 0;

void FinishNoteCloudRequest()
{
    g_note_sleep_lease.Reset();
    g_note_cloud_busy.store(false, std::memory_order_release);
}

bool IsNoteCloudBusy()
{
    return g_note_cloud_busy.load(std::memory_order_acquire);
}

bool QueueNoteCloudRequest(const NoteCloudRequest& request)
{
    if (g_note_request_queue == nullptr) {
        return false;
    }
    bool expected = false;
    if (!g_note_cloud_busy.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    wqn::runtime::SleepLease lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kNoteCloud, "note-cloud", __FILE__, __LINE__);
    if (!lease) {
        g_note_cloud_busy.store(false, std::memory_order_release);
        return false;
    }
    g_note_sleep_lease = std::move(lease);
    if (xQueueSend(g_note_request_queue, &request, 0) != pdTRUE) {
        FinishNoteCloudRequest();
        return false;
    }
    return true;
}

bool QueueNotePackSync()
{
    NoteCloudRequest request;
    request.op = NoteCloudOp::kPackSync;
    return QueueNoteCloudRequest(request);
}

bool QueueNoteSessionStart(
    const wqn::protocol::note_study_v1::CreateSessionRequest& session)
{
    if (session.metadata.request_id.empty() || session.scope.notebook_ids.empty()) {
        return false;
    }
    NoteCloudRequest request;
    request.op = NoteCloudOp::kStartSession;
    std::snprintf(
        request.request_id, sizeof(request.request_id), "%s",
        session.metadata.request_id.c_str());
    std::snprintf(
        request.notebook_id, sizeof(request.notebook_id), "%s",
        session.scope.notebook_ids.front().c_str());
    return QueueNoteCloudRequest(request);
}

bool QueueNoteCandidatePage(
    const std::string& session_id,
    const wqn::protocol::note_study_v1::CandidatePageRequest& page)
{
    if (session_id.size() != 36 || page.metadata.request_id.empty() ||
        page.cursor.empty() || page.limit < 1 ||
        page.limit > static_cast<int>(
            wqn::protocol::note_study_v1::kMaxCandidatePageItems)) {
        return false;
    }
    NoteCloudRequest request;
    request.op = NoteCloudOp::kFetchSessionPage;
    std::snprintf(
        request.request_id, sizeof(request.request_id), "%s",
        page.metadata.request_id.c_str());
    std::snprintf(
        request.session_id, sizeof(request.session_id), "%s", session_id.c_str());
    std::snprintf(request.cursor, sizeof(request.cursor), "%s", page.cursor.c_str());
    request.limit = static_cast<uint16_t>(page.limit);
    return QueueNoteCloudRequest(request);
}

void PumpNoteCandidatePrefetch(UiRuntime* runtime)
{
    if (runtime == nullptr || IsNoteCloudBusy()) return;
    wqn::protocol::note_study_v1::CandidatePageRequest request;
    request.metadata = wqn::services::MakeDeviceRequestMetadata();
    std::string session_id;
    if (!runtime->TakeNoteCandidatePageRequest(&request, &session_id)) {
        return;
    }
    if (!QueueNoteCandidatePage(session_id, request)) {
        runtime->RestoreNoteCandidatePageRequest();
    }
}

bool QueueNoteImageFetch(
    const std::string& note_id, uint8_t image_index, const std::string& image_id)
{
    if (note_id.size() != 36 || image_index > 3 || image_id.size() != 64) {
        return false;
    }
    NoteCloudRequest request;
    request.op = NoteCloudOp::kFetchImage;
    std::snprintf(request.note_id, sizeof(request.note_id), "%s", note_id.c_str());
    std::snprintf(request.image_id, sizeof(request.image_id), "%s", image_id.c_str());
    request.image_index = image_index;
    return QueueNoteCloudRequest(request);
}

void PumpNoteImageFetch(UiRuntime* runtime)
{
    if (runtime == nullptr || IsNoteCloudBusy()) return;
    std::string note_id;
    uint8_t image_index = 0;
    std::string image_id;
    if (!runtime->TakeNoteImageRequest(&note_id, &image_index, &image_id)) {
        return;
    }
    if (!QueueNoteImageFetch(note_id, image_index, image_id)) {
        ESP_LOGW(kNoteTag, "note image queue rejected (busy=%d); will retry",
                 IsNoteCloudBusy() ? 1 : 0);
        runtime->RestoreNoteImageRequest();
    }
}

NoteCloudResult* PeekNoteCloudResult(uint32_t generation)
{
    if (generation == 0 || generation != g_note_result_generation) {
        return nullptr;
    }
    return &g_note_result_slot;
}

void SendNoteCloudResult()
{
    NoteCloudResultReady ready;
    ready.generation = g_note_result_generation;
    if (g_note_result_queue == nullptr ||
        xQueueSend(g_note_result_queue, &ready, pdMS_TO_TICKS(100)) != pdTRUE) {
        FinishNoteCloudRequest();
    }
}

bool IsNoteSessionInvalidError(const wqn::protocol::v3::Error& error)
{
    // Non-transient codes meaning the pinned session is unusable; drop it rather
    // than reuse the same session_id.
    return error.code == "SESSION_NOT_FOUND" ||
           error.code == "SESSION_NOT_ACTIVE" ||
           error.code == "NOTE_SESSION_SNAPSHOT_INCOMPLETE";
}

bool ApplyNoteCloudResult(wqn::UiState* state, NoteCloudResult& result)
{
    if (state == nullptr) {
        return false;
    }
    if (result.op == NoteCloudOp::kPackSync) {
        if (result.result == ESP_OK) {
            if (!result.pack_index_ready) {
                // No content changed; keep the current index and do not
                // manufacture a visible refresh.
                state->note_app.cloud_sync_failed = false;
                state->note_app.cloud_loaded_once = true;
                state->note_app.cloud_sync_requested = false;
                return false;
            }
            wqn::ApplyNotePackIndex(
                &state->note_app, std::move(result.pack_index), result.message);
        } else {
            state->note_app.cloud_sync_failed = true;
            state->note_app.cloud_loaded_once = true;
            state->note_app.cloud_sync_requested = false;
            state->note_app.message = result.auth_required ? "请重新配对" : "笔记同步失败";
        }
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == NoteCloudOp::kStartSession) {
        if (state->screen != wqn::UiScreen::kNote) {
            wqn::CancelNoteSessionStartResult(&state->note_app);
            state->note_app.mode = wqn::NoteAppMode::kNotebookList;
            state->note_app.message = "已取消本次打开";
            return false;
        }
        const bool applied = wqn::ApplyNoteSessionStartResult(
            &state->note_app, result.result, std::move(result.session));
        if (!applied) return false;
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == NoteCloudOp::kFetchSessionPage) {
        if (result.result != ESP_OK && IsNoteSessionInvalidError(result.protocol_error)) {
            wqn::ResetNoteSessionForServerInvalid(&state->note_app);
        } else {
            wqn::ApplyNoteCandidatePageResult(
                &state->note_app, result.result, std::move(result.candidate_page));
        }
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == NoteCloudOp::kFetchImage) {
        wqn::ApplyNoteImageResult(
            &state->note_app, result.result, result.image_id,
            std::move(result.image_wqni));
        // Repaint only when the image layer is actually on screen.
        return state->screen == wqn::UiScreen::kNote &&
               state->note_app.mode == wqn::NoteAppMode::kNoteImageView;
    }
    return false;
}

void NoteCloudTask(void*)
{
    ESP_LOGI(kNoteTag, "Note cloud task started");
    while (true) {
        NoteCloudRequest request;
        if (xQueueReceive(g_note_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        g_note_result_slot = NoteCloudResult{};
        ++g_note_result_generation;
        if (g_note_result_generation == 0) {
            ++g_note_result_generation;
        }
        NoteCloudResult& result = g_note_result_slot;
        result.op = request.op;
        result.message.clear();

        std::string token;
        if (!LoadValidTokenForTodo(&token)) {
            result.auth_required = true;
            result.result = ESP_ERR_INVALID_STATE;
            SendNoteCloudResult();
            continue;
        }

        if (request.op == NoteCloudOp::kPackSync) {
            wqn::NotePackSyncResult sync;
            result.result = wqn::SyncNotePacks(token, &sync);
            result.pack_index = std::move(sync.index);
            result.pack_index_ready = sync.index_ready;
            result.message = sync.message;
            result.auth_required = sync.auth_required;
        } else if (request.op == NoteCloudOp::kStartSession) {
            wqn::protocol::note_study_v1::CreateSessionRequest session;
            session.metadata = wqn::services::MakeDeviceRequestMetadata();
            session.metadata.request_id = request.request_id;
            session.mode = wqn::protocol::note_study_v1::Mode::kRecent;
            session.scope.notebook_ids.push_back(request.notebook_id);
            session.scope.include_archived = false;
            session.optional_count = 500;
            result.result = wqn::CreateNoteStudySessionV1(
                token, session, &result.session, &result.protocol_error);
        } else if (request.op == NoteCloudOp::kFetchSessionPage) {
            wqn::protocol::note_study_v1::CandidatePageRequest page;
            page.metadata = wqn::services::MakeDeviceRequestMetadata();
            page.metadata.request_id = request.request_id;
            page.cursor = request.cursor;
            page.limit = request.limit;
            result.result = wqn::FetchNoteStudyCandidatePageV1(
                token, request.session_id, page, &result.candidate_page,
                &result.protocol_error);
        } else if (request.op == NoteCloudOp::kFetchImage) {
            // Cache first: image ids are content hashes, so a hit needs no
            // network at all. Misses download, verify (size + sha256 in
            // wqn_api, WQNI header + CRC here) and then persist for next time.
            const std::string image_id = request.image_id;
            result.image_id = image_id;
            auto wqni = std::make_shared<std::vector<uint8_t>>();
            result.result = wqn::LoadCachedNoteImage(image_id, wqni.get());
            if (result.result == ESP_OK) {
                ESP_LOGI(kNoteTag, "note image cache hit: %.12s", image_id.c_str());
            } else {
                ESP_LOGI(kNoteTag, "note image cache miss (%s), downloading %.12s index=%u",
                         esp_err_to_name(result.result), image_id.c_str(),
                         static_cast<unsigned>(request.image_index));
                result.result = wqn::DownloadNoteImageV1(
                    token,
                    wqn::services::MakeDeviceRequestMetadata(),
                    request.note_id,
                    request.image_index,
                    image_id,
                    wqni.get());
                if (result.result == ESP_OK) {
                    result.result =
                        wqn::ValidateNoteImageWqni(wqni->data(), wqni->size());
                }
                ESP_LOGI(kNoteTag, "note image download result: %s bytes=%u",
                         esp_err_to_name(result.result),
                         static_cast<unsigned>(wqni->size()));
                if (result.result == ESP_OK &&
                    wqn::StoreCachedNoteImage(image_id, wqni->data(), wqni->size()) !=
                        ESP_OK) {
                    // Cache persistence is best-effort; showing the image wins.
                    ESP_LOGW(kNoteTag, "note image cache store failed: %.12s",
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
        SendNoteCloudResult();
    }
}

}  // namespace device_ui_internal
