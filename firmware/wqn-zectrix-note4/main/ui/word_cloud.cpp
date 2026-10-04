// Word review cloud task: pack sync, session start/paging, review submit.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "ui_runtime.h"
#include "persist_worker.h"
#include "word_app.h"

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
static std::atomic<bool> g_word_pack_cloud_busy{false};
wqn::runtime::SleepLease g_word_sleep_lease;
wqn::runtime::SleepLease g_word_pack_sleep_lease;
WordCloudResult g_word_result_slot;
uint32_t g_word_result_generation = 0;
WordCloudResult g_word_pack_result_slot;
uint32_t g_word_pack_result_generation = 0;

void FinishWordCloudRequest(CloudDomain domain)
{
    const bool bulk = domain == CloudDomain::kWordBulk;
    (bulk ? g_word_pack_sleep_lease : g_word_sleep_lease).Reset();
    ClearCloudDomainBusyWatch(domain);
    (bulk ? g_word_pack_cloud_busy : g_word_cloud_busy).store(
        false, std::memory_order_release);
}

RefreshSchedule PumpWordObservationCommit(UiRuntime* runtime)
{
    if (runtime == nullptr) {
        return RefreshSchedule::kNone;
    }
    // One word commit in flight at a time; wait for the UI to ack the last one
    // (the card stays in kPersisting until then, so no second effect is armed).
    if (IsPersistKindBusy(PersistKind::kWordObservation)) {
        return RefreshSchedule::kNone;
    }
    // Cheap readiness pre-check: reservation takes a SleepLease + pool slot, so
    // an idle pump must not reserve just to cancel. Reserve happens BEFORE the
    // effect is pulled from UI state (that mutates it), so we must first know
    // there is something to commit.
    if (!runtime->state().word_app.session.observation_effect_ready) {
        return RefreshSchedule::kNone;
    }
    // Phase 1: reserve busy + slot + storage lease. On failure the UI state is
    // untouched (effect still armed) -- just retry next pump.
    PersistTicket ticket = TryReservePersist(PersistKind::kWordObservation);
    if (!ticket.valid()) {
        return RefreshSchedule::kNone;
    }
    const auto metadata = wqn::services::MakeDeviceRequestMetadata();
    std::string occurred_at = CurrentIsoTimestamp();
    if (occurred_at.empty()) {
        // Durable even before SNTP; the server clamps implausible times.
        occurred_at = "2024-01-01T00:00:00Z";
    }
    wqn::DurableWordObservation observation;
    wqn::PersistedWordSession advanced_session;
    // Phase 2: only now pull the effect from UI state, binding this dispatch's
    // operation_id so a late result after a scope reset is rejected.
    if (!runtime->TakeWordObservationEffect(
            metadata.request_id, occurred_at, ticket.operation_id,
            &observation, &advanced_session)) {
        // Take mutated state to kFailed ("会话游标无效") and cleared the effect;
        // release the reservation and route the failure through a typed event
        // so the revision advances (this runs after the display commit, so the
        // caller folds the returned refresh into the next iteration's pending
        // schedule instead of leaving a stuck "正在保存").
        CancelPersistReservation(ticket);
        return runtime->DispatchWordObservationTakeFailed().refresh;
    }
    EnqueueReservedWordObservation(
        ticket, std::move(observation), std::move(advanced_session));
    return RefreshSchedule::kNone;
}

bool IsWordCloudBusy()
{
    return g_word_cloud_busy.load(std::memory_order_acquire);
}

bool IsWordPackCloudBusy()
{
    return g_word_pack_cloud_busy.load(std::memory_order_acquire);
}

