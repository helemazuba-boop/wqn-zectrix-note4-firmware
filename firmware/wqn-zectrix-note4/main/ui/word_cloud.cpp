// Word review cloud task: pack sync, session start/paging, review submit.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "ui_runtime.h"
#include "persist_worker.h"
#include "word_app.h"
#include "runtime/wall_clock.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <random>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"
#include "runtime/sleep_coordinator.h"
#include "services/sync_service.h"
#include "services/connectivity_service.h"
#include "storage.h"
#include "word_local_session.h"
#include "word_pack.h"
#include "wqn_api.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

static std::atomic<bool> g_word_cloud_busy{false};
static std::atomic<bool> g_word_pack_cloud_busy{false};
wqn::runtime::SleepLease g_word_sleep_lease;
wqn::runtime::SleepLease g_word_pack_sleep_lease;
wqn::runtime::SleepLease g_word_buffer_sleep_lease;
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
    RefreshSchedule refresh = RefreshSchedule::kNone;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (runtime->state().word_app.session.observation_effect_ready) {
        const auto& pending = runtime->state().word_app.session.pending_observation;
        // A previously assigned identity/time is an existing fact, not new learning.
        const std::string occurred_at = pending.request_id.empty()
            ? CurrentIsoTimestamp() : pending.occurred_at;
        if (occurred_at.empty()) {
            refresh = runtime->DispatchLearningTimeRequired().refresh;
        } else {
            // Accepted RAM facts still flush below while new facts are held.
            // Acquire the buffer's lease BEFORE acceptance. The worker lease
            // covers only its flush and cannot protect an unpersisted RAM tail
            // from normal sleep or power-off.
            if (!g_word_buffer_sleep_lease) {
                g_word_buffer_sleep_lease = wqn::runtime::SleepLease::TryAcquire(
                    wqn::runtime::SleepBlocker::kStorage, "word-RAM-buffer", __FILE__, __LINE__);
                if (!g_word_buffer_sleep_lease) return refresh;
            }
            const std::string request_id = pending.request_id.empty()
                ? wqn::services::MakeDeviceRequestMetadata().request_id : pending.request_id;
            refresh = runtime->DispatchWordObservationBuffered(
                request_id, occurred_at, now_ms).refresh;
        }
    }
    const auto& session = runtime->state().word_app.session;
    if (session.buffered_observations.empty()) {
        g_word_buffer_sleep_lease.Reset();
        return refresh;
    }
    if (!wqn::WordBatchPolicy::FlushDue(session.buffered_observations.size(), session.batch_in_flight,
            session.buffered_observations.front().accepted_ms, now_ms, session.batch_retry_after_ms,
            session.batch_flush_requested) || IsPersistKindBusy(PersistKind::kWordObservation)) return refresh;
    // Reserve the slot before moving the owned flush payload. RAM acceptance
    // is already explicit, so a failed reservation retains every identity.
    PersistTicket ticket = TryReservePersist(PersistKind::kWordObservation);
    if (!ticket.valid()) return refresh;
    std::vector<wqn::DurableWordObservation> observations;
    wqn::PersistedWordSession advanced_session;
    if (!runtime->TakeWordObservationBatch(ticket.operation_id, now_ms, &observations, &advanced_session)) {
        CancelPersistReservation(ticket);
        return refresh;
    }
    EnqueueReservedWordObservations(ticket, std::move(observations), std::move(advanced_session));
    return refresh;
}

