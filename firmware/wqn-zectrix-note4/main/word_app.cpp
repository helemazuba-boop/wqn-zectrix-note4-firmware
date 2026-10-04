#include "word_app.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "storage.h"

namespace {

constexpr char kTag[] = "wqn_word";
constexpr size_t kWordHomeSelectionCount = 3;
// Written by the sync service, read by the UI task when it builds a snapshot.
std::atomic<int32_t> g_word_review_due_count{0};
// Mistakes-pool hint written by the sync service; -1 = unknown (no sync yet).
std::atomic<int32_t> g_word_mistake_count{-1};
// [word-modes-v2] Local replay rules of the review entry: a word the user did
// not recognize comes back after at least kWordReplayMinGap answered cards,
// and at most kWordReplayMax times per session. The FSRS timeline stays
// authoritative -- the unknown judgment already set due=now server-side, so
// the pool only decides how the session interleaves it locally.
constexpr uint32_t kWordReplayMinGap = 5;
constexpr uint8_t kWordReplayMax = 2;
// Today's new-word budget, spent at the head of 顺序过词库. The server keeps
// the daily accounting (word_progress.created_at); the device only asks.
constexpr int kWordNewWordDailyLimit = 20;
constexpr size_t kCandidatePrefetchThreshold =
    wqn::protocol::word_study_v1::kInitialCandidatePageSize;
constexpr wqn::protocol::word_study_v1::Mode kPersistedSessionModes[] = {
    wqn::protocol::word_study_v1::Mode::kSequential,
    wqn::protocol::word_study_v1::Mode::kReview,
    wqn::protocol::word_study_v1::Mode::kShuffle,
    wqn::protocol::word_study_v1::Mode::kMistakes,
};

size_t SelectionIndex(wqn::WordHomeSelection selection)
{
    return static_cast<size_t>(selection);
}

wqn::WordHomeSelection HomeSelectionFromIndex(size_t index)
{
    switch (index % kWordHomeSelectionCount) {
        case 0:
            return wqn::WordHomeSelection::kReview;
        case 1:
            return wqn::WordHomeSelection::kShuffle;
        default:
            return wqn::WordHomeSelection::kMistakes;
    }
}

[[maybe_unused]] std::string HomeSelectionLabel(wqn::WordHomeSelection selection)
{
    switch (selection) {
        case wqn::WordHomeSelection::kReview:
            return "智能复习";
        case wqn::WordHomeSelection::kShuffle:
            return "随机";
        case wqn::WordHomeSelection::kMistakes:
            return "遗忘的单词";
    }
    return "智能复习";
}

bool HasPackWords(const wqn::WordAppState& state)
{
    return !state.pack_index.entries.empty();
}

wqn::WordCardPhase CardPhaseFromSession(
    const wqn::PersistedWordSession& session)
{
    return session.phase == wqn::WordPresentationPhase::kBack
        ? wqn::WordCardPhase::kRevealed
        : wqn::WordCardPhase::kFront;
}

bool IsReviewSession(const wqn::PersistedWordSession& session)
{
    return session.remote.mode == wqn::protocol::word_study_v1::Mode::kReview;
}

bool IsIntakeSession(const wqn::PersistedWordSession& session)
{
    return session.remote.mode == wqn::protocol::word_study_v1::Mode::kIntake;
}

bool IsSequentialSession(const wqn::PersistedWordSession& session)
{
    return session.remote.mode == wqn::protocol::word_study_v1::Mode::kSequential;
}

void ShowStudyCard(wqn::WordAppState* state)
{
    if (state == nullptr) return;
    state->mode = wqn::WordAppMode::kWordCard;
    state->card_phase = CardPhaseFromSession(state->session.persisted);
}

void SetStudySessionResumable(
    wqn::WordAppState* state,
    wqn::protocol::word_study_v1::Mode mode,
    bool resumable)
{
    if (state == nullptr) return;
    switch (mode) {
        case wqn::protocol::word_study_v1::Mode::kReview:
            state->review_session_resumable = resumable;
            break;
        case wqn::protocol::word_study_v1::Mode::kShuffle:
            state->shuffle_session_resumable = resumable;
            break;
        case wqn::protocol::word_study_v1::Mode::kMistakes:
            state->mistakes_session_resumable = resumable;
            break;
        case wqn::protocol::word_study_v1::Mode::kSequential:
        case wqn::protocol::word_study_v1::Mode::kIntake:
            // The library walk has no home card of its own: it is resumed from
            // the completion page and its resume point is the durable cursor.
            break;
        case wqn::protocol::word_study_v1::Mode::kRandom:
        case wqn::protocol::word_study_v1::Mode::kDictionary:
            break;
    }
}

// ---- review replay pool ---------------------------------------------------

uint32_t NextReplayRandom(uint32_t* seed)
{
    if (seed == nullptr || *seed == 0) return 0;
    // xorshift32: tiny and deterministic, which is all that interleaving a
    // handful of replays needs.
    uint32_t value = *seed;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *seed = value;
    return value;
}

void SeedReviewRuntime(
    wqn::WordReviewRuntime* review,
    const std::string& session_id)
{
    if (review == nullptr) return;
    review->pool.clear();
    review->seed = 0;
    review->cards_seen = 0;
    review->replay_in_flight = false;
    review->replay_pool_index = 0;
    review->replay_return_ordinal = 0;
    review->next_is_replay = false;
    review->next_pool_index = 0;
    review->next_return_ordinal = 0;
    review->reviewed = 0;
    review->unknown = 0;
    review->replayed = 0;
    review->replay_unknown = 0;
    review->complete_selection = wqn::WordCompleteSelection::kSequential;
    uint32_t hash = 2166136261u;
    for (const char ch : session_id) {
        hash ^= static_cast<uint8_t>(ch);
        hash *= 16777619u;
    }
    hash ^= static_cast<uint32_t>(esp_timer_get_time());
    review->seed = hash != 0 ? hash : 0x9E3779B9u;
}

bool PickReadyReplay(wqn::WordReviewRuntime* review, size_t* index)
{
    if (review == nullptr || index == nullptr) return false;
    size_t ready_count = 0;
    for (const wqn::WordReplayEntry& entry : review->pool) {
        if (entry.ready_at <= review->cards_seen) ++ready_count;
    }
    if (ready_count == 0) return false;
    size_t target = ready_count > 1
        ? NextReplayRandom(&review->seed) % ready_count
        : 0;
    for (size_t i = 0; i < review->pool.size(); ++i) {
        if (review->pool[i].ready_at > review->cards_seen) continue;
        if (target == 0) {
            *index = i;
            return true;
        }
        --target;
    }
    return false;
}

// The spacing rule cannot be honoured once the queue is empty; drain the entry
// that has been waiting longest.
bool PickAnyReplay(const wqn::WordReviewRuntime& review, size_t* index)
{
    if (review.pool.empty() || index == nullptr) return false;
    size_t best = 0;
    for (size_t i = 1; i < review.pool.size(); ++i) {
        if (review.pool[i].ready_at < review.pool[best].ready_at) best = i;
    }
    *index = best;
    return true;
}

// Pool bookkeeping for one committed review judgment. Runs on commit (not on
// Prepare) so a failed persist can be retried without counting a replay twice.
void ApplyWordReviewBookkeeping(
    wqn::WordAppState* state,
    wqn::protocol::word_study_v1::ObservationAction action,
    uint64_t answered_ordinal)
{
    using wqn::protocol::word_study_v1::ObservationAction;
    if (state == nullptr) return;
    auto& review = state->review;
    const bool answered_replay = review.replay_in_flight;
    ++review.cards_seen;
    if (answered_replay) {
        const size_t pool_index = review.replay_pool_index;
        if (action == ObservationAction::kKnown ||
            action == ObservationAction::kUnknown) {
            ++review.replayed;
        }
        if (pool_index < review.pool.size()) {
            wqn::WordReplayEntry& entry = review.pool[pool_index];
            if (action == ObservationAction::kKnown) {
                // Recognized this time: the word leaves the pool.
                review.pool.erase(
                    review.pool.begin() + static_cast<std::ptrdiff_t>(pool_index));
            } else if (action == ObservationAction::kUnknown) {
                ++review.replay_unknown;
                ++entry.replays;
                if (entry.replays >= kWordReplayMax) {
                    review.pool.erase(
                        review.pool.begin() + static_cast<std::ptrdiff_t>(pool_index));
                } else {
                    entry.ready_at = review.cards_seen + kWordReplayMinGap;
                }
            } else {
                // Skipped without judging: keep it, just push it back.
                entry.ready_at = review.cards_seen + kWordReplayMinGap;
            }
        }
        review.replay_in_flight = false;
        review.replay_pool_index = 0;
    } else if (action == ObservationAction::kUnknown) {
        const auto existing = std::find_if(
            review.pool.begin(),
            review.pool.end(),
            [answered_ordinal](const wqn::WordReplayEntry& entry) {
                return entry.ordinal == answered_ordinal;
            });
        if (existing != review.pool.end()) {
            existing->ready_at = review.cards_seen + kWordReplayMinGap;
        } else {
            wqn::WordReplayEntry entry;
            entry.ordinal = answered_ordinal;
            entry.replays = 0;
            entry.ready_at = review.cards_seen + kWordReplayMinGap;
            review.pool.push_back(entry);
            // "X 张没答对" counts distinct words, so a word missed again on a
            // replay does not inflate it.
            ++review.unknown;
        }
    }
    if (action == ObservationAction::kKnown ||
        action == ObservationAction::kUnknown) {
        ++review.reviewed;
    }
    // A replay chosen while leaving this card starts with the next one.
    if (review.next_is_replay) {
        review.replay_in_flight = true;
        review.replay_pool_index = review.next_pool_index;
        review.replay_return_ordinal = review.next_return_ordinal;
        review.next_is_replay = false;
        review.next_pool_index = 0;
        review.next_return_ordinal = 0;
    }
}

// ---- session cursor helpers ----------------------------------------------

uint64_t SessionEndOrdinal(const wqn::StoredWordSessionData& remote)
{
    return remote.items.empty() ? 0 : remote.items.back().ordinal + 1;
}

size_t SessionIndexOfOrdinal(
    const wqn::StoredWordSessionData& remote,
    uint64_t ordinal)
{
    for (size_t i = 0; i < remote.items.size(); ++i) {
        if (remote.items[i].ordinal == ordinal) return i;
    }
    return remote.items.size();
}

bool IsChainSkipped(const wqn::WordSessionChain& chain, uint64_t ordinal)
{
    return std::find(
               chain.skip_ordinals.begin(),
               chain.skip_ordinals.end(),
               ordinal) != chain.skip_ordinals.end();
}

// Next queue index at/after `from`, skipping the intake words the sequential
// walk must not repeat. May return items.size() (queue done).
size_t NextQueuePosition(const wqn::WordAppState& state, size_t from)
{
    const auto& items = state.session.persisted.remote.items;
    size_t position = from;
    while (position < items.size() &&
           IsChainSkipped(state.chain, items[position].ordinal)) {
        ++position;
    }
    return position;
}

// Where a plain (non-replay) advance from the current card lands.
uint64_t NextQueueOrdinal(const wqn::WordAppState& state)
{
    const auto& remote = state.session.persisted.remote;
    const size_t next =
        NextQueuePosition(state, state.session.persisted.position + 1);
    return next < remote.items.size() ? remote.items[next].ordinal
                                      : SessionEndOrdinal(remote);
}


esp_err_t LoadCurrentReviewWord(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    state->current_word = wqn::WqnWordEntry{};
    const auto& session = state->session.persisted;
    if (!session.active || session.position >= session.remote.items.size()) {
        return ESP_OK;
    }
    const char* item_id = session.remote.items[session.position].item_id;
    const auto entry = std::find_if(
        state->pack_index.entries.begin(),
        state->pack_index.entries.end(),
        [&](const wqn::WordPackIndexEntry& value) {
            return std::strcmp(item_id, value.word_id) == 0;
        });
    if (entry == state->pack_index.entries.end()) {
        return ESP_ERR_NOT_FOUND;
    }
    return wqn::ReadWordPackEntry(*entry, &state->current_word);
}

// Decide where the card on screen leads. Review sessions interleave the replay
// pool into the remaining queue; every other mode is a straight walk. Pure with
// respect to the pool: the bookkeeping happens when the observation commits
// (ApplyWordObservationCommitResult), so a failed persist can be retried
// without counting a replay twice.
uint64_t PlannedNextOrdinal(wqn::WordAppState* state)
{
    const auto& persisted = state->session.persisted;
    const auto& remote = persisted.remote;
    auto& review = state->review;
    review.next_is_replay = false;
    review.next_pool_index = 0;
    review.next_return_ordinal = 0;
    if (review.replay_in_flight) {
        // Answering a replay returns to the queue position it interrupted.
        return review.replay_return_ordinal;
    }
    if (!IsReviewSession(persisted)) {
        return NextQueueOrdinal(*state);
    }
    // 1) A due replay is inserted into the remaining queue.
    size_t pool_index = 0;
    if (PickReadyReplay(&review, &pool_index)) {
        const size_t item_index =
            SessionIndexOfOrdinal(remote, review.pool[pool_index].ordinal);
        if (item_index < remote.items.size()) {
            review.next_is_replay = true;
            review.next_pool_index = pool_index;
            review.next_return_ordinal = NextQueueOrdinal(*state);
            return review.pool[pool_index].ordinal;
        }
        // The word rolled out of the local candidate window: forget it.
        review.pool.erase(
            review.pool.begin() + static_cast<std::ptrdiff_t>(pool_index));
    }
    // 2) The queue itself.
    const size_t next = NextQueuePosition(*state, persisted.position + 1);
    if (next < remote.items.size()) {
        return remote.items[next].ordinal;
    }
    // 3) Queue exhausted and no further pages: drain whatever the pool still
    //    holds. The minimum-gap rule degrades to "as soon as possible" here.
    if (!remote.has_more && PickAnyReplay(review, &pool_index)) {
        const size_t item_index =
            SessionIndexOfOrdinal(remote, review.pool[pool_index].ordinal);
        if (item_index < remote.items.size()) {
            review.next_is_replay = true;
            review.next_pool_index = pool_index;
            review.next_return_ordinal = SessionEndOrdinal(remote);
            return review.pool[pool_index].ordinal;
        }
        review.pool.erase(
            review.pool.begin() + static_cast<std::ptrdiff_t>(pool_index));
    }
    return SessionEndOrdinal(remote);
}

void PrepareObservation(
    wqn::WordAppState* state,
    wqn::protocol::word_study_v1::ObservationAction action,
    wqn::WordObservationTarget target,
    wqn::WordPresentationPhase next_phase)
{
    if (state == nullptr || !state->session.persisted.active ||
        state->session.persisted.position >=
            state->session.persisted.remote.items.size()) {
        return;
    }
    const auto& remote = state->session.persisted.remote;
    const size_t position = state->session.persisted.position;
    uint64_t next_ordinal = remote.items[position].ordinal;
    if (target == wqn::WordObservationTarget::kAdvance) {
        next_ordinal = PlannedNextOrdinal(state);
    }
    if (next_ordinal > UINT32_MAX) {
        state->message = "会话游标超限";
        return;
    }
    auto& observation = state->session.pending_observation;
    observation = {};
    observation.session_id = remote.session_id;
    observation.sequence = remote.next_sequence;
    observation.item_id = remote.items[position].item_id;
    observation.action = action;
    observation.mode = remote.mode;
    observation.next_position = static_cast<uint32_t>(next_ordinal);
    observation.next_phase = next_phase;
    state->session.commit_state = wqn::WordObservationCommitState::kPersisting;
    state->card_phase = wqn::WordCardPhase::kPersisting;
    state->session.observation_effect_ready = true;
    state->message = "正在保存";
}

size_t RemainingCandidateItems(const wqn::PersistedWordSession& session)
{
    return session.position < session.remote.items.size()
        ? session.remote.items.size() - session.position
        : 0;
}

// Compaction erases the already-answered prefix of the candidate window; drop
// every local ordinal reference that rolled out with it (a pooled replay whose
// item is gone can no longer be shown, and a stale skip ordinal is harmless but
// noise).
void PruneSessionOrdinals(wqn::WordAppState* state)
{
    if (state == nullptr) return;
    const auto& items = state->session.persisted.remote.items;
    const auto present = [&items](uint64_t ordinal) {
        return std::any_of(
            items.begin(),
            items.end(),
            [ordinal](const wqn::StoredWordSessionItem& item) {
                return item.ordinal == ordinal;
            });
    };
    auto& pool = state->review.pool;
    pool.erase(
        std::remove_if(
            pool.begin(),
            pool.end(),
            [&present](const wqn::WordReplayEntry& entry) {
                return !present(entry.ordinal);
            }),
        pool.end());
    if (state->review.replay_in_flight &&
        state->review.replay_return_ordinal !=
            SessionEndOrdinal(state->session.persisted.remote) &&
        !present(state->review.replay_return_ordinal)) {
        // The interrupted queue position rolled out of the window: continue
        // from the end of the new one.
        state->review.replay_return_ordinal =
            SessionEndOrdinal(state->session.persisted.remote);
    }
    auto& skip = state->chain.skip_ordinals;
    skip.erase(
        std::remove_if(
            skip.begin(),
            skip.end(),
            [&present](uint64_t ordinal) { return !present(ordinal); }),
        skip.end());
}

void RequestCandidatePageIfNeeded(wqn::WordAppState* state)
{
    if (state == nullptr || !state->session.persisted.active ||
        state->session.persisted.paused ||
        !state->session.persisted.remote.has_more ||
        state->session.page_in_flight || state->session.page_requested) {
        return;
    }
    if (RemainingCandidateItems(state->session.persisted) >
        kCandidatePrefetchThreshold) {
        return;
    }
    // [word-modes-v2] Compaction drops the answered prefix, which is exactly
    // where a pending replay lives. Wait for the pool to drain (a few cards)
    // before pulling the next page.
    if (!state->review.pool.empty()) {
        return;
    }
    state->session.page_requested = true;
}

bool SetSessionCursorOrdinal(
    wqn::PersistedWordSession* session,
    uint32_t ordinal)
{
    if (session == nullptr) return false;
    const auto match = std::find_if(
        session->remote.items.begin(), session->remote.items.end(),
        [&](const auto& item) { return item.ordinal == ordinal; });
    if (match != session->remote.items.end()) {
        session->position = static_cast<uint32_t>(
            match - session->remote.items.begin());
        return true;
    }
    const uint64_t end_ordinal = session->remote.items.empty()
        ? 0
        : session->remote.items.back().ordinal + 1;
    if (ordinal == end_ordinal) {
        session->position = static_cast<uint32_t>(session->remote.items.size());
        return true;
    }
    return false;
}

bool SnapshotMatches(
    const wqn::StoredWordSessionData& session,
    const wqn::protocol::word_study_v1::CandidatePageData& page)
{
    if (session.snapshot.size() != page.snapshot.size()) return false;
    for (size_t index = 0; index < session.snapshot.size(); ++index) {
        const auto& stored = session.snapshot[index];
        const auto& remote = page.snapshot[index];
        if (remote.deck_id != stored.deck_id ||
            remote.content_revision != stored.content_revision ||
            remote.pack_revision != stored.pack_revision ||
            remote.sha256 != stored.sha256) {
            return false;
        }
    }
    return true;
}

uint16_t ClampUint16(size_t value)
{
    return static_cast<uint16_t>(std::min<size_t>(value, UINT16_MAX));
}

void InstallWordPackIndex(
    wqn::WordAppState* state,
    wqn::WordPackIndex index,
    const std::string& message)
{
    const bool has_manifest = index.has_manifest;
    const bool pack_error = index.pack_error;
    const std::string status_message = index.status_message;
    state->pack_index = std::move(index);
    // Every install lands the library index unless the caller re-pins it
    // immediately (the resumed-session path below does).
    state->pack_index_pinned = false;
    state->cloud_loaded_once = has_manifest;
    state->cloud_sync_failed = pack_error;
    state->cloud_sync_requested = !has_manifest || pack_error;
    state->message = !message.empty() ? message : status_message;
    if (state->message.empty()) {
        state->message = HasPackWords(*state) ? "词库已就绪" : "词库未同步";
    }
}

void ActivatePendingWordPackIndex(wqn::WordAppState* state)
{
    if (state == nullptr || state->mode != wqn::WordAppMode::kHome ||
        (state->session.persisted.active && !state->session.persisted.paused) ||
        !state->pending_pack_index_ready) {
        return;
    }
    wqn::WordPackIndex pending = std::move(state->pending_pack_index);
    state->pending_pack_index = {};
    state->pending_pack_index_ready = false;
    InstallWordPackIndex(state, std::move(pending), "词库更新已启用");
}

// [word-sequential-chain] Persist how far the library walk has come, so the
// completion page can offer 续 #N after a reboot. Only meaningful for a
// sequential session; every other mode has no library cursor.
void SaveSequentialCursor(wqn::WordAppState* state)
{
    if (state == nullptr || !IsSequentialSession(state->session.persisted)) {
        return;
    }
    const uint64_t index = static_cast<uint64_t>(state->session.persisted.start_index) +
        state->session.persisted.position;
    const uint32_t cursor =
        index > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(index);
    const esp_err_t result = wqn::SaveWordSequentialCursor(cursor);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "save sequential cursor failed: %s", esp_err_to_name(result));
        return;
    }
    state->chain.sequential_cursor = cursor;
}

