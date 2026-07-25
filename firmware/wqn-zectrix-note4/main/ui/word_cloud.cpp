// Word review cloud task: pack sync, review submit, online search, AI lookup.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "ui_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "esp_log.h"
#include "runtime/sleep_coordinator.h"
#include "services/sync_service.h"
#include "storage.h"
#include "word_pack.h"
#include "wqn_api.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

static std::atomic<bool> g_word_cloud_busy{false};
wqn::runtime::SleepLease g_word_sleep_lease;
WordCloudResult g_word_result_slot;
uint32_t g_word_result_generation = 0;

void FinishWordCloudRequest()
{
    g_word_sleep_lease.Reset();
    g_word_cloud_busy.store(false, std::memory_order_release);
}

bool IsWordCloudBusy()
{
    return g_word_cloud_busy.load(std::memory_order_acquire);
}

bool QueueWordCloudRequest(const WordCloudRequest& request)
{
    bool expected = false;
    if (!g_word_cloud_busy.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    wqn::runtime::SleepLease lease =
        wqn::runtime::SleepLease::TryAcquire(
            wqn::runtime::SleepBlocker::kWordCloud, "word-cloud", __FILE__, __LINE__);
    if (!lease) {
        g_word_cloud_busy.store(false, std::memory_order_release);
        return false;
    }
    g_word_sleep_lease = std::move(lease);
    CloudJob job;
    job.domain = CloudDomain::kWord;
    job.word = request;
    if (!EnqueueCloudJob(job)) {
        FinishWordCloudRequest();
        return false;
    }
    return true;
}

bool QueueWordReviewRefresh()
{
    WordCloudRequest request;
    request.op = WordCloudOp::kPackSync;
    return QueueWordCloudRequest(request);
}

bool QueueWordSessionStart(
    const wqn::protocol::word_study_v1::CreateSessionRequest& session)
{
    if (session.metadata.request_id.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kStartSession;
    std::snprintf(
        request.request_id,
        sizeof(request.request_id),
        "%s",
        session.metadata.request_id.c_str());
    request.study_mode = static_cast<uint8_t>(session.mode);
    return QueueWordCloudRequest(request);
}

bool QueueWordCandidatePage(
    const std::string& session_id,
    const wqn::protocol::word_study_v1::CandidatePageRequest& page)
{
    if (session_id.size() != 36 || page.metadata.request_id.empty() ||
        page.cursor.empty() || page.limit < 1 ||
        page.limit > static_cast<int>(
            wqn::protocol::word_study_v1::kMaxCandidatePageItems)) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kFetchSessionPage;
    std::snprintf(
        request.request_id,
        sizeof(request.request_id),
        "%s",
        page.metadata.request_id.c_str());
    std::snprintf(
        request.session_id,
        sizeof(request.session_id),
        "%s",
        session_id.c_str());
    std::snprintf(
        request.cursor,
        sizeof(request.cursor),
        "%s",
        page.cursor.c_str());
    request.limit = static_cast<uint16_t>(page.limit);
    return QueueWordCloudRequest(request);
}

void PumpWordCandidatePrefetch(UiRuntime* runtime)
{
    if (runtime == nullptr || IsWordCloudBusy()) return;
    wqn::protocol::word_study_v1::CandidatePageRequest request;
    request.metadata = wqn::services::MakeDeviceRequestMetadata();
    std::string session_id;
    if (!runtime->TakeWordCandidatePageRequest(&request, &session_id)) {
        return;
    }
    if (!QueueWordCandidatePage(session_id, request)) {
        runtime->RestoreWordCandidatePageRequest();
    }
}

bool QueueWordSearch(const wqn::WqnWordSearchRequest& search)
{
    if (search.query.empty() && search.prefix.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kSearch;
    const std::string query = !search.query.empty() ? search.query : search.prefix;
    std::snprintf(request.query, sizeof(request.query), "%s", query.c_str());
    return QueueWordCloudRequest(request);
}

bool QueueWordAiLookup(const wqn::WqnWordAiLookupRequest& lookup)
{
    if (lookup.query.empty() && lookup.prefix.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kAiLookup;
    const std::string query = !lookup.query.empty() ? lookup.query : lookup.prefix;
    std::snprintf(request.query, sizeof(request.query), "%s", query.c_str());
    return QueueWordCloudRequest(request);
}

WordCloudResult* PeekWordCloudResult(uint32_t generation)
{
    if (generation == 0 || generation != g_word_result_generation) {
        return nullptr;
    }
    return &g_word_result_slot;
}

void SendWordCloudResult()
{
    CloudResultReady ready;
    ready.domain = CloudDomain::kWord;
    ready.generation = g_word_result_generation;
    if (g_cloud_result_queue == nullptr ||
        xQueueSend(g_cloud_result_queue, &ready, pdMS_TO_TICKS(100)) != pdTRUE) {
        FinishWordCloudRequest();
    }
}

bool IsWordSessionInvalidError(const wqn::protocol::v3::Error& error)
{
    // Codes that mean the pinned session is unusable server-side. These are not
    // transient, so the device must drop the session instead of retrying it.
    return error.code == "SESSION_NOT_FOUND" ||
           error.code == "SESSION_NOT_ACTIVE" ||
           error.code == "WORD_SESSION_SNAPSHOT_INCOMPLETE";
}

bool ApplyWordCloudResult(wqn::UiState* state, WordCloudResult& result)
{
    if (state == nullptr) {
        return false;
    }
    if (result.op == WordCloudOp::kPackSync) {
        if (result.result == ESP_OK) {
            if (!result.pack_index_ready) {
                // The remote cursor contained no content changes. Keep the
                // current session/index and do not manufacture a visible
                // word-result revision or a full EPD refresh.
                state->word_app.cloud_sync_failed = false;
                state->word_app.cloud_loaded_once = true;
                state->word_app.cloud_sync_requested = false;
                return false;
            }
            wqn::ApplyWordPackIndex(
                &state->word_app,
                std::move(result.pack_index),
                result.message);
        } else {
            state->word_app.cloud_sync_failed = true;
            state->word_app.cloud_loaded_once = true;
            state->word_app.cloud_sync_requested = false;
            state->word_app.message = result.auth_required ? "请重新配对" : "单词同步失败";
        }
        BuildHomeSummary(state);
        return true;
    }

    if (result.op == WordCloudOp::kStartSession) {
        if (state->screen != wqn::UiScreen::kWord) {
            wqn::CancelWordSessionStartResult(&state->word_app);
            state->word_app.mode = wqn::WordAppMode::kHome;
            state->word_app.message = "已取消本轮准备";
            return false;
        }
        const bool applied = wqn::ApplyWordSessionStartResult(
            &state->word_app,
            result.result,
            result.session_compact_result,
            result.session_persist_result,
            std::move(result.persisted_session));
        if (!applied) return false;
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == WordCloudOp::kFetchSessionPage) {
        if (result.result != ESP_OK && IsWordSessionInvalidError(result.protocol_error)) {
            wqn::ResetWordSessionForServerInvalid(&state->word_app);
        } else {
            wqn::ApplyWordCandidatePageResult(
                &state->word_app,
                result.result,
                std::move(result.candidate_page));
        }
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == WordCloudOp::kSearch) {
        if (state->screen != wqn::UiScreen::kWord) {
            wqn::CancelWordLookupResult(&state->word_app);
            return false;
        }
        const bool applied = result.result == ESP_OK
            ? wqn::ApplyWordSearchResult(
                  &state->word_app, result.query, result.search)
            : wqn::ApplyWordLookupFailure(
                  &state->word_app,
                  result.query,
                  result.auth_required ? "请重新配对" : "在线搜索失败");
        if (!applied) return false;
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == WordCloudOp::kAiLookup) {
        if (state->screen != wqn::UiScreen::kWord) {
            wqn::CancelWordLookupResult(&state->word_app);
            return false;
        }
        const bool applied = result.result == ESP_OK
            ? wqn::ApplyWordAiLookupResult(
                  &state->word_app, result.query, result.lookup)
            : wqn::ApplyWordLookupFailure(
                  &state->word_app,
                  result.query,
                  result.auth_required ? "请重新配对" : "AI 查词失败");
        if (!applied) return false;
        BuildHomeSummary(state);
        return true;
    }
    return false;
}

void ExecuteWordCloudRequest(const WordCloudRequest& request)
{
    g_word_result_slot = WordCloudResult{};
    ++g_word_result_generation;
    if (g_word_result_generation == 0) {
        ++g_word_result_generation;
    }
    WordCloudResult& result = g_word_result_slot;
    result.op = request.op;
    result.query = request.query;
    result.message.clear();

    std::string token;
    if (!LoadValidTokenForTodo(&token)) {
        result.auth_required = true;
        result.result = ESP_ERR_INVALID_STATE;
        SendWordCloudResult();
        return;
    }

    if (request.op == WordCloudOp::kPackSync) {
        wqn::WqnWordPackManifest local_manifest;
        bool had_local_manifest = true;
        bool manifest_content_changed = false;
        result.result = wqn::LoadWordPackManifest(&local_manifest);
        if (result.result == ESP_ERR_NOT_FOUND) {
            had_local_manifest = false;
            result.result = wqn::ResetWordPackStorageCache();
            if (result.result == ESP_OK) {
                local_manifest = {};
            }
        } else if (result.result != ESP_OK) {
            ESP_LOGW(
                kTag,
                "local word pack manifest is incompatible; reset cache: %s",
                esp_err_to_name(result.result));
            had_local_manifest = false;
            result.result = wqn::ResetWordPackStorageCache();
            if (result.result == ESP_OK) {
                local_manifest = {};
            }
        }
        constexpr size_t kMaxManifestPagesPerSync = 32;
        size_t page_count = 0;
        bool has_more = result.result == ESP_OK;
        while (result.result == ESP_OK && has_more &&
               page_count < kMaxManifestPagesPerSync) {
            ++page_count;
            wqn::WqnWordPackManifest manifest_delta;
            const auto metadata = wqn::services::MakeDeviceRequestMetadata();
            result.result = wqn::FetchWordPackManifest(
                token,
                metadata,
                local_manifest.cursor,
                &manifest_delta);
            if (result.result != ESP_OK) {
                break;
            }
            if (manifest_delta.has_more &&
                manifest_delta.cursor <= local_manifest.cursor) {
                result.result = ESP_ERR_INVALID_RESPONSE;
                result.message = "词库游标未推进";
                break;
            }
            manifest_content_changed =
                manifest_content_changed || !manifest_delta.packs.empty();
            size_t total_needed = 0;
            for (const auto& item : manifest_delta.packs) {
                if (!item.deleted && wqn::WordPackNeedsDownload(item)) {
                    total_needed += item.byte_size;
                }
            }
            if (total_needed > 0) {
                wqn::StorageCapacitySnapshot storage;
                if (wqn::ReadStorageCapacitySnapshot(&storage) && storage.spiffs_valid) {
                    const size_t available =
                        storage.spiffs_total_bytes > storage.spiffs_used_bytes
                            ? storage.spiffs_total_bytes - storage.spiffs_used_bytes
                            : 0;
                    if (available < total_needed) {
                        ESP_LOGW(kTag, "SPIFFS space insufficient: need=%u avail=%u",
                                 static_cast<unsigned>(total_needed), static_cast<unsigned>(available));
                        result.result = ESP_ERR_NO_MEM;
                        result.message = "存储空间不足";
                    }
                }
            }

            for (const wqn::WqnWordPackManifestItem& item : manifest_delta.packs) {
                if (result.result != ESP_OK) {
                    break;
                }
                if (item.deleted || !wqn::WordPackNeedsDownload(item)) {
                    continue;
                }
                result.result = wqn::DownloadWordPackToStorage(
                    token,
                    wqn::services::MakeDeviceRequestMetadata(),
                    item);
                if (result.result != ESP_OK) {
                    break;
                }
            }
            if (result.result != ESP_OK) {
                break;
            }
            const bool manifest_page_changed =
                !manifest_delta.packs.empty() ||
                manifest_delta.cursor != local_manifest.cursor;
            if (manifest_page_changed) {
                wqn::WqnWordPackManifest merged;
                result.result = wqn::MergeWordPackManifestDelta(
                    manifest_delta, &merged);
                if (result.result == ESP_OK) {
                    result.result = wqn::SaveWordPackManifest(merged);
                }
                if (result.result == ESP_OK) {
                    local_manifest = std::move(merged);
                }
            }
            has_more = manifest_delta.has_more;
        }
        if (result.result == ESP_OK && has_more) {
            result.result = ESP_ERR_INVALID_SIZE;
            result.message = "词库变更过多，请重试";
        }
        if (result.result == ESP_OK &&
            (manifest_content_changed || !had_local_manifest)) {
            result.result = wqn::LoadWordPackIndex(&result.pack_index);
            result.message = result.pack_index.status_message;
            result.pack_index_ready = result.result == ESP_OK;
        } else if (result.result == ESP_OK) {
            result.message = "词库无变更";
            ESP_LOGI(kTag, "word pack manifest unchanged; index rebuild skipped");
        }
    } else if (request.op == WordCloudOp::kStartSession) {
        wqn::protocol::word_study_v1::CreateSessionRequest session;
        session.metadata = wqn::services::MakeDeviceRequestMetadata();
        session.metadata.request_id = request.request_id;
        session.mode = static_cast<wqn::protocol::word_study_v1::Mode>(
            request.study_mode);
        result.result = wqn::CreateWordStudySessionV1(
            token,
            session,
            &result.session,
            &result.protocol_error);
        if (result.result == ESP_OK) {
            // Compact + persist on the runner thread; the snapshot fsync used
            // to run inside the UI task's apply step. Inactive sessions are
            // never persisted (mirrors the old apply-side order).
            result.persisted_session.active = !result.session.items.empty();
            result.persisted_session.paused = false;
            result.persisted_session.position = 0;
            result.persisted_session.phase = wqn::WordPresentationPhase::kFront;
            result.session_compact_result = wqn::CompactWordSessionData(
                result.session, &result.persisted_session.remote);
            if (result.session_compact_result == ESP_OK &&
                result.persisted_session.active) {
                result.session_persist_result =
                    wqn::SavePersistedWordSession(result.persisted_session);
            }
        }
    } else if (request.op == WordCloudOp::kFetchSessionPage) {
        wqn::protocol::word_study_v1::CandidatePageRequest page;
        page.metadata = wqn::services::MakeDeviceRequestMetadata();
        page.metadata.request_id = request.request_id;
        page.cursor = request.cursor;
        page.limit = request.limit;
        result.result = wqn::FetchWordStudyCandidatePageV1(
            token,
            request.session_id,
            page,
            &result.candidate_page,
            &result.protocol_error);
    } else if (request.op == WordCloudOp::kSearch) {
        wqn::WqnWordSearchRequest search;
        search.query = request.query;
        search.limit = 8;
        result.result = wqn::SearchWords(token, search, &result.search);
    } else if (request.op == WordCloudOp::kAiLookup) {
        wqn::WqnWordAiLookupRequest lookup;
        lookup.query = request.query;
        result.result = wqn::LookupWordWithAi(token, lookup, &result.lookup);
    } else {
        result.result = ESP_ERR_INVALID_ARG;
    }

    if (result.result != ESP_OK) {
        std::string after_token;
        result.auth_required = !LoadValidTokenForTodo(&after_token);
    }
    SendWordCloudResult();
}

}  // namespace device_ui_internal