bool QueueWordCloudRequest(const WordCloudRequest& request)
{
    const bool bulk = request.op == WordCloudOp::kPackSync;
    std::atomic<bool>& busy = bulk ? g_word_pack_cloud_busy : g_word_cloud_busy;
    bool expected = false;
    if (!busy.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    wqn::runtime::SleepLease lease =
        wqn::runtime::SleepLease::TryAcquire(
            wqn::runtime::SleepBlocker::kWordCloud, "word-cloud", __FILE__, __LINE__);
    if (!lease) {
        busy.store(false, std::memory_order_release);
        return false;
    }
    (bulk ? g_word_pack_sleep_lease : g_word_sleep_lease) = std::move(lease);
    CloudJob job;
    job.domain = bulk ? CloudDomain::kWordBulk : CloudDomain::kWord;
    job.word = request;
    if (!EnqueueCloudJob(job)) {
        FinishWordCloudRequest(job.domain);
        return false;
    }
    return true;
}

bool QueueWordReviewRefresh()
{
    const wqn::services::SyncContentTicket ticket =
        wqn::services::TryClaimContentRefresh(
            wqn::services::SyncContentDomain::kWordPacks);
    if (!ticket) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kPackSync;
    request.content_sync_generation = ticket.generation;
    request.content_target_revision = ticket.target_revision;
    if (!QueueWordCloudRequest(request)) {
        wqn::services::CancelContentRefreshClaim(ticket);
        return false;
    }
    return true;
}

bool QueueWordSessionStart(
    const wqn::protocol::word_study_v1::CreateSessionRequest& session)
{
    if (session.metadata.request_id.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kStartSession;
    // [deck-scope] Epoch at queue time: a deck switch completing while this
    // request is in flight must invalidate both the runner-side session save
    // and the apply of the result.
    request.scope_generation = wqn::GetDeckScopeGeneration();
    std::snprintf(
        request.request_id,
        sizeof(request.request_id),
        "%s",
        session.metadata.request_id.c_str());
    request.study_mode = static_cast<uint8_t>(session.mode);
    // Carry the -1 "unset" sentinel untouched; collapsing it to 0 here
    // made every non-sequential start send a literal start_index.
    request.start_index = session.start_index;
    request.new_word_limit = session.new_word_limit > 0
        ? static_cast<uint16_t>(session.new_word_limit)
        : 0;
    // [deck-scope] Carry the UI's deck choice across the queue boundary. The
    // runner rebuilds the request from this struct, so anything left here is
    // what actually reaches the server. Only a full 36-char UUID is meaningful.
    if (!session.scope.deck_ids.empty() &&
        session.scope.deck_ids.front().size() == 36) {
        std::snprintf(
            request.deck_id,
            sizeof(request.deck_id),
            "%s",
            session.scope.deck_ids.front().c_str());
    }
    return QueueWordCloudRequest(request);
}


// [ui-gates] Hand-off slot for the one candidate page in flight.
// `session.page_in_flight` guarantees only one page is outstanding at a time,
// and the result is applied before the next can be queued, so a single slot is
// enough -- and it keeps CloudJob POD (a PersistedWordSession is 168 bytes but
// not trivially copyable, and the job union needs POD). The handle is the
// validation: a job whose handle no longer matches must not consume a snapshot
// parked for a later page.
struct PageExtensionSlot {
    std::atomic<uint32_t> handle{0};
    wqn::PersistedWordSession snapshot;
};
PageExtensionSlot g_page_extension;

// Parks the snapshot for a queued page and returns its handle (never 0).
uint32_t ParkPageExtension(const wqn::PersistedWordSession& snapshot)
{
    uint32_t handle = g_page_extension.handle.load(std::memory_order_relaxed) + 1;
    if (handle == 0) {
        handle = 1;  // 0 means "empty"
    }
    g_page_extension.snapshot = snapshot;
    g_page_extension.handle.store(handle, std::memory_order_release);
    return handle;
}

// Takes the snapshot parked for `handle`, or returns false when it is gone
// (superseded, or the queue send failed and nothing was parked).
bool TakePageExtension(uint32_t handle, wqn::PersistedWordSession* snapshot)
{
    if (handle == 0 || snapshot == nullptr) {
        return false;
    }
    if (g_page_extension.handle.load(std::memory_order_acquire) != handle) {
        return false;
    }
    *snapshot = g_page_extension.snapshot;
    g_page_extension.handle.store(0, std::memory_order_release);
    return true;
}

bool QueueWordCandidatePage(
    const std::string& session_id,
    const wqn::protocol::word_study_v1::CandidatePageRequest& page,
    const wqn::PersistedWordSession& snapshot)
{
    if (session_id.size() != 36 || page.metadata.request_id.empty() ||
        page.cursor.empty() || page.limit < 1 ||
        page.limit > static_cast<int>(
            wqn::protocol::word_study_v1::kMaxCandidatePageItems)) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kFetchSessionPage;
    request.scope_generation = wqn::GetDeckScopeGeneration();
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
    request.page_extension_handle = ParkPageExtension(snapshot);
    return QueueWordCloudRequest(request);
}

void PumpWordCandidatePrefetch(UiRuntime* runtime)
{
    if (runtime == nullptr || IsWordCloudBusy()) return;
    wqn::protocol::word_study_v1::CandidatePageRequest request;
    request.metadata = wqn::services::MakeDeviceRequestMetadata();
    std::string session_id;
    wqn::PersistedWordSession snapshot;
    if (!runtime->TakeWordCandidatePageRequest(&request, &snapshot, &session_id)) {
        return;
    }
    if (!QueueWordCandidatePage(session_id, request, snapshot)) {
        runtime->RestoreWordCandidatePageRequest();
    }
}

WordCloudResult* PeekWordCloudResult(CloudDomain domain, uint32_t generation)
{
    const bool bulk = domain == CloudDomain::kWordBulk;
    const uint32_t current = bulk ? g_word_pack_result_generation
                                  : g_word_result_generation;
    if (generation == 0 || generation != current) {
        return nullptr;
    }
    return bulk ? &g_word_pack_result_slot : &g_word_result_slot;
}

void SendWordCloudResult(CloudDomain domain)
{
    const uint32_t generation = domain == CloudDomain::kWordBulk
        ? g_word_pack_result_generation
        : g_word_result_generation;
    CloudResultReady ready;
    ready.domain = domain;
    ready.generation = generation;
    PublishCloudResult(domain, generation);
    (void)ready;
}

bool IsWordSessionInvalidError(const wqn::protocol::v3::Error& error)
{
    // Codes that mean the pinned session is unusable server-side. These are not
    // transient, so the device must drop the session instead of retrying it.
    return error.code == "SESSION_NOT_FOUND" ||
           error.code == "SESSION_NOT_ACTIVE" ||
           error.code == "WORD_SESSION_SNAPSHOT_INCOMPLETE";
}

// Rebuilds the note screen's [词] rows from the mounted deck catalog. The
// current default deck is excluded: it lives on the word page itself, the
// mixed list only carries the extra decks.
void RebuildNoteWordDeckRows(wqn::UiState* state)
{
    if (state == nullptr) {
        return;
    }
    std::vector<wqn::NoteWordDeckRow> rows;
    rows.reserve(state->word_app.deck_catalog.size());
    for (const wqn::WordDeckInfo& deck : state->word_app.deck_catalog) {
        if (!state->word_app.default_deck_id.empty() &&
            deck.deck_id == state->word_app.default_deck_id) {
            continue;
        }
        wqn::NoteWordDeckRow row;
        row.deck_id = deck.deck_id;
        row.title = deck.title;
        row.entry_count = deck.entry_count;
        rows.push_back(std::move(row));
    }
    wqn::ApplyNoteWordDeckRows(&state->note_app, std::move(rows));
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
            // Content changed: refresh the deck catalog (manifest titles +
            // counts) and the note screen's [词] rows that mirror it.
            std::vector<wqn::WordDeckInfo> catalog;
            if (wqn::BuildWordDeckCatalog(&catalog) == ESP_OK) {
                wqn::InstallWordDeckCatalog(&state->word_app, std::move(catalog));
            }
            RebuildNoteWordDeckRows(state);
            // Keep the settings row value in step (a cloud-deleted default
            // falls back to 全部词库 inside InstallWordDeckCatalog).
            state->settings.default_word_deck_title =
                state->word_app.default_deck_title;
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
        // [deck-scope] A default-deck switch committed while this request was
        // in flight: the runner-side save was already rejected by the store;
        // drop the result too so a stale session never installs over the
        // fresh scope. The user re-enters the word page to start a new round.
        if (result.scope_generation != wqn::GetDeckScopeGeneration()) {
            ESP_LOGW(kTag, "word session start dropped: scope epoch %lu != %lu",
                     static_cast<unsigned long>(result.scope_generation),
                     static_cast<unsigned long>(wqn::GetDeckScopeGeneration()));
            wqn::CancelWordSessionStartResult(&state->word_app);
            state->word_app.mode = wqn::WordAppMode::kHome;
            state->word_app.message = "词库已切换，请重新进入";
            return state->screen == wqn::UiScreen::kWord;
        }
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
        // [deck-scope] Same stale-epoch drop as kStartSession: route it through
        // the existing failure path so page_in_flight resets without touching
        // the (already reset) session.
        if (result.scope_generation != wqn::GetDeckScopeGeneration()) {
            ESP_LOGW(kTag, "word candidate page dropped: scope epoch %lu != %lu",
                     static_cast<unsigned long>(result.scope_generation),
                     static_cast<unsigned long>(wqn::GetDeckScopeGeneration()));
            wqn::ApplyWordCandidatePageResult(
                &state->word_app,
                ESP_ERR_INVALID_STATE,
                ESP_OK,
                ESP_OK,
                state->word_app.session.persisted,
                {});
            return false;
        }
        if (result.result != ESP_OK && IsWordSessionInvalidError(result.protocol_error)) {
            wqn::ResetWordSessionForServerInvalid(&state->word_app);
        } else {
            wqn::ApplyWordCandidatePageResult(
                &state->word_app,
                result.result,
                result.session_compact_result,
                result.session_persist_result,
                result.persisted_session,
                std::move(result.candidate_page));
        }
        BuildHomeSummary(state);
        return true;
    }
    return false;
}

void ExecuteWordCloudRequest(const WordCloudRequest& request)
{
    const CloudDomain result_domain = request.op == WordCloudOp::kPackSync
        ? CloudDomain::kWordBulk
        : CloudDomain::kWord;
    const wqn::services::SyncContentTicket content_ticket = {
        wqn::services::SyncContentDomain::kWordPacks,
        request.content_sync_generation,
        request.content_target_revision,
    };
    WordCloudResult& result_slot = result_domain == CloudDomain::kWordBulk
        ? g_word_pack_result_slot
        : g_word_result_slot;
    uint32_t& result_generation = result_domain == CloudDomain::kWordBulk
        ? g_word_pack_result_generation
        : g_word_result_generation;
    result_slot = WordCloudResult{};
    ++result_generation;
    if (result_generation == 0) {
        ++result_generation;
    }
    WordCloudResult& result = result_slot;
    result.op = request.op;
    result.scope_generation = request.scope_generation;
    result.message.clear();

    std::string token;
    if (!LoadValidTokenForTodo(&token)) {
        result.auth_required = true;
        result.result = ESP_ERR_INVALID_STATE;
        if (request.op == WordCloudOp::kPackSync) {
            wqn::services::CompleteContentRefresh(
                content_ticket, result.result, nullptr, "auth-required");
        }
        SendWordCloudResult(result_domain);
        return;
    }

    if (request.op == WordCloudOp::kPackSync) {
        result.result = wqn::services::BeginContentInstall(content_ticket);
        if (result.result != ESP_OK) {
            result.message = "词库安装标记失败";
        }
        wqn::WqnWordPackManifest local_manifest;
        bool had_local_manifest = true;
        bool manifest_content_changed = false;
        if (result.result == ESP_OK) {
            result.result = wqn::LoadWordPackManifest(&local_manifest);
        }
        if (result.result == ESP_ERR_NOT_FOUND) {
            had_local_manifest = false;
            result.result = wqn::ResetWordPackStorageCache();
            if (result.result == ESP_OK) {
                local_manifest = {};
            }
        } else if (result.result != ESP_OK && result.message.empty()) {
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
                result.result = wqn::EnsurePackDownloadCapacity(total_needed);
                if (result.result == ESP_ERR_NO_MEM) {
                    result.message = "存储空间不足，已保留现有词库";
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
        session.start_index = request.start_index;
        session.new_word_limit = static_cast<int>(request.new_word_limit);
        // [deck-scope] The runner rebuilds the request, so the queued deck id
        // has to be re-attached here or the server sees an empty scope and
        // silently substitutes the first 32 visible decks.
        if (std::strlen(request.deck_id) == 36) {
            session.scope.deck_ids.push_back(request.deck_id);
        }
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
            // Stored as uint32_t; keep the -1 sentinel out of it.
            result.persisted_session.start_index = request.start_index > 0
                ? static_cast<uint32_t>(request.start_index)
                : 0;
            result.persisted_session.phase = wqn::WordPresentationPhase::kFront;
            // [deck-scope] Pin the session to the epoch it was REQUESTED under;
            // the store rejects the save below if a deck switch landed since.
            result.persisted_session.deck_scope_generation =
                request.scope_generation;
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
        // [ui-gates] Take the parked snapshot FIRST, before the network call,
        // so the slot is free for the next page and a superseded job cannot
        // consume it.
        wqn::PersistedWordSession parked;
        const bool have_parked = TakePageExtension(
            request.page_extension_handle, &parked);
        result.result = wqn::FetchWordStudyCandidatePageV1(
            token,
            request.session_id,
            page,
            &result.candidate_page,
            &result.protocol_error);
        if (result.result == ESP_OK && have_parked) {
            // Extend and persist the snapshot this page was queued with, on this
            // thread. The apply step used to run the multi-second snapshot fsync
            // on the UI task. When the snapshot is gone the UI merges the page in
            // memory instead and the next observation commit persists it.
            result.persisted_session = parked;
            result.session_compact_result = wqn::ExtendPersistedWordSessionWithPage(
                parked,
                result.candidate_page,
                &result.persisted_session);
            if (result.session_compact_result == ESP_OK &&
                result.persisted_session.active) {
                result.session_persist_result =
                    wqn::SavePersistedWordSession(result.persisted_session);
            }
        }
    } else {
        result.result = ESP_ERR_INVALID_ARG;
    }

    if (result.result != ESP_OK) {
        std::string after_token;
        result.auth_required = !LoadValidTokenForTodo(&after_token);
    }
    if (request.op == WordCloudOp::kPackSync) {
        wqn::services::CompleteContentRefresh(
            content_ticket, result.result, nullptr,
            result.result == ESP_OK ? nullptr : result.message.c_str());
    }
    SendWordCloudResult(result_domain);
}

}  // namespace device_ui_internal