// Arm the library walk (mode=sequential from the durable cursor). Used both by
// the completion page and after the intake head of the chain.
void StartSequentialWalk(wqn::WordAppState* state, bool from_chain)
{
    if (state == nullptr) return;
    if (state->session.requested_mode !=
        wqn::protocol::word_study_v1::Mode::kSequential) {
        state->session.create_request_id.clear();
    }
    state->session.requested_mode = wqn::protocol::word_study_v1::Mode::kSequential;
    state->session.requested_start_index = state->chain.sequential_cursor;
    state->session.requested_new_word_limit = 0;
    state->session.start_requested = true;
    state->mode = wqn::WordAppMode::kSessionStarting;
    state->message = from_chain ? "正在进入词库顺序" : "正在准备词库顺序";
}

// Arm today's new words (mode=intake), the head of 顺序过词库.
void StartIntakeHead(wqn::WordAppState* state)
{
    if (state == nullptr) return;
    state->session.requested_mode = wqn::protocol::word_study_v1::Mode::kIntake;
    state->session.requested_start_index = 0;
    state->session.requested_new_word_limit = kWordNewWordDailyLimit;
    state->session.create_request_id.clear();
    state->session.start_requested = true;
    state->mode = wqn::WordAppMode::kSessionStarting;
    state->message = "正在准备今日新词";
}