RefreshSchedule PumpWordSessionCursor(UiRuntime* runtime)
{
    if (runtime == nullptr) return RefreshSchedule::kNone;
    auto& word = runtime->state().word_app;
    if (!word.session.cursor_intent_armed ||
        word.session.cursor_intent_operation_id != 0 ||
        word.session.cursor_awaiting_retry_confirm || IsWordCloudBusy() ||
        wqn::HasBufferedWordObservations(word) ||
        word.session.batch_in_flight != 0 ||
        word.session.commit_state ==
            wqn::WordObservationCommitState::kPersisting ||
        wqn::DefaultDeckRepairBlocking(word) ||
        word.scope_reset_pending_valid ||
        IsPersistDomainBusy(PersistDomain::kWord) ||
        IsPersistKindBusy(PersistKind::kSettingsDefaultDeck)) {
        if (word.session.cursor_intent_armed &&
            wqn::HasBufferedWordObservations(word)) {
            runtime->RequestWordBatchFlush();
        }
        return RefreshSchedule::kNone;
    }
    PersistTicket ticket =
        TryReservePersist(PersistKind::kWordSessionCursor);
    if (!ticket.valid()) return RefreshSchedule::kNone;
    wqn::PersistedWordSession session;
    if (!runtime->TakeWordSessionCursorEffect(
            ticket.operation_id, &session)) {
        CancelPersistReservation(ticket);
        return RefreshSchedule::kNone;
    }
    EnqueueReservedWordSessionCursor(
        ticket, std::move(session));
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

std::string MakeStableLocalUuid(const std::string& request_id)
{
    uint64_t value = 1469598103934665603ULL;
    for (char c : request_id) {
        value ^= static_cast<unsigned char>(c);
        value *= 1099511628211ULL;
    }
    char out[37] = {};
    std::snprintf(out, sizeof(out),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>(value & 0xFFFFFFFFU),
        static_cast<unsigned>((value >> 32) & 0xFFFFU),
                static_cast<unsigned>(((value >> 48) & 0x0FFFU) | 0x4000U),
                static_cast<unsigned>(((value >> 20) & 0x3FFFU) | 0x8000U),
        static_cast<unsigned long long>(value ^ 0x9E3779B97F4A7C15ULL) & 0xFFFFFFFFFFFFULL);
    return out;
}

bool QueueWordSessionStart(
    const wqn::protocol::word_study_v1::CreateSessionRequest& session,
    bool resume_existing)
{
    if (session.metadata.request_id.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kStartSession;
    request.resume_existing = resume_existing;
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
    const std::string local_id = !session.seed.empty() &&
            wqn::IsValidLocalSessionUuid(session.seed)
        ? session.seed
        : MakeStableLocalUuid(session.metadata.request_id);
    std::snprintf(request.session_id, sizeof(request.session_id), "%s",
        local_id.c_str());
    std::snprintf(request.seed, sizeof(request.seed), "%s",
        session.seed.empty() ? "local-sequential-v1" : session.seed.c_str());
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
    request.study_mode = static_cast<uint8_t>(snapshot.remote.mode);
    request.local_walk = snapshot.remote.mode == wqn::protocol::word_study_v1::Mode::kSequential ||
        (snapshot.remote.mode == wqn::protocol::word_study_v1::Mode::kShuffle && wqn::IsValidLocalSessionUuid(snapshot.remote.seed));
    request.page_extension_handle = ParkPageExtension(snapshot);
    return QueueWordCloudRequest(request);
}

void PumpWordSessionStart(UiRuntime* runtime)
{
    if (runtime == nullptr || IsWordCloudBusy()) return;
    auto& pending = runtime->state().word_app;
    if (pending.session.session_clear_armed) {
        if (wqn::HasBufferedWordObservations(pending) ||
            IsPersistDomainBusy(PersistDomain::kWord) ||
            wqn::DefaultDeckRepairBlocking(pending)) return;
        WordCloudRequest clear;
        clear.op = WordCloudOp::kClearSession;
        clear.scope_generation = wqn::GetDeckScopeGeneration();
        clear.study_mode = static_cast<uint8_t>(pending.session.session_clear_mode);
        std::snprintf(clear.session_id, sizeof(clear.session_id), "%s",
                      pending.session.session_clear_id.c_str());
        QueueWordCloudRequest(clear);
        return;
    }
    if (!wqn::runtime::IsLearningTimeReady()) return;
    if (runtime->state().screen != wqn::UiScreen::kWord ||
        (!pending.session.start_requested && !pending.chain.segment_finished && !pending.shuffle_chain.segment_finished)) return;
    if (!pending.chain.segment_finished && !pending.shuffle_chain.segment_finished &&
        pending.mode != wqn::WordAppMode::kSessionStarting) return;
    if (pending.session.cursor_intent_armed || IsPersistDomainBusy(PersistDomain::kWord)) return;
    // [deck-autorepair] Defer a new session while a repair is pending/in
    // flight: the scope is about to change, so a session started now would
    // cross into the reset scope (generation defense, gap 3).
    if (wqn::DefaultDeckRepairBlocking(runtime->state().word_app)) return;
    const auto& word = runtime->state().word_app;
    if (wqn::HasBufferedWordObservations(word) || word.session.observation_effect_ready ||
        word.session.commit_state == wqn::WordObservationCommitState::kPersisting ||
        IsPersistKindBusy(PersistKind::kWordObservation)) {
        // The old SID must remain installed until its entire RAM tail is
        // durable. kPersisting also covers the Prepare-to-reserve gap.
        runtime->RequestWordBatchFlush();
        return;
    }
    wqn::protocol::word_study_v1::CreateSessionRequest request;
    request.metadata = wqn::services::MakeDeviceRequestMetadata();
    if (!runtime->TakeWordSessionStartRequest(&request)) return;
    if (!QueueWordSessionStart(request, word.session.session_load_armed)) {
        runtime->RestoreWordSessionStartRequest();
        return;
    }
    ESP_LOGI(kTag, "word session start queued: mode=%u start_index=%d new_word_limit=%d request=%s",
        static_cast<unsigned>(request.mode), request.start_index, request.new_word_limit,
        request.metadata.request_id.c_str());
}

void PumpWordCardPrefetch(UiRuntime* runtime)
{
    if (runtime == nullptr || runtime->state().screen != wqn::UiScreen::kWord ||
        IsWordCloudBusy() || IsWordPackCloudBusy() ||
        IsPersistKindBusy(PersistKind::kWordObservation)) return;
    const auto& session = runtime->state().word_app.session;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    // Do not start speculative I/O ahead of an already-due/forced flush. Once
    // a SPIFFS read starts it is non-preemptible, even on the background lane.
    if (session.observation_effect_ready || session.batch_flush_requested ||
        (!session.buffered_observations.empty() && wqn::WordBatchPolicy::FlushDue(
            session.buffered_observations.size(), session.batch_in_flight,
            session.buffered_observations.front().accepted_ms, now_ms,
            session.batch_retry_after_ms, false))) return;
    wqn::WordPackIndexEntry index{};
    if (!wqn::GetWordCardPrefetchEntry(runtime->state().word_app, now_ms, &index)) return;
    const PersistTicket ticket = TryReservePersist(PersistKind::kWordCardPrefetch);
    if (!ticket.valid()) return;
    if (!runtime->TakeWordCardPrefetchEntry(ticket.operation_id, now_ms, &index)) {
        CancelPersistReservation(ticket);
        return;
    }
    EnqueueReservedWordCardPrefetch(ticket, index);
}

void PumpWordCandidatePrefetch(UiRuntime* runtime)
{
    if (runtime == nullptr || IsWordCloudBusy()) return;
    // [deck-autorepair] Defer a candidate page while a repair is pending/in
    // flight (scope about to change, gap 3).
    if (wqn::DefaultDeckRepairBlocking(runtime->state().word_app)) return;
    if (wqn::HasBufferedWordObservations(runtime->state().word_app) ||
        runtime->state().word_app.session.observation_effect_ready) {
        // A page extension writes a full snapshot. Never publish RAM-ahead
        // progress or prune the candidate window before the buffer is durable.
        if (runtime->state().word_app.session.page_requested) runtime->RequestWordBatchFlush();
        return;
    }
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

// [snapshot-coalesce] Neither the initial window nor its immediate prefetch
// has been exposed to UI yet. Build at most one extended window in RAM, then
// keep the existing durable-before-publish boundary with ONE snapshot save.
void PersistInitialWordSessionSnapshot(const std::string& token, WordCloudResult* result)
{
    if (result == nullptr || result->result != ESP_OK ||
        result->session_compact_result != ESP_OK || !result->persisted_session.active) return;
    auto& initial = result->persisted_session;
    const size_t base_items = initial.remote.items.size();
    const bool prefetch = !initial.paused && initial.position == 0 &&
        initial.phase == wqn::WordPresentationPhase::kFront && base_items > 0 &&
        base_items <= wqn::protocol::word_study_v1::kInitialCandidatePageSize &&
        initial.remote.has_more && !initial.remote.cursor.empty();
    bool coalesced = false;
    esp_err_t prefetch_result = ESP_OK;
    int64_t prefetch_ms = 0;
    if (prefetch) {
        wqn::protocol::word_study_v1::CandidatePageRequest request;
        request.metadata = wqn::services::MakeDeviceRequestMetadata();
        request.cursor = initial.remote.cursor;
        request.limit = static_cast<int>(wqn::protocol::word_study_v1::kCandidatePrefetchPageSize);
        wqn::protocol::word_study_v1::CandidatePageData page;
        wqn::protocol::v3::Error error;
        const int64_t started_us = esp_timer_get_time();
        prefetch_result = wqn::TryFetchWordStudyCandidatePageV1(
            token, initial.remote.session_id, request, &page, &error);
        if (prefetch_result == ESP_OK) {
            wqn::PersistedWordSession extended;
            prefetch_result = wqn::ExtendPersistedWordSessionWithPage(initial, page, &extended);
            if (prefetch_result == ESP_OK) {
                initial = std::move(extended);
                coalesced = true;
            }
        } else {
            // Transient or readiness failure falls back to the valid initial
            // window. Do not revive a server-invalid session or ignore the
            // existing API's 401 token clearing just to make prefetch succeed.
            std::string current_token;
            if (IsWordSessionInvalidError(error) || !LoadValidTokenForTodo(&current_token)) {
                result->result = prefetch_result;
                result->protocol_error = std::move(error);
                ESP_LOGW(kTag, "word initial snapshot rejected: prefetch_result=%s session=%s",
                    esp_err_to_name(prefetch_result), initial.remote.session_id.c_str());
                return;
            }
        }
        prefetch_ms = (esp_timer_get_time() - started_us) / 1000;
    }
    // The existing store still merges concurrent durable progress and rejects
    // stale scope before writing. No intermediate window was saved/published.
    result->session_persist_result = wqn::SavePersistedWordSession(initial);
    ESP_LOGI(kTag,
        "word initial snapshot: base_items=%u items=%u coalesced=%d prefetch_attempted=%d "
        "prefetch_result=%s prefetch_ms=%lld result=%s session=%s",
        static_cast<unsigned>(base_items), static_cast<unsigned>(initial.remote.items.size()),
        coalesced ? 1 : 0, prefetch ? 1 : 0, esp_err_to_name(prefetch_result),
        static_cast<long long>(prefetch_ms), esp_err_to_name(result->session_persist_result),
        initial.remote.session_id.c_str());
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
                // word-result revision or a full EPD refresh. The runner STILL
                // delivered an authoritative catalog for an unchanged (or
                // authoritative-empty) manifest; install it in memory so a
                // deleted default deck is detected here too, not only on a
                // content-changing sync (gap 1). No BuildWordDeckCatalog here.
                state->word_app.cloud_sync_failed = false;
                state->word_app.cloud_loaded_once = true;
                state->word_app.cloud_sync_requested = false;
                if (result.catalog_authoritative) {
                    wqn::InstallWordDeckCatalog(
                        &state->word_app, std::move(result.catalog),
                        /*authoritative=*/true);
                    wqn::ArmDefaultDeckRepair(
                        &state->word_app, wqn::GetDeckScopeGeneration());
                    RebuildNoteWordDeckRows(state);
                    state->settings.default_word_deck_id =
                        state->word_app.default_deck_id;
                    state->settings.default_word_deck_title =
                        state->word_app.default_deck_title;
                }
                return false;
            }
            wqn::ApplyWordPackIndex(
                &state->word_app,
                std::move(result.pack_index),
                result.message);
            // [deck-autorepair] Install the RUNNER-built catalog in memory.
            // NEVER call BuildWordDeckCatalog here: that would read SPIFFS on
            // the UI task (gap 4). Arm the INDEPENDENT auto-repair intent; the
            // actual worker submit happens in the per-tick pump with full gates
            // + bounded backoff, attributed back by operation_id (gap 2).
            if (result.catalog_authoritative) {
                wqn::InstallWordDeckCatalog(
                    &state->word_app, std::move(result.catalog),
                    /*authoritative=*/true);
                wqn::ArmDefaultDeckRepair(
                    &state->word_app, wqn::GetDeckScopeGeneration());
            }
            RebuildNoteWordDeckRows(state);
            // Keep the settings row value in step. InstallWordDeckCatalog does
            // not clear the in-memory default: a deleted deck is repaired
            // durably by the worker, and memory changes only on its ACK.
            state->settings.default_word_deck_id = state->word_app.default_deck_id;
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

    if (result.op == WordCloudOp::kClearSession) {
        auto& session = state->word_app.session;
        if (session.session_clear_armed && session.session_clear_id == result.control_session_id) {
            session.session_clear_armed = false;
            session.session_clear_id.clear();
            state->word_app.message = result.result == ESP_OK
                ? "旧会话已关闭，可重新开始" : "旧会话关闭失败，请同步后重试";
        }
        return state->screen == wqn::UiScreen::kWord;
    }
    if (result.op == WordCloudOp::kStartSession) {
        // [deck-scope] A default-deck switch committed while this request was
        // in flight: the runner-side save was already rejected by the store;
        // drop the result too so a stale session never installs over the
        // fresh scope. [deck-autorepair] Also drop while a repair is pending/
        // in flight: the scope is about to change even if the generation has
        // not been bumped yet, so installing now would cross scopes (gap 3).
        if (result.scope_generation != wqn::GetDeckScopeGeneration() ||
            wqn::DefaultDeckRepairBlocking(state->word_app)) {
            ESP_LOGW(kTag, "word session start dropped: scope epoch %lu != %lu",
                     static_cast<unsigned long>(result.scope_generation),
                     static_cast<unsigned long>(wqn::GetDeckScopeGeneration()));
            wqn::CancelWordSessionStartResult(&state->word_app);
            state->word_app.mode = wqn::WordAppMode::kHome;
            state->word_app.message = "词库已切换，请重新进入";
            return state->screen == wqn::UiScreen::kWord;
        }
        if ((state->screen != wqn::UiScreen::kWord ||
             state->word_app.session.cancel_start_requested) &&
            result.result == ESP_OK && result.persisted_session.active) {
            auto& session = state->word_app.session;
            session.persisted = result.persisted_session;
            session.session_load_armed = false;
            session.cancel_start_requested = false;
            session.cursor_intent_armed = true;
            session.cursor_intent_paused = true;
            session.cursor_intent_session = result.persisted_session;
            session.cursor_intent_session.paused = true;
            session.cursor_intent_operation_id = 0;
            wqn::CancelWordSessionStartResult(&state->word_app);
            state->word_app.message = "正在保存暂停";
            return state->screen == wqn::UiScreen::kWord;
        }
        if (state->screen != wqn::UiScreen::kWord) {
            wqn::CancelWordSessionStartResult(&state->word_app);
            state->word_app.mode = wqn::WordAppMode::kHome;
            state->word_app.message = "已取消本轮准备";
            return false;
        }
        if (result.local_walk && result.local_round_complete) {
            wqn::CancelWordSessionStartResult(&state->word_app);
            state->word_app.mode = wqn::WordAppMode::kHome;
            auto& complete_chain=static_cast<wqn::protocol::word_study_v1::Mode>(state->word_app.session.requested_mode)==wqn::protocol::word_study_v1::Mode::kShuffle
                ? state->word_app.shuffle_chain : state->word_app.chain;
            complete_chain.sequential_cursor = 0;
            complete_chain.segment_finished = false;
            complete_chain.next_segment_armed = false;
            state->word_app.message = state->word_app.session.requested_mode==wqn::protocol::word_study_v1::Mode::kShuffle
                ? "当前没有可随机学习的单词" : "本轮完成，下次从第一词开始";
            return state->screen == wqn::UiScreen::kWord;
        }
        if (result.result == ESP_OK && result.pack_index_ready) {
            wqn::InstallPreparedWordPackIndex(
                &state->word_app, std::move(result.pack_index));
        }
        const bool offer_local_walk = result.review_queue_unavailable &&
            !state->word_app.session.cancel_start_requested;
        const bool applied = wqn::ApplyWordSessionStartResult(
            &state->word_app,
            result.result,
            result.session_compact_result,
            result.session_persist_result,
            std::move(result.persisted_session));
        if (!applied) return false;
        if (offer_local_walk) {
            state->word_app.review={};state->word_app.review.queue_unavailable=true;
            state->word_app.mode=wqn::WordAppMode::kReviewComplete;
        }
        if (result.result != ESP_OK) {
            state->word_app.message = result.auth_required ? "请重新配对"
                : (!result.message.empty() ? result.message : "本轮准备失败，可重试");
        } else if (!result.message.empty() && result.pack_index_ready &&
                   result.message == "词库已更新，已从第一词开始") {
            state->word_app.message = result.message;
        }
        if (result.local_walk && result.result == ESP_OK) {
            auto& chain = state->word_app.session.persisted.remote.mode==wqn::protocol::word_study_v1::Mode::kShuffle
                ? state->word_app.shuffle_chain : state->word_app.chain;
            chain.active = true;
            chain.sequential_total = result.local_round_total;
            chain.pinned_session_id =
                state->word_app.session.persisted.remote.session_id;
            chain.pinned_deck_id = result.local_scope_deck_id;
            chain.pinned_scope_generation = result.scope_generation;
            (void)result.local_segment_start;
            chain.segment_finished = false;
            chain.next_segment_armed = false;
            uint64_t absolute = 0;
            if (wqn::WordSequenceAbsoluteCursor(
                    state->word_app.session.persisted,
                    &absolute) == ESP_OK && absolute <= UINT32_MAX) {
                chain.sequential_cursor =
                    static_cast<uint32_t>(absolute);
            }
        }
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == WordCloudOp::kFetchSessionPage) {
        // [deck-scope] Same stale-epoch drop as kStartSession: route it through
        // the existing failure path so page_in_flight resets without touching
        // the (already reset) session. [deck-autorepair] Also drop while a
        // repair is pending/in flight (scope about to change, gap 3).
        if (result.scope_generation != wqn::GetDeckScopeGeneration() ||
            wqn::DefaultDeckRepairBlocking(state->word_app)) {
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

namespace {

bool IsLocalWordStudyOp(const WordCloudRequest& request)
{
    using wqn::protocol::word_study_v1::Mode;
    const auto mode = static_cast<Mode>(request.study_mode);
    return (mode == Mode::kSequential || mode == Mode::kShuffle) &&
           (request.op == WordCloudOp::kStartSession ||
            (request.op == WordCloudOp::kFetchSessionPage &&
             (mode == Mode::kSequential || request.local_walk)));
}
bool CanOfferLocalWalkAfterReviewFailure(const WordCloudRequest& request,
                                       const WordCloudResult& result)
{
    // A connected WiFi link does not prove cloud reachability. Keep the old
    // completion-page action available after transport/retryable failures,
    // without turning an authentication or semantic rejection into success.
    return request.op == WordCloudOp::kStartSession &&
           request.study_mode == static_cast<uint8_t>(wqn::protocol::word_study_v1::Mode::kReview) &&
           result.result != ESP_OK && !result.auth_required &&
           result.protocol_error.code != "UNAUTHORIZED" &&
           (result.protocol_error.code.empty() || result.protocol_error.retryable);
}

esp_err_t RefreshLocalWordEligibility(const wqn::WordPackIndex& index, const std::string& scope,
                                      uint32_t generation, const std::string& token,
                                      wqn::LocalWordEligibility* out)
{
    wqn::protocol::word_study_v1::EligibilityRequest request;
    request.metadata = wqn::services::MakeDeviceRequestMetadata();
    wqn::LocalWordEligibility saved;
    saved.scope_deck_id = scope;
    saved.deck_scope_generation = generation;
    saved.actor_binding = wqn::WordLocalSha256(token);
    for (const auto& pin : index.pack_identities)
    {
        if (!scope.empty() && scope != pin.deck_id)
            continue;
        wqn::protocol::word_study_v1::PackSnapshot wire;
        wire.deck_id = pin.deck_id;
        wire.sha256 = pin.sha256;
        wire.pack_revision = pin.pack_revision;
        wire.content_revision = pin.content_revision;
        request.snapshot.push_back(wire);
        wqn::StoredWordPackSnapshot item;
        std::snprintf(item.deck_id, sizeof(item.deck_id), "%s", pin.deck_id);
        std::snprintf(item.sha256, sizeof(item.sha256), "%s", pin.sha256);
        item.pack_revision = pin.pack_revision;
        item.content_revision = pin.content_revision;
        saved.snapshot.push_back(item);
    }
    wqn::protocol::word_study_v1::EligibilityData data;
    wqn::protocol::v3::Error error;
    int status = 0;
    auto result = wqn::FetchWordEligibilityV1(token, request, &data, &error, &status);
    if (status == 404 || status == 501)
        return ESP_ERR_NOT_SUPPORTED;
    if (result != ESP_OK)
        return result;
    saved.progress_revision = data.progress_revision;
    saved.item_ids_sha256 = std::move(data.item_ids_sha256);
    saved.mastered_bitmap = std::move(data.mastered_bitmap);
    if (!wqn::LocalWordEligibilityMatches(index, saved, scope))
        return ESP_ERR_INVALID_RESPONSE;
    result = wqn::SaveLocalWordEligibility(saved);
    if (result == ESP_OK && out != nullptr)
        *out = std::move(saved);
    return result;
}

void ExecuteLocalWordStart(const WordCloudRequest& request, WordCloudResult* result)
{
    result->op = WordCloudOp::kStartSession;
    result->local_walk = true;
    const std::string scope = request.deck_id;
    result->local_scope_deck_id = scope;
    wqn::PersistedWordSession old;
    const auto mode = static_cast<wqn::protocol::word_study_v1::Mode>(request.study_mode);
    const bool shuffle = mode == wqn::protocol::word_study_v1::Mode::kShuffle;
    const esp_err_t loaded = wqn::LoadPersistedWordSession(mode, &old);
    if (loaded != ESP_OK && loaded != ESP_ERR_NOT_FOUND)
    {
        result->result = loaded;
        return;
    }
    wqn::LocalWordSessionDescriptor prior;
    bool have_prior = false;
    if (loaded == ESP_OK)
    {
        const esp_err_t found = wqn::LoadLocalWordSessionDescriptor(old.remote.session_id, &prior);
        if (found != ESP_OK && found != ESP_ERR_NOT_FOUND)
        {
            result->result = found;
            return;
        }
        have_prior = found == ESP_OK;
    }
    uint64_t absolute = 0;
    if (have_prior &&
        (prior.session.deck_scope_generation != request.scope_generation ||
         wqn::WordSequenceAbsoluteCursor(old, &absolute) != ESP_OK || absolute > prior.total_count))
    {
        result->result = ESP_ERR_INVALID_STATE;
        return;
    }
    // A completed round is retained for recovery. A new home entry at zero
    // starts a new round; mid-round completion continues from its durable end.
    const bool new_round =
        have_prior && !old.active && absolute == prior.total_count && request.start_index == 0;
    bool resume = have_prior && old.active;
    bool pinned = have_prior && !new_round;
    uint32_t start = pinned ? static_cast<uint32_t>(absolute) : 0;
    wqn::WordPackIndex index;
    esp_err_t indexed = pinned ? wqn::LoadWordPackIndexForSession(prior.session, &index)
                               : wqn::LoadWordPackIndex(&index);
    bool restarted = false;
    if (pinned && indexed != ESP_OK)
    {
        wqn::WordPackIndex current;
        const esp_err_t fresh = wqn::LoadWordPackIndex(&current);
        if (fresh == ESP_OK && !wqn::LocalSequenceSnapshotMatches(current, prior))
        {
            index = std::move(current);
            indexed = ESP_OK;
            pinned = false;
            resume = false;
            start = 0;
            restarted = true;
        }
    }
    if (indexed != ESP_OK || !index.mounted || !index.has_manifest || index.pack_error ||
        index.truncated || index.entries.empty())
    {
        result->result = indexed == ESP_OK ? ESP_ERR_INVALID_STATE : indexed;
        result->message = "词库未缓存或校验失败，请同步";
        return;
    }
    if (pinned && !wqn::LocalSequenceSnapshotMatches(index, prior))
    {
        result->result = ESP_ERR_INVALID_RESPONSE;
        return;
    }
    wqn::LocalWordEligibility eligibility;
    std::string round_seed = pinned ? prior.session.remote.seed : request.seed;
    if (shuffle && !resume)
    {
        std::string token;
        if (wqn::LoadAccessToken(&token) != ESP_OK || !wqn::IsValidAccessToken(token))
        {
            result->result = ESP_ERR_INVALID_STATE;
            result->message = "请先同步学习状态";
            return;
        }
        esp_err_t state = ESP_ERR_NOT_FOUND;
        esp_err_t refresh = ESP_ERR_NOT_FOUND;
        if (pinned)
        {
            state = wqn::LoadLocalWordEligibility(round_seed, &eligibility);
        }
        else
        {
            state = wqn::LoadLocalWordEligibility("", &eligibility);
            const bool reusable = state == ESP_OK &&
                                  eligibility.actor_binding == wqn::WordLocalSha256(token) &&
                                  wqn::LocalWordEligibilityMatches(index, eligibility, scope);
            if (!reusable && state != ESP_FAIL && wqn::services::IsConnectivityOnline())
                state = refresh = RefreshLocalWordEligibility(
                    index, scope, request.scope_generation, token, &eligibility);
        }
        if (state != ESP_OK || eligibility.actor_binding != wqn::WordLocalSha256(token) ||
            (pinned && (eligibility.deck_scope_generation != request.scope_generation ||
                        eligibility.progress_revision != prior.session.remote.progress_revision)) ||
            !wqn::LocalWordEligibilityMatches(index, eligibility, scope))
        {
            result->legacy_online_fallback =
                !pinned && refresh == ESP_ERR_NOT_SUPPORTED && state == ESP_ERR_NOT_SUPPORTED;
            result->result = state == ESP_OK ? ESP_ERR_INVALID_STATE : state;
            result->message = "学习状态未缓存或已失效，请同步";
            return;
        }
        if (!pinned)
        {
            round_seed = request.session_id;
            eligibility.round_id = round_seed;
            eligibility.deck_scope_generation = request.scope_generation;
            result->result = wqn::SaveLocalWordEligibility(eligibility);
            if (result->result != ESP_OK)
                return;
        }
    }
    if (resume)
    {
        old.paused = false;
        result->result = wqn::SaveWordSessionCursor(old);
        if (result->result != ESP_OK)
            return;
        result->persisted_session = std::move(old);
        result->local_round_total = prior.total_count;
        result->local_segment_start = prior.session.start_index;
    }
    else
    {
        wqn::LocalWordSessionDescriptor descriptor;
        result->result = wqn::PrepareLocalWordSequence(
            index, scope, start, request.session_id,
            round_seed.empty() ? "local-sequential-v1" : round_seed, request.scope_generation,
            &descriptor, mode, shuffle ? &eligibility : nullptr);
        if (result->result == ESP_ERR_NOT_FOUND)
        {
            result->result = ESP_OK;
            result->local_round_complete = true;
            return;
        }
        if (result->result != ESP_OK)
            return;
        result->session_persist_result = wqn::SaveLocalWordSessionDescriptor(descriptor);
        if (result->session_persist_result != ESP_OK)
        {
            result->result = result->session_persist_result;
            return;
        }
        result->session_compact_result =
            wqn::BuildLocalWordInitialSession(descriptor, 32, &result->persisted_session);
        if (result->session_compact_result != ESP_OK)
        {
            result->result = result->session_compact_result;
            return;
        }
        result->session_persist_result = wqn::SavePersistedWordSession(result->persisted_session);
        if (result->session_persist_result != ESP_OK)
        {
            result->result = result->session_persist_result;
            return;
        }
        result->local_round_total = descriptor.total_count;
        result->local_segment_start = descriptor.session.start_index;
    }
    result->result = ESP_OK;
    result->pack_index = std::move(index);
    result->pack_index_ready = true;
    result->message =
        restarted ? "词库已更新，已从第一词开始" : (shuffle ? "本轮随机已就绪" : "顺序学习已就绪");
}

void ExecuteLocalWordPage(const WordCloudRequest& request, WordCloudResult* result)
{
    if (result == nullptr) return;
    result->op = WordCloudOp::kFetchSessionPage;
    result->local_walk = true;
    wqn::PersistedWordSession parked;
    const bool have_parked =
        TakePageExtension(request.page_extension_handle, &parked);
    wqn::LocalWordSessionDescriptor descriptor;
    result->result = wqn::LoadLocalWordSessionDescriptor(
        request.session_id, &descriptor);
    if (result->result != ESP_OK || !have_parked ||
        descriptor.session.deck_scope_generation != request.scope_generation ||
        descriptor.session.remote.session_id !=
            parked.remote.session_id) {
        if (result->result == ESP_OK) result->result = ESP_ERR_INVALID_STATE;
        result->message = "本地顺序会话已失效";
        return;
    }
    result->local_round_total = descriptor.total_count;
    result->result = wqn::GetLocalWordCandidatePage(
        descriptor, request.cursor,
        request.limit == 0 ? 64 : request.limit,
        &result->candidate_page);
    if (result->result == ESP_ERR_NOT_FOUND) {
        // Segment exhausted: the worker result arms the next descriptor only
        // after the UI's final batch ACK has cleared the buffer.
        result->result = ESP_OK;
        result->candidate_page = {};
        result->candidate_page.cursor = request.cursor;
        result->candidate_page.has_more = false;
        result->message = "本段已学完";
        result->persisted_session = parked;
        result->session_compact_result = ESP_OK;
        result->session_persist_result = ESP_OK;
        return;
    }
    if (result->result != ESP_OK) return;
    result->persisted_session = parked;
    result->session_compact_result =
        wqn::ExtendPersistedWordSessionWithPage(
            parked, result->candidate_page, &result->persisted_session);
    if (result->session_compact_result != ESP_OK) {
        result->result = result->session_compact_result;
        return;
    }
    if (result->persisted_session.active) {
        result->session_persist_result =
            wqn::SavePersistedWordSession(result->persisted_session);
        if (result->session_persist_result != ESP_OK) {
            result->result = result->session_persist_result;
            return;
        }
    }
    result->result = ESP_OK;
}

}  // namespace

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

    result.control_session_id = request.session_id;
    if (request.op == WordCloudOp::kClearSession) {
        result.result = wqn::ClearPersistedWordSessionIfMatches(
            static_cast<wqn::protocol::word_study_v1::Mode>(request.study_mode),
            request.session_id, request.scope_generation);
        SendWordCloudResult(result_domain);
        return;
    }
    if (request.op == WordCloudOp::kStartSession && request.resume_existing &&
        request.study_mode != static_cast<uint8_t>(wqn::protocol::word_study_v1::Mode::kSequential)) {
        result.result = wqn::LoadPersistedWordSession(
            static_cast<wqn::protocol::word_study_v1::Mode>(request.study_mode), &result.persisted_session);
        if (result.result == ESP_OK && result.persisted_session.remote.mode == wqn::protocol::word_study_v1::Mode::kShuffle &&
            wqn::IsValidLocalSessionUuid(result.persisted_session.remote.seed)) {
            ExecuteLocalWordStart(request,&result);SendWordCloudResult(result_domain);return;
        }
        if (result.result == ESP_OK && result.persisted_session.active) {
            result.result = wqn::LoadWordPackIndexForSession(result.persisted_session, &result.pack_index);
            if (result.result == ESP_OK) {
                result.persisted_session.paused = false;
                result.result = wqn::SaveWordSessionCursor(result.persisted_session);
                result.pack_index_ready = result.result == ESP_OK;
            }
        } else if (result.result == ESP_OK) result.result = ESP_ERR_NOT_FOUND;
        SendWordCloudResult(result_domain);
        return;
    }

    // Local sequential walks must never require a valid token or an online
    // create/intake round first. Dispatch before ANY token validation.
    if (IsLocalWordStudyOp(request)) {
        if (request.op == WordCloudOp::kStartSession) {
            ExecuteLocalWordStart(request, &result);
        } else {
            ExecuteLocalWordPage(request, &result);
        }
        if (!result.legacy_online_fallback) {
            SendWordCloudResult(result_domain);
            return;
        }
        // An older cloud may not expose personal snapshots yet. Preserve its
        // existing online shuffle behavior only for a new round with no cache.
        // Never replace a pinned local round or reinterpret an I/O failure.
        result.local_walk = false;
        result.message.clear();
    }

    if(request.op==WordCloudOp::kStartSession &&
       request.study_mode==static_cast<uint8_t>(wqn::protocol::word_study_v1::Mode::kReview) &&
       !wqn::services::IsConnectivityOnline()) {
        result.result=ESP_ERR_TIMEOUT;result.review_queue_unavailable=true;
        result.message="离线，到期队列尚未更新";
        SendWordCloudResult(result_domain);return;
    }
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
            if (result.result == ESP_ERR_NOT_FOUND) {
                had_local_manifest = false;
                local_manifest = {};
                result.result = ESP_OK;
            } else if (result.result != ESP_OK) {
                // Keep every file when a load cannot distinguish resource
                // failure from corruption. Only confirmed absence starts at 0.
                ESP_LOGW(
                    kTag,
                    "local word pack manifest unavailable; preserve cache: %s",
                    esp_err_to_name(result.result));
                result.message = "词库清单读取失败，已保留本地内容";
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
        session.start_index = session.mode == wqn::protocol::word_study_v1::Mode::kSequential
            ? request.start_index : -1;
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
            PersistInitialWordSessionSnapshot(token, &result);
        }
        // Include empty sessions: they advance the intake->sequential chain
        // without a snapshot save, so save-only logs cannot diagnose it.
        ESP_LOGI(kTag,
            "word session start result: mode=%u items=%u active=%d has_more=%d result=%s "
            "compact_result=%s persist_result=%s session=%s",
            static_cast<unsigned>(session.mode), static_cast<unsigned>(result.session.items.size()),
            result.persisted_session.active ? 1 : 0, result.session.has_more ? 1 : 0,
            esp_err_to_name(result.result), esp_err_to_name(result.session_compact_result),
            esp_err_to_name(result.session_persist_result), result.session.session_id.c_str());
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
        if (CanOfferLocalWalkAfterReviewFailure(request, result)) {
            result.review_queue_unavailable = true;
            result.message = "到期队列暂不可用，可继续顺序过词库";
        }
    }
    if (request.op == WordCloudOp::kPackSync) {
        wqn::services::CompleteContentRefresh(
            content_ticket, result.result, nullptr,
            result.result == ESP_OK ? nullptr : result.message.c_str());
        // [deck-autorepair] Build the mounted deck catalog HERE on the bulk
        // runner lane, where SPIFFS reads are allowed -- the UI apply step must
        // never read SPIFFS. This runs for EVERY confirmed full manifest
        // success: changed content, unchanged manifest, and an authoritative
        // empty catalog (all decks deleted). Only a full success (result==OK)
        // yields an authoritative payload; a partial/failed walk leaves it
        // false so the UI never arms an NVS-clearing repair from bad data.
        if (result.result == ESP_OK) {
            if (wqn::BuildWordDeckCatalog(&result.catalog) == ESP_OK) {
                result.catalog_authoritative = true;
            }
        }
    }
    if(request.op==WordCloudOp::kPackSync && result.result==ESP_OK &&
        wqn::services::IsConnectivityOnline()) {
        wqn::WordPackIndex cached;
        const auto* index=&result.pack_index;
        if(!result.pack_index_ready) {
            if(wqn::LoadWordPackIndex(&cached)==ESP_OK)index=&cached;
        }
        if(index->mounted && index->has_manifest && !index->pack_error && !index->truncated)
            (void)RefreshLocalWordEligibility(*index,request.deck_id,request.scope_generation,token,nullptr);
    }
    SendWordCloudResult(result_domain);
}

}  // namespace device_ui_internal