void FinishOrLoadAdvancedReview(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return;
    }
    auto& session = state->session.persisted;
    if (session.active && session.position < session.remote.items.size()) {
        const esp_err_t load_result = LoadCurrentReviewWord(state);
        if (load_result == ESP_OK) {
            ShowStudyCard(state);
            return;
        }
        state->message = "会话词包不可用";
    }
    // [word-modes-v2] A review session is not finished while its local replay
    // pool still holds a word: jump back to it. This also runs before paging,
    // because compaction drops the pool entries that rolled out of the window.
    if (session.active && IsReviewSession(session) &&
        !state->review.replay_in_flight) {
        size_t pool_index = 0;
        if (PickAnyReplay(state->review, &pool_index)) {
            const size_t item_index = SessionIndexOfOrdinal(
                session.remote, state->review.pool[pool_index].ordinal);
            if (item_index < session.remote.items.size()) {
                state->review.replay_in_flight = true;
                state->review.replay_pool_index = pool_index;
                state->review.replay_return_ordinal =
                    SessionEndOrdinal(session.remote);
                session.position = static_cast<uint32_t>(item_index);
                if (LoadCurrentReviewWord(state) == ESP_OK) {
                    ShowStudyCard(state);
                    state->message = "重学一遍";
                    return;
                }
                state->review.replay_in_flight = false;
                state->message = "会话词包不可用";
            } else {
                state->review.pool.erase(
                    state->review.pool.begin() +
                    static_cast<std::ptrdiff_t>(pool_index));
                FinishOrLoadAdvancedReview(state);
                return;
            }
        }
    }
    if (session.active && session.position >= session.remote.items.size() &&
        session.remote.has_more) {
        state->mode = wqn::WordAppMode::kSessionStarting;
        state->session.page_requested = true;
        state->message = "正在加载后续单词";
        return;
    }
    // [word-sequential-chain] The intake head is done: remember its words so
    // the walk does not repeat them, then arm the sequential session.
    if (IsIntakeSession(session) && state->chain.active) {
        state->chain.exclude_ids.clear();
        for (const wqn::StoredWordSessionItem& item : session.remote.items) {
            state->chain.exclude_ids.push_back(item.item_id);
        }
        session.active = false;
        session.paused = false;
        StartSequentialWalk(state, true);
        return;
    }
    // The observation effect may already have cleared `active` (the queue ran
    // out), so the mode -- not the flag -- decides where this lands.
    const bool completed_review = IsReviewSession(session);
    const bool completed_sequential = IsSequentialSession(session);
    session.active = false;
    session.paused = false;
    SetStudySessionResumable(state, session.remote.mode, false);
    state->current_word = wqn::WqnWordEntry{};
    if (completed_sequential) {
        // The walk reached the end of the library: wrap the cursor so the next
        // 顺序过词库 starts over instead of asking for an empty slice.
        state->chain.sequential_cursor = 0;
        const esp_err_t wrap_result = wqn::SaveWordSequentialCursor(0);
        if (wrap_result != ESP_OK) {
            ESP_LOGW(
                kTag,
                "reset sequential cursor failed: %s",
                esp_err_to_name(wrap_result));
        }
    }
    if (state->pending_pack_index_ready) {
        ActivatePendingWordPackIndex(state);
    } else if (state->pack_index_pinned) {
        // Only a resumed session's pinned snapshot needs replacing. Pack files
        // are immutable, so re-reading SPIFFS and re-hashing every pack on a
        // plain completion would freeze the caller for seconds and change
        // nothing (the boot contract fixtures hit this path hundreds of times).
        wqn::WordPackIndex current;
        if (wqn::LoadWordPackIndex(&current) == ESP_OK) {
            InstallWordPackIndex(state, std::move(current), "");
        }
    }
    if (completed_review) {
        state->mode = wqn::WordAppMode::kReviewComplete;
        state->review.complete_selection = wqn::WordCompleteSelection::kSequential;
        state->message = "今天的复习完成了";
        // Nothing is due anymore: drop the sync hint so the home card does not
        // keep advertising yesterday's queue until the next sync.
        wqn::SetWordReviewDueCount(0);
        return;
    }
    state->mode = wqn::WordAppMode::kHome;
    state->message = "本轮浏览完成";
}

// Long-press on a card: keep the cursor, remember it as resumable, go home.
void PauseWordSession(wqn::WordAppState* state)
{
    if (state == nullptr) return;
    state->session.persisted.paused = true;
    SetStudySessionResumable(state, state->session.persisted.remote.mode, true);
    SaveSequentialCursor(state);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        wqn::SaveWordSessionCursor(state->session.persisted));
    state->mode = wqn::WordAppMode::kHome;
    state->message = "本轮已暂停";
    ActivatePendingWordPackIndex(state);
}

// Resume a paused session of `mode`, pinning the session's own pack snapshot
// when the mounted index moved on. Returns true when the caller must not start
// a new session (the card is on screen, or the pinned pack is unavailable).
bool TryResumePausedSession(
    wqn::WordAppState* state,
    wqn::protocol::word_study_v1::Mode mode)
{
    if (state == nullptr) return false;
    wqn::PersistedWordSession stored_session;
    if (wqn::LoadPersistedWordSession(mode, &stored_session) == ESP_OK &&
        stored_session.active && stored_session.paused &&
        (stored_session.position < stored_session.remote.items.size() ||
         (stored_session.position == stored_session.remote.items.size() &&
          stored_session.remote.has_more))) {
        state->session.persisted = std::move(stored_session);
        if (!wqn::WordPackIndexMatchesSession(
                state->pack_index, state->session.persisted)) {
            wqn::WordPackIndex pinned_index;
            const esp_err_t pinned_result = wqn::LoadWordPackIndexForSession(
                state->session.persisted, &pinned_index);
            if (pinned_result != ESP_OK || pinned_index.pack_error) {
                state->message = "会话词包不可用";
                return true;
            }
            InstallWordPackIndex(state, std::move(pinned_index), "已载入会话词包");
            state->pack_index_pinned = true;
        } else {
            ESP_LOGI(kTag, "reuse in-memory word pack index for pinned session");
        }
    }
    if (!state->session.persisted.active ||
        !state->session.persisted.paused ||
        state->session.persisted.remote.mode != mode ||
        !(state->session.persisted.position <
              state->session.persisted.remote.items.size() ||
          (state->session.persisted.position ==
               state->session.persisted.remote.items.size() &&
           state->session.persisted.remote.has_more))) {
        return false;
    }
    state->session.persisted.paused = false;
    SetStudySessionResumable(state, mode, false);
    if (mode == wqn::protocol::word_study_v1::Mode::kReview) {
        // A resumed review session starts a fresh local pool: every word the
        // user missed is already due=now on the server.
        SeedReviewRuntime(&state->review, state->session.persisted.remote.session_id);
    }
    const esp_err_t cursor_result =
        wqn::SaveWordSessionCursor(state->session.persisted);
    if (cursor_result != ESP_OK) {
        ESP_LOGW(kTag, "resume word session failed: %s", esp_err_to_name(cursor_result));
        state->message = "会话未保存，请重试";
        return true;
    }
    if (state->session.persisted.position ==
        state->session.persisted.remote.items.size()) {
        state->mode = wqn::WordAppMode::kSessionStarting;
        state->session.page_requested = true;
        state->message = "正在加载后续单词";
        return true;
    }
    if (LoadCurrentReviewWord(state) != ESP_OK) {
        state->message = "会话词包不可用";
        return true;
    }
    ShowStudyCard(state);
    state->message = "已继续上次会话";
    RequestCandidatePageIfNeeded(state);
    return true;
}

}  // namespace

namespace wqn {

void SetWordReviewDueCount(int count)
{
    g_word_review_due_count.store(
        count > 0 ? count : 0, std::memory_order_release);
}

void SetWordMistakeCount(int count)
{
    g_word_mistake_count.store(
        count >= 0 ? count : -1, std::memory_order_release);
}

esp_err_t InitWordApp(WordAppState* state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->initialized) {
        return ESP_OK;
    }

    state->mode = WordAppMode::kHome;
    state->card_phase = WordCardPhase::kFront;
    state->home_selection = WordHomeSelection::kReview;
    state->message = "词库同步中";
    // The sequential walk resumes from this cursor (and the completion page
    // shows it as 续 #N).
    uint32_t sequential_cursor = 0;
    if (wqn::LoadWordSequentialCursor(&sequential_cursor) != ESP_OK) {
        sequential_cursor = 0;
    }
    state->chain.sequential_cursor = sequential_cursor;

    const esp_err_t storage_result = InitWordPackStorage();
    if (storage_result != ESP_OK) {
        state->message = "词库分区不可用";
    } else {
        WordPackIndex index;
        const esp_err_t index_result = LoadWordPackIndex(&index);
        const std::string index_message = index.status_message;
        ApplyWordPackIndex(state, std::move(index), index_message);
        if (index_result != ESP_OK) {
            ESP_LOGW(kTag, "load local word pack index failed: %s", esp_err_to_name(index_result));
        }
    }

    WordOutboxSnapshot outbox;
    if (ReadWordOutboxSnapshot(&outbox) == ESP_OK) {
        state->outbox.pending_count = outbox.pending_count;
        state->outbox.suspended_count = outbox.suspended_count;
        state->outbox.blocked_count = outbox.blocked_count;
        state->outbox.capacity = outbox.capacity;
    }

    if (LoadDefaultWordDeckId(&state->default_deck_id) != ESP_OK) {
        state->default_deck_id.clear();
    }
    std::vector<WordDeckInfo> catalog;
    if (BuildWordDeckCatalog(&catalog) == ESP_OK) {
        InstallWordDeckCatalog(state, std::move(catalog));
    }

    bool found_paused_session = false;
    bool found_corrupt_session = false;
    for (const auto mode : kPersistedSessionModes) {
        PersistedWordSession persisted;
        const esp_err_t session_result = LoadPersistedWordSession(mode, &persisted);
        const bool resumable = persisted.active &&
            (persisted.position < persisted.remote.items.size() ||
             (persisted.position == persisted.remote.items.size() &&
              persisted.remote.has_more));
        if (session_result == ESP_OK && resumable) {
            if (persisted.remote.mode ==
                protocol::word_study_v1::Mode::kSequential) {
                // No home card of its own: the walk is resumed from the review
                // completion page, and its cursor is already loaded.
                state->chain.sequential_cursor =
                    persisted.start_index + persisted.position;
            } else {
                SetStudySessionResumable(state, persisted.remote.mode, true);
            }
        }
        if (session_result == ESP_OK && resumable) {
            // At most one session should be unpaused. Prefer it so a reset
            // restores the exact card and presentation phase.
            if (!persisted.paused) {
                state->session.persisted = std::move(persisted);
                if (state->session.persisted.position ==
                    state->session.persisted.remote.items.size()) {
                    state->mode = WordAppMode::kSessionStarting;
                    state->session.page_requested = true;
                    state->message = "正在恢复后续单词";
                } else if (LoadCurrentReviewWord(state) == ESP_OK) {
                    ShowStudyCard(state);
                    state->message = "已恢复上次会话";
                } else {
                    state->mode = WordAppMode::kHome;
                    state->message = "上次会话可继续";
                }
                break;
            }
            if (!found_paused_session) {
                state->session.persisted = std::move(persisted);
                found_paused_session = true;
            }
        } else if (session_result == ESP_ERR_INVALID_VERSION) {
            // [word-modes-v2] A record written by an older schema (v3 lacked
            // start_index) can never be loaded again. Drop it instead of
            // greeting every upgraded device with "会话记录损坏": a session is
            // only a browse cursor, the observation outbox is the durable one.
            ESP_LOGI(
                kTag,
                "discard legacy word session: mode=%u",
                static_cast<unsigned>(mode));
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ClearPersistedWordSession(mode));
        } else if (session_result != ESP_OK && session_result != ESP_ERR_NOT_FOUND) {
            found_corrupt_session = true;
            ESP_LOGW(
                kTag,
                "load persisted word session failed: mode=%u error=%s",
                static_cast<unsigned>(mode),
                esp_err_to_name(session_result));
        }
    }
    RequestCandidatePageIfNeeded(state);
    if (state->mode == WordAppMode::kHome && found_paused_session) {
        state->message = "上次会话可继续";
    } else if (state->mode == WordAppMode::kHome && found_corrupt_session) {
        state->message = "会话记录损坏";
    }

    ESP_LOGI(
        kTag,
        "word runtime restored: mode=%u session=%s position=%u phase=%u pending=%u",
        static_cast<unsigned>(state->mode),
        state->session.persisted.remote.session_id.empty() ? "none" :
            state->session.persisted.remote.session_id.c_str(),
        static_cast<unsigned>(state->session.persisted.position),
        static_cast<unsigned>(state->session.persisted.phase),
        static_cast<unsigned>(state->outbox.pending_count));

    state->initialized = true;
    state->cloud_sync_requested = !state->cloud_loaded_once || state->pack_index.pack_error;
    return ESP_OK;
}

esp_err_t HandleWordAppInput(WordAppState* state, WordInput input)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!state->initialized) {
        ESP_RETURN_ON_ERROR(InitWordApp(state), kTag, "init word app");
    }
    ActivatePendingWordPackIndex(state);

    switch (state->mode) {
        case WordAppMode::kHome: {
            size_t selected = SelectionIndex(state->home_selection);
            if (input == WordInput::kUp) {
                selected = (selected + kWordHomeSelectionCount - 1) % kWordHomeSelectionCount;
                state->home_selection = HomeSelectionFromIndex(selected);
                return ESP_OK;
            }
            if (input == WordInput::kDown) {
                selected = (selected + 1) % kWordHomeSelectionCount;
                state->home_selection = HomeSelectionFromIndex(selected);
                return ESP_OK;
            }
            if (input == WordInput::kLongConfirm) {
                // Long-press confirm on the word home is handled by the UI layer
                // (ui_model.cpp) to exit back to the device home screen. Pack
                // sync is triggered automatically when entering the word page.
                return ESP_OK;
            }
            if (input != WordInput::kConfirm) {
                return ESP_OK;
            }
            if (!HasPackWords(*state)) {
                state->message = state->pack_index.status_message.empty() ? "词库未同步" : state->pack_index.status_message;
                state->cloud_sync_requested = true;
                return ESP_OK;
            }
            const auto requested_mode =
                state->home_selection == WordHomeSelection::kShuffle
                    ? protocol::word_study_v1::Mode::kShuffle
                    : (state->home_selection == WordHomeSelection::kMistakes
                           ? protocol::word_study_v1::Mode::kMistakes
                           : protocol::word_study_v1::Mode::kReview);
            if (TryResumePausedSession(state, requested_mode)) {
                return ESP_OK;
            }
            if (state->session.requested_mode != requested_mode) {
                state->session.create_request_id.clear();
            }
            state->session.requested_mode = requested_mode;
            state->session.requested_start_index = 0;
            state->session.requested_new_word_limit = 0;
            state->session.start_requested = true;
            state->mode = WordAppMode::kSessionStarting;
            state->message = "正在准备本轮单词";
            return ESP_OK;
        }

        case WordAppMode::kSessionStarting:
            if (input == WordInput::kConfirm &&
                state->session.persisted.active &&
                state->session.persisted.position ==
                    state->session.persisted.remote.items.size() &&
                state->session.persisted.remote.has_more) {
                state->session.page_requested = true;
                state->message = "正在重试加载";
                return ESP_OK;
            }
            if (input == WordInput::kLongConfirm) {
                state->session.start_requested = false;
                CancelWordSessionStartResult(state);
                if (state->session.persisted.active) {
                    state->session.persisted.paused = true;
                    SetStudySessionResumable(
                        state, state->session.persisted.remote.mode, true);
                    SaveSequentialCursor(state);
                    ESP_ERROR_CHECK_WITHOUT_ABORT(
                        SaveWordSessionCursor(state->session.persisted));
                }
                state->chain.active = false;
                state->chain.exclude_ids.clear();
                state->chain.skip_ordinals.clear();
                state->mode = WordAppMode::kHome;
                state->message = state->session.persisted.active
                    ? "本轮已暂停"
                    : "已取消";
            }
            return ESP_OK;

        case WordAppMode::kWordCard:
            if (state->card_phase == WordCardPhase::kPersisting) {
                return ESP_OK;
            }
            if (state->session.commit_state == WordObservationCommitState::kFailed) {
                if (input == WordInput::kConfirm) {
                    state->session.commit_state = WordObservationCommitState::kPersisting;
                    state->card_phase = WordCardPhase::kPersisting;
                    state->session.observation_effect_ready = true;
                    state->message = "正在重试保存";
                }
                return ESP_OK;
            }
            if (state->card_phase == WordCardPhase::kFront) {
                if (input == WordInput::kConfirm) {
                    PrepareObservation(
                        state,
                        protocol::word_study_v1::ObservationAction::kRevealed,
                        WordObservationTarget::kStay,
                        WordPresentationPhase::kBack);
                } else if (input == WordInput::kDown) {
                    PrepareObservation(
                        state,
                        protocol::word_study_v1::ObservationAction::kSkipped,
                        WordObservationTarget::kAdvance,
                        WordPresentationPhase::kFront);
                } else if (input == WordInput::kLongConfirm) {
                    PauseWordSession(state);
                }
                return ESP_OK;
            }
            if (input == WordInput::kConfirm) {
                PrepareObservation(
                    state,
                    protocol::word_study_v1::ObservationAction::kKnown,
                    WordObservationTarget::kAdvance,
                    WordPresentationPhase::kFront);
            } else if (input == WordInput::kUp) {
                PrepareObservation(
                    state,
                    protocol::word_study_v1::ObservationAction::kUnknown,
                    WordObservationTarget::kAdvance,
                    WordPresentationPhase::kFront);
            } else if (input == WordInput::kDown) {
                PrepareObservation(
                    state,
                    protocol::word_study_v1::ObservationAction::kSkipped,
                    WordObservationTarget::kAdvance,
                    WordPresentationPhase::kFront);
            } else if (input == WordInput::kLongConfirm) {
                PauseWordSession(state);
            }
            return ESP_OK;

        case WordAppMode::kReviewComplete:
            if (input == WordInput::kUp || input == WordInput::kDown) {
                state->review.complete_selection =
                    state->review.complete_selection ==
                        WordCompleteSelection::kSequential
                    ? WordCompleteSelection::kReturn
                    : WordCompleteSelection::kSequential;
                return ESP_OK;
            }
            if (input == WordInput::kLongConfirm) {
                state->mode = WordAppMode::kHome;
                state->message = "已返回单词主页";
                ActivatePendingWordPackIndex(state);
                return ESP_OK;
            }
            if (input != WordInput::kConfirm) {
                return ESP_OK;
            }
            if (state->review.complete_selection ==
                WordCompleteSelection::kReturn) {
                state->mode = WordAppMode::kHome;
                state->message = "已返回单词主页";
                ActivatePendingWordPackIndex(state);
                return ESP_OK;
            }
            if (!HasPackWords(*state)) {
                state->message = "词库未同步";
                state->cloud_sync_requested = true;
                return ESP_OK;
            }
            // [word-sequential-chain] 顺序过词库: today's new words first (the
            // server's intake accounting), then the library walk from the
            // durable cursor. A paused walk/intake session resumes instead.
            state->chain.active = true;
            if (state->chain.sequential_cursor >= state->pack_index.entries.size()) {
                state->chain.sequential_cursor = 0;
                ESP_ERROR_CHECK_WITHOUT_ABORT(wqn::SaveWordSequentialCursor(0));
            }
            if (TryResumePausedSession(
                    state, protocol::word_study_v1::Mode::kIntake)) {
                return ESP_OK;
            }
            if (TryResumePausedSession(
                    state, protocol::word_study_v1::Mode::kSequential)) {
                return ESP_OK;
            }
            state->chain.exclude_ids.clear();
            state->chain.skip_ordinals.clear();
            StartIntakeHead(state);
            return ESP_OK;
    }

    return ESP_OK;
}

esp_err_t BuildWordDeckCatalog(std::vector<WordDeckInfo>* catalog)
{
    if (catalog == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    catalog->clear();
    WqnWordPackManifest manifest;
    const esp_err_t result = LoadWordPackManifest(&manifest);
    if (result == ESP_ERR_NOT_FOUND) {
        return ESP_OK;  // not synced yet: empty catalog
    }
    if (result != ESP_OK) {
        return result;
    }
    catalog->reserve(manifest.packs.size());
    for (const WqnWordPackManifestItem& item : manifest.packs) {
        if (item.deleted || item.deck_id.size() != 36) {
            continue;
        }
        WordDeckInfo deck;
        deck.deck_id = item.deck_id;
        deck.title = item.title;
        deck.entry_count = item.entry_count;
        catalog->push_back(std::move(deck));
    }
    return ESP_OK;
}

void InstallWordDeckCatalog(WordAppState* state, std::vector<WordDeckInfo> catalog)
{
    if (state == nullptr) {
        return;
    }
    state->deck_catalog = std::move(catalog);
    state->default_deck_title.clear();
    if (state->default_deck_id.empty()) {
        return;
    }
    for (const WordDeckInfo& deck : state->deck_catalog) {
        if (deck.deck_id == state->default_deck_id) {
            state->default_deck_title = deck.title;
            return;
        }
    }
    if (!state->deck_catalog.empty()) {
        // The default deck disappeared from the cloud: fall back to all decks
        // in memory (NVS keeps the id; re-picking in settings overwrites it).
        ESP_LOGW(kTag, "default word deck missing from catalog; falling back to all decks");
        state->default_deck_id.clear();
    }
}

void SetDefaultWordDeck(
    WordAppState* state, const std::string& deck_id, const std::string& title)
{
    if (state == nullptr) {
        return;
    }
    state->default_deck_id = deck_id;
    state->default_deck_title = deck_id.empty() ? std::string() : title;
}

void ResetWordSessionsForScopeChange(WordAppState* state, bool clear_persisted)
{
    if (state == nullptr) {
        return;
    }
    ESP_LOGI(
        kTag,
        "word session reset after deck scope change: old_session=%s clear_persisted=%d",
        state->session.persisted.remote.session_id.empty()
            ? "none"
            : state->session.persisted.remote.session_id.c_str(),
        clear_persisted ? 1 : 0);
    if (clear_persisted) {
        for (const auto mode : kPersistedSessionModes) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(ClearPersistedWordSession(mode));
        }
    }
    state->session = WordSessionState{};
    state->review = WordReviewRuntime{};
    state->chain = WordSessionChain{};
    // The library walk's cursor indexes the scoped library, so a scope switch
    // invalidates it.
    ESP_ERROR_CHECK_WITHOUT_ABORT(SaveWordSequentialCursor(0));
    state->review_session_resumable = false;
    state->shuffle_session_resumable = false;
    state->mistakes_session_resumable = false;
    state->card_phase = WordCardPhase::kFront;
    state->current_word = WqnWordEntry{};
    state->mode = WordAppMode::kHome;
    state->message = "词库范围已切换";
}

void ApplyWordPackIndex(WordAppState* state, WordPackIndex index, const std::string& message)
{
    if (state == nullptr) {
        return;
    }
    if (state->mode != WordAppMode::kHome ||
        (state->session.persisted.active && !state->session.persisted.paused)) {
        state->pending_pack_index = std::move(index);
        state->pending_pack_index_ready = true;
        state->cloud_loaded_once = state->pending_pack_index.has_manifest;
        state->cloud_sync_failed = state->pending_pack_index.pack_error;
        state->cloud_sync_requested = !state->pending_pack_index.has_manifest ||
            state->pending_pack_index.pack_error;
        state->message = state->pending_pack_index.pack_error
            ? "词库更新校验失败"
            : "词库已更新，下轮启用";
        return;
    }
    InstallWordPackIndex(state, std::move(index), message);
}

bool TakeWordSessionStartRequest(
    WordAppState* state,
    protocol::word_study_v1::CreateSessionRequest* request)
{
    if (state == nullptr || request == nullptr || !state->session.start_requested) {
        return false;
    }
    if (state->session.create_request_id.empty()) {
        state->session.create_request_id = request->metadata.request_id;
    } else {
        request->metadata.request_id = state->session.create_request_id;
    }
    request->mode = state->session.requested_mode;
    request->scope = {};
    // Study sessions honour the deck scope ([词] row override first, then the
    // NVS default).
    const std::string& scope_deck_id = !state->scoped_deck_id.empty()
        ? state->scoped_deck_id
        : state->default_deck_id;
    if (scope_deck_id.size() == 36) {
        request->scope.deck_ids.push_back(scope_deck_id);
    }
    request->optional_count = 500;
    request->seed.clear();
    request->start_index = -1;
    request->new_word_limit = 0;
    if (request->mode == protocol::word_study_v1::Mode::kSequential) {
        request->start_index =
            static_cast<int>(state->session.requested_start_index);
    } else if (request->mode == protocol::word_study_v1::Mode::kIntake) {
        request->new_word_limit = state->session.requested_new_word_limit;
    }
    state->session.start_requested = false;
    state->session.start_result_expected = true;
    return true;
}

bool ApplyWordSessionStartResult(
    WordAppState* state,
    esp_err_t result,
    esp_err_t compact_result,
    esp_err_t persist_result,
    PersistedWordSession persisted)
{
    if (state == nullptr || !state->session.start_result_expected) return false;
    state->session.start_result_expected = false;
    const auto requested_mode = state->session.requested_mode;
    if (result != ESP_OK) {
        state->mode = WordAppMode::kHome;
        state->message = result == ESP_ERR_INVALID_STATE
            ? "请先完成配对"
            : "本轮准备失败，可重试";
        state->chain.active = false;
        state->chain.exclude_ids.clear();
        state->chain.skip_ordinals.clear();
        return true;
    }
    // The runner thread already compacted and (for active sessions) persisted
    // the snapshot; only the in-memory install happens here.
    if (compact_result != ESP_OK) {
        state->mode = WordAppMode::kHome;
        state->message = "会话数据过大";
        state->chain.active = false;
        state->chain.exclude_ids.clear();
        state->chain.skip_ordinals.clear();
        return true;
    }
    if (!persisted.active) {
        state->session.create_request_id.clear();
        // [word-modes-v2] An empty review session is not a failure: it is the
        // "nothing due today" variant of the completion page. An empty intake
        // head just means today's new-word budget is spent -- the walk still
        // starts.
        if (requested_mode == protocol::word_study_v1::Mode::kReview) {
            SeedReviewRuntime(&state->review, std::string());
            state->mode = WordAppMode::kReviewComplete;
            state->review.complete_selection =
                WordCompleteSelection::kSequential;
            state->message = "今天没有到期的单词";
            return true;
        }
        if (requested_mode == protocol::word_study_v1::Mode::kIntake &&
            state->chain.active) {
            state->chain.exclude_ids.clear();
            StartSequentialWalk(state, true);
            return true;
        }
        state->mode = WordAppMode::kHome;
        state->message = "当前范围没有可浏览的单词";
        return true;
    }
    if (persist_result != ESP_OK) {
        state->mode = WordAppMode::kHome;
        state->message = "会话未保存，请重试";
        state->chain.active = false;
        state->chain.exclude_ids.clear();
        state->chain.skip_ordinals.clear();
        return true;
    }
    state->session.persisted = std::move(persisted);
    if (IsSequentialSession(state->session.persisted)) {
        // The server response does not carry the walk's continuation point;
        // stamp the value the request was built with.
        state->session.persisted.start_index =
            state->session.requested_start_index;
    }
    SetStudySessionResumable(
        state, state->session.persisted.remote.mode, false);
    state->session.commit_state = WordObservationCommitState::kIdle;
    state->session.page_in_flight = false;
    state->session.page_requested = false;
    state->session.create_request_id.clear();
    if (IsReviewSession(state->session.persisted)) {
        SeedReviewRuntime(&state->review, state->session.persisted.remote.session_id);
    }
    if (IsSequentialSession(state->session.persisted) && state->chain.active) {
        // The walk must not repeat the words the intake head just introduced.
        state->chain.skip_ordinals.clear();
        for (const StoredWordSessionItem& item :
             state->session.persisted.remote.items) {
            const auto match = std::find(
                state->chain.exclude_ids.begin(),
                state->chain.exclude_ids.end(),
                std::string(item.item_id));
            if (match != state->chain.exclude_ids.end()) {
                state->chain.skip_ordinals.push_back(item.ordinal);
            }
        }
        state->chain.exclude_ids.clear();
        const size_t first =
            NextQueuePosition(*state, state->session.persisted.position);
        state->session.persisted.position = static_cast<uint32_t>(first);
        if (first >= state->session.persisted.remote.items.size()) {
            // This whole window was already introduced by the intake head:
            // hand over to the normal paging/finish path.
            FinishOrLoadAdvancedReview(state);
            return true;
        }
    }
    result = LoadCurrentReviewWord(state);
    if (result != ESP_OK) {
        state->session.persisted.active = false;
        SetStudySessionResumable(
            state, state->session.persisted.remote.mode, false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(SavePersistedWordSession(state->session.persisted));
        state->mode = WordAppMode::kHome;
        state->message = "会话词包尚未就绪";
        state->chain.active = false;
        state->chain.exclude_ids.clear();
        state->chain.skip_ordinals.clear();
        return true;
    }
    ShowStudyCard(state);
    state->message = "确认翻面";
    RequestCandidatePageIfNeeded(state);
    return true;
}

void CancelWordSessionStartResult(WordAppState* state)
{
    if (state == nullptr) return;
    state->session.start_requested = false;
    state->session.start_result_expected = false;
    state->session.create_request_id.clear();
}

void ResetWordSessionForServerInvalid(WordAppState* state)
{
    if (state == nullptr) return;
    // The server rejected this session as unusable (corrupt snapshot / gone /
    // inactive). Retrying the same session_id can never repair it, so drop the
    // durable record and every derived flag; the next entry creates a fresh
    // session. The local pack index is untouched, so the word library stays
    // usable.
    const protocol::word_study_v1::Mode mode = state->session.persisted.remote.mode;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ClearPersistedWordSession(mode));
    state->session = WordSessionState{};
    state->review = WordReviewRuntime{};
    state->chain.active = false;
    state->chain.exclude_ids.clear();
    state->chain.skip_ordinals.clear();
    state->review_session_resumable = false;
    state->shuffle_session_resumable = false;
    state->mistakes_session_resumable = false;
    state->card_phase = WordCardPhase::kFront;
    state->current_word = WqnWordEntry{};
    state->mode = WordAppMode::kHome;
    state->message = "上次会话已失效，请重新开始";
}

bool TakeWordCandidatePageRequest(
    WordAppState* state,
    protocol::word_study_v1::CandidatePageRequest* request,
    PersistedWordSession* snapshot,
    std::string* session_id)
{
    if (state == nullptr || request == nullptr || snapshot == nullptr ||
        session_id == nullptr ||
        !state->session.page_requested || state->session.page_in_flight ||
        !state->session.persisted.active ||
        state->session.persisted.paused ||
        !state->session.persisted.remote.has_more ||
        state->session.persisted.remote.cursor.empty()) {
        return false;
    }
    request->cursor = state->session.persisted.remote.cursor;
    request->limit = static_cast<int>(
        protocol::word_study_v1::kCandidatePrefetchPageSize);
    // [ui-gates] Snapshot as of this take: the runner persists THIS one, so it
    // must be the state the page was asked for, not whatever the user has
    // advanced to by the time the page comes back.
    *snapshot = state->session.persisted;
    *session_id = state->session.persisted.remote.session_id;
    state->session.page_requested = false;
    state->session.page_in_flight = true;
    return true;
}

void RestoreWordCandidatePageRequest(WordAppState* state)
{
    if (state == nullptr) return;
    state->session.page_in_flight = false;
    if (state->session.persisted.active &&
        !state->session.persisted.paused &&
        state->session.persisted.remote.has_more) {
        state->session.page_requested = true;
    }
}

// [ui-gates] The runner's extend/persist outcomes transport the UI messages the
// old inline apply produced; keeping them in one place stops the two paths from
// drifting apart in wording.
const char* WordPageExtendMessage(esp_err_t compact_result)
{
    switch (compact_result) {
        case ESP_ERR_INVALID_RESPONSE:
            return "后续单词快照不一致";
        case ESP_ERR_INVALID_STATE:
            return "会话游标损坏";
        case ESP_ERR_INVALID_SIZE:
            return "候选窗口超限";
        case ESP_ERR_INVALID_ARG:
            return "候选页顺序无效";
        default:
            return "后续单词已就绪";
    }
}

void ApplyWordCandidatePageResult(
    WordAppState* state,
    esp_err_t result,
    esp_err_t compact_result,
    esp_err_t persist_result,
    const PersistedWordSession& runner_snapshot,
    protocol::word_study_v1::CandidatePageData page)
{
    if (state == nullptr) return;
    state->session.page_in_flight = false;
    auto& persisted = state->session.persisted;
    if (!persisted.active || persisted.paused) {
        // The user left or paused while this bounded prefetch was in flight.
        // Its result belongs to the old interaction context and must not
        // replace the home message or revive the session UI.
        return;
    }
    if (result != ESP_OK) {
        state->session.page_requested = false;
        state->message = "后续单词加载失败，继续时重试";
        return;
    }
    // [ui-gates] The runner extended and persisted the snapshot the page was
    // QUEUED with. If the session has advanced since -- the user answered while
    // the page was in flight -- that snapshot is stale: installing it would roll
    // the position back, and the durable state is already ahead because the
    // observation commit persisted the advanced session. Keep the in-memory
    // state and merge the page into it here; the next commit re-persists.
    const bool runner_snapshot_current =
        persisted.remote.session_id == runner_snapshot.remote.session_id &&
        persisted.position == runner_snapshot.position;
    PersistedWordSession updated;
    if (runner_snapshot_current) {
        if (compact_result != ESP_OK) {
            state->message = WordPageExtendMessage(compact_result);
            return;
        }
        if (persist_result != ESP_OK) {
            state->message = "后续单词未保存";
            return;
        }
        updated = runner_snapshot;
    } else {
        const esp_err_t merge_result = wqn::ExtendPersistedWordSessionWithPage(
            persisted, page, &updated);
        if (merge_result != ESP_OK) {
            state->message = WordPageExtendMessage(merge_result);
            return;
        }
        ESP_LOGI(
            kTag,
            "candidate page merged over an advanced session: position=%lu runner=%lu",
            static_cast<unsigned long>(persisted.position),
            static_cast<unsigned long>(runner_snapshot.position));
    }
    persisted = std::move(updated);
    PruneSessionOrdinals(state);
    auto& committed_remote = persisted.remote;
    if (persisted.position < committed_remote.items.size()) {
        result = LoadCurrentReviewWord(state);
        if (result != ESP_OK) {
            state->message = "会话词包不可用";
            return;
        }
        ShowStudyCard(state);
        state->message = "后续单词已就绪";
    } else if (!committed_remote.has_more) {
        // The last page brought nothing left to show: let the normal finish
        // path decide (review completion page, replay drain, or home).
        FinishOrLoadAdvancedReview(state);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            SavePersistedWordSession(state->session.persisted));
        RequestCandidatePageIfNeeded(state);
        return;
    }
    RequestCandidatePageIfNeeded(state);
}

bool TakeWordObservationEffect(
    WordAppState* state,
    const std::string& request_id,
    const std::string& occurred_at,
    uint32_t operation_id,
    DurableWordObservation* observation,
    PersistedWordSession* advanced_session)
{
    if (state == nullptr || observation == nullptr || advanced_session == nullptr ||
        !state->session.observation_effect_ready || request_id.empty() ||
        occurred_at.empty()) {
        return false;
    }
    auto& pending = state->session.pending_observation;
    if (pending.request_id.empty()) {
        pending.request_id = request_id;
        pending.occurred_at = occurred_at;
    }
    PersistedWordSession advanced = state->session.persisted;
    if (pending.session_id != advanced.remote.session_id ||
        pending.sequence != advanced.remote.next_sequence ||
        !SetSessionCursorOrdinal(&advanced, pending.next_position)) {
        state->session.observation_effect_ready = false;
        state->session.commit_state = WordObservationCommitState::kFailed;
        state->card_phase = CardPhaseFromSession(state->session.persisted);
        state->message = "会话游标无效";
        return false;
    }
    advanced.phase = pending.next_phase;
    advanced.remote.next_sequence = pending.sequence + 1;
    // A review session whose replay pool still owes a word is not finished; the
    // unknown judgment below is what puts the just-missed word in that pool.
    const bool review_owes_replay =
        advanced.remote.mode == protocol::word_study_v1::Mode::kReview &&
        (!state->review.pool.empty() ||
         pending.action == protocol::word_study_v1::ObservationAction::kUnknown);
    if (advanced.position >= advanced.remote.items.size() &&
        !advanced.remote.has_more && !review_owes_replay) {
        advanced.active = false;
        advanced.paused = false;
    }
    state->session.pending_advanced_session = advanced;
    state->session.observation_effect_ready = false;
    // Bind this dispatch so a late worker result that arrives after a scope
    // reset / newer submit is rejected instead of applied.
    state->session.pending_persist_operation_id = operation_id;
    *observation = pending;
    *advanced_session = std::move(advanced);
    return true;
}

void ApplyWordObservationCommitResult(WordAppState* state, esp_err_t result)
{
    if (state == nullptr) return;
    // The bound dispatch has now been consumed; clear it so a duplicate/late
    // result cannot re-apply (the commit_state guard at the caller also blocks
    // this, but clearing keeps the expected-id invariant tight).
    state->session.pending_persist_operation_id = 0;
    if (result != ESP_OK) {
        state->session.commit_state = WordObservationCommitState::kFailed;
        state->card_phase = CardPhaseFromSession(state->session.persisted);
        state->message = result == ESP_ERR_NO_MEM
            ? "记录空间已满，确认重试"
            : "未保存，确认重试";
        return;
    }
    const auto action = state->session.pending_observation.action;
    const auto observation_mode = state->session.pending_observation.mode;
    uint64_t answered_ordinal = 0;
    if (state->session.persisted.position <
        state->session.persisted.remote.items.size()) {
        answered_ordinal =
            state->session.persisted.remote
                .items[state->session.persisted.position].ordinal;
    }
    state->session.persisted = std::move(state->session.pending_advanced_session);
    state->session.pending_advanced_session = {};
    state->session.pending_observation = {};
    state->session.commit_state = WordObservationCommitState::kCloudPending;
    if (state->outbox.pending_count + state->outbox.suspended_count <
        state->outbox.capacity) {
        ++state->outbox.pending_count;
    }
    if (action == protocol::word_study_v1::ObservationAction::kKnown) {
        ++state->reviewed_today;
        ++state->correct_today;
        state->message = "已保存，待同步";
    } else if (action == protocol::word_study_v1::ObservationAction::kUnknown) {
        ++state->reviewed_today;
        state->message = "已保存到遗忘单词，待同步";
    } else if (action == protocol::word_study_v1::ObservationAction::kSkipped) {
        state->message = "已记录跳过，待同步";
    } else {
        state->message = "已保存，待同步";
    }
    if (observation_mode == protocol::word_study_v1::Mode::kReview &&
        action != protocol::word_study_v1::ObservationAction::kRevealed) {
        ApplyWordReviewBookkeeping(state, action, answered_ordinal);
    }
    RequestCandidatePageIfNeeded(state);
    if (action == protocol::word_study_v1::ObservationAction::kRevealed &&
        state->session.persisted.active &&
        state->session.persisted.position <
            state->session.persisted.remote.items.size()) {
        // Revealing the back does not advance the item. current_word already
        // owns the exact pinned content, so reopening and reparsing the same
        // JSONL record only delays the flip.
        ShowStudyCard(state);
        return;
    }
    FinishOrLoadAdvancedReview(state);
}

void RefreshWordOutboxState(WordAppState* state)
{
    if (state == nullptr) return;
    WordOutboxSnapshot snapshot;
    if (ReadWordOutboxSnapshot(&snapshot) != ESP_OK) return;
    state->outbox.pending_count = snapshot.pending_count;
    state->outbox.suspended_count = snapshot.suspended_count;
    state->outbox.blocked_count = snapshot.blocked_count;
    state->outbox.capacity = snapshot.capacity;
    if (snapshot.pending_count == 0 && snapshot.suspended_count == 0 &&
        state->session.commit_state == WordObservationCommitState::kCloudPending) {
        state->session.commit_state = WordObservationCommitState::kCloudAcknowledged;
        if (state->mode == WordAppMode::kWordCard) {
            state->message = "已同步";
        }
    }
}

WordAppSnapshot BuildWordAppSnapshot(const WordAppState& state)
{
    WordAppSnapshot snapshot;
    snapshot.mode = state.mode;
    snapshot.card_phase = state.card_phase;
    snapshot.commit_state = state.session.commit_state;
    snapshot.home_selection = state.home_selection;
    snapshot.complete_selection = state.review.complete_selection;
    snapshot.pack_ready = HasPackWords(state);
    snapshot.pack_truncated = state.pack_index.truncated;
    snapshot.cloud_sync_failed = state.cloud_sync_failed;
    snapshot.review_session_resumable = state.review_session_resumable;
    snapshot.shuffle_session_resumable = state.shuffle_session_resumable;
    snapshot.mistakes_session_resumable = state.mistakes_session_resumable;
    snapshot.reviewed_today = state.reviewed_today;
    snapshot.correct_today = state.correct_today;
    snapshot.total_count = ClampUint16(state.pack_index.entries.size());
    const bool study_cursor_visible =
        state.mode == WordAppMode::kWordCard ||
        state.mode == WordAppMode::kSessionStarting;
    snapshot.card_count = study_cursor_visible
        ? ClampUint16(state.session.persisted.remote.items.size())
        : 0;
    snapshot.card_position = !study_cursor_visible ||
            state.session.persisted.remote.items.empty()
        ? 0
        : ClampUint16(state.session.persisted.position + 1);
    snapshot.finished_today = !state.session.persisted.active &&
        !state.session.persisted.remote.session_id.empty();
    snapshot.review_complete_empty = state.review.cards_seen == 0;
    snapshot.review_complete_reviewed = state.review.reviewed;
    snapshot.review_complete_unknown = state.review.unknown;
    snapshot.review_complete_replayed = state.review.replayed;
    const int32_t due_hint =
        g_word_review_due_count.load(std::memory_order_acquire);
    snapshot.review_due_count =
        ClampUint16(static_cast<size_t>(due_hint > 0 ? due_hint : 0));
    const int32_t mistake_hint =
        g_word_mistake_count.load(std::memory_order_acquire);
    snapshot.mistake_count = static_cast<int16_t>(
        mistake_hint > INT16_MAX ? INT16_MAX : mistake_hint);
    snapshot.sequential_cursor = state.chain.sequential_cursor;
    snapshot.sequential_total = ClampUint16(state.pack_index.entries.size());
    snapshot.pack_count = state.pack_index.pack_count;
    snapshot.pack_bytes = state.pack_index.pack_bytes;
    snapshot.progress_line = WordAppProgressLabel(state);
    snapshot.status_line = WordAppStatusLine(state);
    snapshot.hint = state.message.empty() ? "确认选择，长按确认返回" : state.message;

    const WqnWordEntry& word = state.current_word;
    if (!word.word.empty()) {
        snapshot.has_card = true;
        snapshot.word = word.word;
        snapshot.phonetic = word.phonetic;
        snapshot.meaning = word.meaning;
        snapshot.example = word.example;
        snapshot.example_translation = word.example_translation;
        snapshot.part_of_speech = word.part_of_speech;
    }
    return snapshot;
}

std::string WordAppProgressLabel(const WordAppState& state)
{
    if (state.mode != WordAppMode::kWordCard &&
        state.mode != WordAppMode::kSessionStarting) {
        return "";
    }
    if (state.session.persisted.remote.items.empty()) {
        return "";
    }
    const size_t visible_position = std::min(
        static_cast<size_t>(state.session.persisted.position) + 1,
        state.session.persisted.remote.items.size());
    if (IsSequentialSession(state.session.persisted)) {
        // The walk counts through the whole library, not the local window.
        const size_t library_index =
            static_cast<size_t>(state.session.persisted.start_index) +
            visible_position;
        const std::string total = state.pack_index.entries.empty()
            ? std::string("--")
            : std::to_string(state.pack_index.entries.size());
        return "#" + std::to_string(library_index) + "/" + total;
    }
    return std::to_string(visible_position) + "/" +
           std::to_string(state.session.persisted.remote.items.size());
}

std::string WordAppStatusLine(const WordAppState& state)
{
    if (!state.pack_index.mounted) {
        return "词库分区不可用";
    }
    if (state.cloud_sync_failed) {
        return "词库同步异常";
    }
    if (!HasPackWords(state)) {
        return state.pack_index.status_message.empty() ? "词库未同步" : state.pack_index.status_message;
    }
    if (state.outbox.suspended_count > 0) {
        std::string status =
            "同步挂起 " + std::to_string(state.outbox.suspended_count) + " 条";
        if (state.outbox.blocked_count > 0) {
            status += "，同会话待处理 " +
                std::to_string(state.outbox.blocked_count) + " 条";
        }
        return status;
    }
    if (state.outbox.pending_count > 0) {
        return "待同步 " + std::to_string(state.outbox.pending_count) + " 条";
    }
    // Surface the active study scope so "why is this word here" is answerable
    // at a glance; scoped ([词] row) wins over the settings default.
    const std::string& scope_title = !state.scoped_deck_title.empty()
        ? state.scoped_deck_title
        : state.default_deck_title;
    if (!scope_title.empty()) {
        return "当前词库 " + scope_title;
    }
    return "本地词库 " + std::to_string(state.pack_index.entries.size()) + " 词";
}

std::string WordAppSignature(const WordAppState& state)
{
    std::string signature;
    signature.reserve(180);
    signature.append(std::to_string(static_cast<int>(state.mode)));
    signature.push_back('/');
    signature.append(std::to_string(static_cast<int>(state.card_phase)));
    signature.push_back('/');
    signature.append(std::to_string(static_cast<int>(state.home_selection)));
    signature.push_back('/');
    signature.append(std::to_string(
        static_cast<int>(state.review.complete_selection)));
    signature.push_back('/');
    signature.append(std::to_string(static_cast<int>(state.session.commit_state)));
    signature.push_back('/');
    signature.append(state.review_session_resumable ? "1" : "0");
    signature.append(state.shuffle_session_resumable ? "1" : "0");
    signature.append(state.mistakes_session_resumable ? "1" : "0");
    signature.push_back('/');
    signature.append(state.scoped_deck_id);
    signature.push_back(':');
    signature.append(state.default_deck_id);
    signature.push_back('/');
    signature.append(std::to_string(state.session.persisted.start_index));
    signature.push_back('/');
    signature.append(std::to_string(state.session.persisted.position));
    signature.push_back('/');
    signature.append(std::to_string(state.session.persisted.remote.items.size()));
    signature.push_back('/');
    signature.append(std::to_string(state.session.persisted.remote.next_sequence));
    signature.push_back('/');
    signature.append(std::to_string(state.outbox.pending_count));
    signature.push_back('/');
    signature.append(std::to_string(state.outbox.suspended_count));
    signature.push_back('/');
    signature.append(std::to_string(state.outbox.blocked_count));
    signature.push_back('/');
    signature.append(state.current_word.id);
    signature.push_back('/');
    signature.append(state.current_word.word);
    signature.push_back('/');
    signature.append(std::to_string(state.review.pool.size()));
    signature.push_back('/');
    signature.append(std::to_string(state.review.cards_seen));
    signature.push_back('/');
    signature.append(std::to_string(state.review.reviewed));
    signature.push_back('/');
    signature.append(std::to_string(state.review.unknown));
    signature.push_back('/');
    signature.append(std::to_string(state.review.replayed));
    signature.push_back('/');
    signature.append(std::to_string(state.chain.sequential_cursor));
    signature.push_back('/');
    signature.append(std::to_string(
        g_word_review_due_count.load(std::memory_order_acquire)));
    signature.push_back('/');
    signature.append(std::to_string(
        g_word_mistake_count.load(std::memory_order_acquire)));
    signature.push_back('/');
    signature.append(std::to_string(state.pack_index.entries.size()));
    signature.push_back('/');
    signature.append(state.message);
    return signature;
}

namespace {

// WordAppState is intentionally large (currently 1408 bytes). The boot
// contract test needs several independent states, so keeping them as ordinary
// locals would exceed app_main's fixed 8 KiB stack even though the test runs
// sequentially. Allocate each bounded fixture in PSRAM and release it when the
// self-test returns; production runtime state ownership is unchanged.
class WordPageFixtureState {
public:
    WordPageFixtureState()
    {
        storage_ = heap_caps_malloc(
            sizeof(WordAppState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (storage_ != nullptr) {
            state_ = new (storage_) WordAppState();
        }
    }

    ~WordPageFixtureState()
    {
        if (state_ != nullptr) {
            state_->~WordAppState();
        }
        heap_caps_free(storage_);
    }

    WordPageFixtureState(const WordPageFixtureState&) = delete;
    WordPageFixtureState& operator=(const WordPageFixtureState&) = delete;

    explicit operator bool() const { return state_ != nullptr; }
    WordAppState* operator->() { return state_; }
    WordAppState& get() { return *state_; }

private:
    void* storage_ = nullptr;
    WordAppState* state_ = nullptr;
};

}  // namespace

bool RunWordPageStateSelfTest()
{
    auto require = [](bool condition, const char* label) {
        if (!condition) {
            ESP_LOGE(kTag, "word page self-test failed: %s", label);
        }
        return condition;
    };

    WordPageFixtureState home;
    if (!home) return require(false, "allocate home fixture");
    home->initialized = true;
    home->mode = WordAppMode::kHome;
    for (size_t index = 0; index < 500; ++index) {
        const WordInput input = (index % 2 == 0)
            ? WordInput::kDown
            : WordInput::kUp;
        if (HandleWordAppInput(&home.get(), input) != ESP_OK) return false;
    }
    if (!require(home->mode == WordAppMode::kHome,
                 "home mixed navigation remains home")) {
        return false;
    }

    WordPageFixtureState study;
    if (!study) return require(false, "allocate study fixture");
    study->initialized = true;
    study->mode = WordAppMode::kWordCard;
    study->card_phase = WordCardPhase::kFront;
    study->session.persisted.active = true;
    study->session.persisted.phase = WordPresentationPhase::kFront;
    study->session.persisted.remote.session_id =
        "00000000-0000-4000-8000-000000000001";
    study->session.persisted.remote.next_sequence = 7;
    StoredWordSessionItem item;
    std::snprintf(
        item.item_id,
        sizeof(item.item_id),
        "%s",
        "00000000-0000-4000-8000-000000000002");
    std::snprintf(
        item.deck_id,
        sizeof(item.deck_id),
        "%s",
        "00000000-0000-4000-8000-000000000003");
    item.ordinal = 11;
    study->session.persisted.remote.items.push_back(item);

    if (HandleWordAppInput(&study.get(), WordInput::kConfirm) != ESP_OK ||
        !require(study->mode == WordAppMode::kWordCard,
                 "front and revealed share WordCard") ||
        !require(study->card_phase == WordCardPhase::kPersisting,
                 "front confirm enters persisting") ||
        !require(study->session.pending_observation.action ==
                     protocol::word_study_v1::ObservationAction::kRevealed,
                 "front confirm records revealed")) {
        return false;
    }

    const DurableWordObservation pending =
        study->session.pending_observation;
    for (size_t index = 0; index < 500; ++index) {
        const WordInput input = index % 3 == 0
            ? WordInput::kConfirm
            : (index % 3 == 1 ? WordInput::kDown
                              : WordInput::kLongConfirm);
        if (HandleWordAppInput(&study.get(), input) != ESP_OK) return false;
    }
    if (!require(study->card_phase == WordCardPhase::kPersisting,
                 "persisting blocks mixed input") ||
        !require(study->session.pending_observation.sequence == pending.sequence &&
                     study->session.pending_observation.item_id == pending.item_id &&
                     study->session.pending_observation.action == pending.action,
                 "persisting keeps one observation")) {
        return false;
    }

    WordPageFixtureState mixed;
    if (!mixed) return require(false, "allocate mixed fixture");
    mixed->initialized = true;
    mixed->mode = WordAppMode::kWordCard;
    mixed->session.persisted.remote.session_id =
        "00000000-0000-4000-8000-000000000010";
    mixed->session.persisted.remote.mode =
        protocol::word_study_v1::Mode::kSequential;
    constexpr size_t kMixedItemCount = 64;
    mixed->session.persisted.remote.items.reserve(kMixedItemCount);
    for (size_t index = 0; index < kMixedItemCount; ++index) {
        StoredWordSessionItem mixed_item;
        std::snprintf(
            mixed_item.item_id,
            sizeof(mixed_item.item_id),
            "00000000-0000-4000-8000-%012u",
            static_cast<unsigned>(index + 100));
        std::snprintf(
            mixed_item.deck_id,
            sizeof(mixed_item.deck_id),
            "%s",
            "00000000-0000-4000-8000-000000000020");
        mixed_item.ordinal = index;
        mixed->session.persisted.remote.items.push_back(mixed_item);
    }
    for (size_t index = 0; index < 500; ++index) {
        const size_t item_index = index % kMixedItemCount;
        mixed->session.persisted.active = true;
        mixed->session.persisted.paused = false;
        mixed->session.persisted.position = item_index;
        mixed->session.persisted.phase = WordPresentationPhase::kBack;
        mixed->session.persisted.remote.next_sequence = index;
        mixed->card_phase = WordCardPhase::kRevealed;
        mixed->session.commit_state = WordObservationCommitState::kIdle;
        const WordInput input = index % 3 == 0
            ? WordInput::kConfirm
            : (index % 3 == 1 ? WordInput::kUp : WordInput::kDown);
        const auto expected_action = index % 3 == 0
            ? protocol::word_study_v1::ObservationAction::kKnown
            : (index % 3 == 1
                   ? protocol::word_study_v1::ObservationAction::kUnknown
                   : protocol::word_study_v1::ObservationAction::kSkipped);
        if (HandleWordAppInput(&mixed.get(), input) != ESP_OK) return false;
        char request_id[40] = {};
        std::snprintf(
            request_id,
            sizeof(request_id),
            "req_word_mix_%016u",
            static_cast<unsigned>(index));
        DurableWordObservation observation;
        PersistedWordSession advanced;
        if (!TakeWordObservationEffect(
                &mixed.get(),
                request_id,
                "2026-07-20T12:00:00Z",
                1u,
                &observation,
                &advanced) ||
            observation.sequence != index ||
            observation.action != expected_action ||
            observation.item_id !=
                mixed->session.persisted.remote.items[item_index].item_id ||
            advanced.position != item_index + 1 ||
            advanced.remote.next_sequence != index + 1) {
            return require(false, "500 mixed actions preserve attribution");
        }
        mixed->session.pending_observation = {};
        mixed->session.pending_advanced_session = {};
    }

    WordPageFixtureState revealed;
    if (!revealed) return require(false, "allocate revealed fixture");
    revealed->initialized = true;
    revealed->mode = WordAppMode::kWordCard;
    revealed->card_phase = WordCardPhase::kRevealed;
    revealed->session.persisted = study->session.persisted;
    revealed->session.persisted.phase = WordPresentationPhase::kBack;
    revealed->session.commit_state = WordObservationCommitState::kIdle;
    if (HandleWordAppInput(&revealed.get(), WordInput::kUp) != ESP_OK ||
        !require(revealed->session.pending_observation.action ==
                     protocol::word_study_v1::ObservationAction::kUnknown,
                 "revealed up records unknown") ||
        !require(revealed->card_phase == WordCardPhase::kPersisting,
                 "classification persists before advance")) {
        return false;
    }

    // [word-modes-v2] Review replay pool: a word the user did not recognize
    // comes back after at least five answered cards, at most twice, and the
    // queue resumes where the replay interrupted it.
    WordPageFixtureState review;
    if (!review) return require(false, "allocate review fixture");
    review->initialized = true;
    review->session.persisted.active = true;
    review->session.persisted.remote.mode =
        protocol::word_study_v1::Mode::kReview;
    review->session.persisted.remote.session_id =
        "00000000-0000-4000-8000-000000000030";
    review->session.persisted.remote.next_sequence = 100;
    constexpr size_t kReviewItemCount = 12;
    review->session.persisted.remote.items.reserve(kReviewItemCount);
    for (size_t index = 0; index < kReviewItemCount; ++index) {
        StoredWordSessionItem review_item;
        std::snprintf(
            review_item.item_id,
            sizeof(review_item.item_id),
            "00000000-0000-4000-8000-%012u",
            static_cast<unsigned>(index + 500));
        std::snprintf(
            review_item.deck_id,
            sizeof(review_item.deck_id),
            "%s",
            "00000000-0000-4000-8000-000000000020");
        review_item.ordinal = index;
        review->session.persisted.remote.items.push_back(review_item);
    }
    // Answering a card needs no pack access; the fixture only tracks the
    // planned cursor. Re-arm the card surface between commits (a commit with
    // an exhausted queue parks on the completion page).
    // Sized by the step count, not the card count: a 12-element buffer
    // written by this 16-step loop is UB past step 11, which let the
    // compiler drop the loop's exit test and delete the whole tail of
    // the self-test as unreachable (20260926.7 boot failure).
    constexpr size_t kReviewSteps = 16;
    uint32_t planned[kReviewSteps] = {};
    for (size_t step = 0; step < kReviewSteps; ++step) {
        review->mode = WordAppMode::kWordCard;
        review->card_phase = WordCardPhase::kRevealed;
        review->session.commit_state = WordObservationCommitState::kIdle;
        review->session.persisted.active = true;
        review->session.persisted.paused = false;
        review->session.persisted.phase = WordPresentationPhase::kBack;
        if (HandleWordAppInput(&review.get(), WordInput::kUp) != ESP_OK) {
            return false;
        }
        planned[step] =
            review->session.pending_observation.next_position;
        char request_id[40] = {};
        std::snprintf(
            request_id,
            sizeof(request_id),
            "req_word_review_%016u",
            static_cast<unsigned>(step));
        DurableWordObservation observation;
        PersistedWordSession advanced;
        if (!TakeWordObservationEffect(
                &review.get(),
                request_id,
                "2026-07-20T12:00:00Z",
                1u,
                &observation,
                &advanced)) {
            return require(false, "review observation enters durable effect");
        }
        ApplyWordObservationCommitResult(&review.get(), ESP_OK);
        review->session.persisted.position = advanced.position;
    }
    // Sixteen misses over twelve cards pin the whole interleave: a replay waits
    // at least five answered cards, returns to the queue position it
    // interrupted, and the second miss of a word drains it for good.
    const uint32_t expected_planned[kReviewSteps] = {
        1, 2, 3, 4, 5, 6, 0, 7, 1, 8, 2, 9, 3, 10, 0, 11};
    for (size_t step = 0; step < kReviewSteps; ++step) {
        if (!require(
                planned[step] == expected_planned[step],
                "review replay spacing and resume")) {
            return false;
        }
    }
    if (!require(
            review->review.replayed == 5 && review->review.unknown == 11 &&
                review->review.reviewed == 16 && review->review.cards_seen == 16,
            "review counters follow the commits") ||
        !require(
            review->review.pool.size() == 10 &&
                !review->review.replay_in_flight,
            "review pool drains the twice-replayed word")) {
        return false;
    }

    // A drawn replay must come from the ready set (never a word whose minimum
    // gap has not elapsed) and must actually consult the seeded stream: with a
    // constant pick every draw below would be the first ready entry.
    wqn::WordReviewRuntime seeded;
    seeded.seed = 0x12345678u;
    seeded.cards_seen = 10;
    seeded.pool = {{11, 0, 10}, {12, 0, 10}, {13, 0, 10}, {14, 0, 11}};
    const uint64_t expected_replays[] = {13, 13, 12};
    for (const uint64_t expected : expected_replays) {
        size_t pool_index = 0;
        if (!require(
                PickReadyReplay(&seeded, &pool_index),
                "seeded review has a ready replay") ||
            !require(
                seeded.pool[pool_index].ready_at <= seeded.cards_seen,
                "replay respects the minimum gap") ||
            !require(
                seeded.pool[pool_index].ordinal == expected,
                "replay draw follows the seeded stream")) {
            return false;
        }
    }

    // Queue exhausted while the pool still owes a word: the plan drains the
    // pool (returning to the end of the queue) instead of completing, and a
    // pooled ordinal that left the candidate window is forgotten.
    WordPageFixtureState drained;
    if (!drained) return require(false, "allocate drain fixture");
    drained->initialized = true;
    drained->mode = WordAppMode::kWordCard;
    drained->session.persisted.active = true;
    drained->session.persisted.remote.mode =
        protocol::word_study_v1::Mode::kReview;
    drained->session.persisted.remote.session_id =
        "00000000-0000-4000-8000-000000000033";
    StoredWordSessionItem drained_item;
    std::snprintf(
        drained_item.item_id,
        sizeof(drained_item.item_id),
        "%s",
        "00000000-0000-4000-8000-000000000034");
    std::snprintf(
        drained_item.deck_id,
        sizeof(drained_item.deck_id),
        "%s",
        "00000000-0000-4000-8000-000000000020");
    drained_item.ordinal = 0;
    drained->session.persisted.remote.items.push_back(drained_item);
    drained->session.persisted.position = 1;
    drained->review.cards_seen = 6;
    wqn::WordReplayEntry owed;
    owed.ordinal = 0;
    owed.ready_at = 7;  // minimum gap not reached: this is the drain path
    drained->review.pool.push_back(owed);
    if (!require(
            PlannedNextOrdinal(&drained.get()) == 0 &&
                drained->review.next_is_replay &&
                drained->review.next_return_ordinal == 1,
            "exhausted queue drains the replay pool")) {
        return false;
    }
    drained->review.pool.clear();
    wqn::WordReplayEntry stale_replay;
    stale_replay.ordinal = 99;
    stale_replay.ready_at = 7;
    drained->review.pool.push_back(stale_replay);
    if (!require(
            PlannedNextOrdinal(&drained.get()) == 1 &&
                drained->review.pool.empty() &&
                !drained->review.next_is_replay,
            "pooled ordinal outside the window is forgotten")) {
        return false;
    }

    // A review session whose queue empties lands on the local completion page.
    WordPageFixtureState done;
    if (!done) return require(false, "allocate completion fixture");
    done->initialized = true;
    done->session.persisted.active = true;
    done->session.persisted.remote.mode =
        protocol::word_study_v1::Mode::kReview;
    done->session.persisted.remote.session_id =
        "00000000-0000-4000-8000-000000000031";
    for (size_t index = 0; index < 3; ++index) {
        StoredWordSessionItem done_item;
        std::snprintf(
            done_item.item_id,
            sizeof(done_item.item_id),
            "00000000-0000-4000-8000-%012u",
            static_cast<unsigned>(index + 700));
        std::snprintf(
            done_item.deck_id,
            sizeof(done_item.deck_id),
            "%s",
            "00000000-0000-4000-8000-000000000020");
        done_item.ordinal = index;
        done->session.persisted.remote.items.push_back(done_item);
    }
    for (size_t step = 0; step < 3; ++step) {
        done->mode = WordAppMode::kWordCard;
        done->card_phase = WordCardPhase::kRevealed;
        done->session.commit_state = WordObservationCommitState::kIdle;
        done->session.persisted.active = true;
        done->session.persisted.paused = false;
        done->session.persisted.phase = WordPresentationPhase::kBack;
        if (HandleWordAppInput(&done.get(), WordInput::kConfirm) != ESP_OK) {
            return false;
        }
        char request_id[40] = {};
        std::snprintf(
            request_id,
            sizeof(request_id),
            "req_word_done_%016u",
            static_cast<unsigned>(step));
        DurableWordObservation observation;
        PersistedWordSession advanced;
        if (!TakeWordObservationEffect(
                &done.get(),
                request_id,
                "2026-07-20T12:00:00Z",
                1u,
                &observation,
                &advanced)) {
            return require(false, "completion observation enters effect");
        }
        ApplyWordObservationCommitResult(&done.get(), ESP_OK);
        done->session.persisted.position = advanced.position;
    }
    if (!require(
            done->mode == WordAppMode::kReviewComplete,
            "exhausted review lands on the completion page") ||
        !require(
            done->review.reviewed == 3 && done->review.unknown == 0 &&
                !done->session.persisted.active,
            "completion page keeps the session totals")) {
        return false;
    }

    // An empty review session is the "nothing due today" completion variant,
    // not an error.
    WordPageFixtureState idle;
    if (!idle) return require(false, "allocate empty-review fixture");
    idle->initialized = true;
    idle->mode = WordAppMode::kSessionStarting;
    idle->session.requested_mode = protocol::word_study_v1::Mode::kReview;
    idle->session.start_result_expected = true;
    PersistedWordSession empty_session;
    if (!require(
            ApplyWordSessionStartResult(
                &idle.get(), ESP_OK, ESP_OK, ESP_OK, std::move(empty_session)),
            "empty review result is applied") ||
        !require(
            idle->mode == WordAppMode::kReviewComplete,
            "empty review opens the completion page")) {
        return false;
    }

    WordPageFixtureState stale;
    if (!stale) return require(false, "allocate stale-result fixture");
    stale->initialized = true;
    stale->mode = WordAppMode::kHome;
    stale->home_selection = WordHomeSelection::kShuffle;
    PersistedWordSession stale_persisted;
    stale_persisted.active = true;
    if (!require(!ApplyWordSessionStartResult(
                     &stale.get(), ESP_OK, ESP_OK, ESP_OK,
                     std::move(stale_persisted)),
                 "cancelled session result is ignored") ||
        !require(stale->mode == WordAppMode::kHome &&
                     stale->home_selection == WordHomeSelection::kShuffle,
                 "stale session result preserves selection")) {
        return false;
    }

    const WordAppSnapshot front_snapshot = BuildWordAppSnapshot(study.get());
    const WordAppSnapshot done_snapshot = BuildWordAppSnapshot(done.get());
    return require(front_snapshot.mode == WordAppMode::kWordCard,
                   "study snapshot uses WordCard") &&
        require(done_snapshot.mode == WordAppMode::kReviewComplete &&
                    done_snapshot.review_complete_reviewed == 3,
                "completion snapshot carries the totals");
}

}  // namespace wqn
