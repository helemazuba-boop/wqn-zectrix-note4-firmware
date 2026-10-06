#include "opencode_session.h"

#include <algorithm>
#include <atomic>
#include <utility>

#if CONFIG_WQN_AGENT_ENABLE

#include "ai_session.h"
#include "ai_history.h"
#include "agent_round_policy.h"
#include "agent_voice_pipe.h"
#include "audio_capture.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "opencode_client.h"
#include "runtime/sleep_coordinator.h"
#include "services/connectivity_service.h"
#include "storage.h"

namespace {

constexpr char kTag[] = "wqn_agent";
constexpr TickType_t kConnectivityWait = pdMS_TO_TICKS(20000);
constexpr size_t kMaxPromptBytes = 4096;
constexpr uint32_t kWorkerStackBytes = 9216;
// [voice-pipe] How long Transcribe() waits for the agent WS turn to produce an
// ASR result after FINAL. The ASR-only pipeline is fast (streamed deltas),
// and a stuck turn should surface as an error long before the STD driver's
// 10-minute chat budget.
constexpr uint32_t kAgentVoiceFinalizeTimeoutMs = 15000;

enum class WorkerCommand : uint8_t {
    kNone,
    kLoadSessions,
    kCreateSession,
    kPrepareCapture,
    kTranscribe,
    kCancelVoice,
    kRunPrompt,
    kObserveSession,
    kLoadHistory,
};

StaticSemaphore_t g_lock_storage = {};
SemaphoreHandle_t g_lock = nullptr;
StaticTask_t g_worker_tcb = {};
StackType_t g_worker_stack[kWorkerStackBytes / sizeof(StackType_t)] = {};
TaskHandle_t g_worker = nullptr;
wqn::AgentSessionState g_state;
WorkerCommand g_command = WorkerCommand::kNone;
bool g_changed = false;
bool g_recording_requested = false;
bool g_run_failed = false;
bool g_observing = false;
std::string g_run_session_id;
std::string g_run_prompt;
// [run-id] Idempotency key of the current logical submission. Minted once at
// confirm time and reused by a transport retry of the same prompt, so the
// cloud attaches to the live run instead of running the prompt twice. Cleared
// when the prompt changes and when a run reaches a known terminal outcome
// (both mean the next submission is a new logical run).
std::string g_run_request_id;
// [detail] Detail tier for the command in flight, handed over the same way as
// g_run_session_id: written under g_lock at arm time, read by the worker
// without the lock. Not cleared on finish -- every arm overwrites it.
uint8_t g_run_detail = wqn::kOpenCodeDetailDefault;
// One-shot for the post-run list refresh: RunPrompt sets it before chaining
// kLoadHistory, so the backfill knows it is replacing the live transcript of a
// finished run (not filling the empty channel a lock starts from). Consumed by
// LoadHistory, which also leaves the run-terminal UI alone when it is set.
bool g_history_refresh = false;
wqn::OpenCodeOutboundQueue g_outbound_replies;
// The ask a queued reply answers. The UI closes its option bar the moment the
// reply is queued, so the live ask id is not proof that a reply went out: this
// is. Only a failed POST consults it, and it holds exactly one reply because
// the worker drains the queue serially.
std::string g_reply_flight_permission_id;
std::string g_reply_flight_question_id;
// Cancel handshake for an attached stream. The UI thread only sets the request;
// the worker is the one that makes the interrupt POST, so it is also the one
// that records whether the interrupt actually reached the gateway.
std::atomic<bool> g_interrupt_requested{false};
std::atomic<bool> g_interrupt_delivered{false};
// [agent] Switch-away handshake for an attached stream (symptom 2). Unlike the
// cancel pair above, a switch asks for no upstream action at all: the device
// detaches and the cloud keeps running the session. The UI thread sets the
// request along with the follow-up it wants next; the streaming worker records
// the detach, and the tail of whichever stream was open honours both.
//
// The follow-up is an existing WorkerCommand rather than a new kSwitch: the
// thing the user actually asked for is "open the picker" / "lock this session"
// / "observe this session", each of which already has a command. The switch is
// a modifier on it, not a command of its own -- so it reuses the existing
// dispatch and the existing ChainWorkerCommandLocked hand-over.
WorkerCommand g_switch_follow_up = WorkerCommand::kNone;
std::string g_switch_session_id;
std::atomic<bool> g_switch_requested{false};
std::atomic<bool> g_switch_delivered{false};
// [voice-pipe] Hard-abort request for the capture phases. Set by
// CancelAgentVoiceInput (UI thread); consumed by the kCancelVoice worker
// command -- or, when the capture worker is still bringing the WS turn up
// inside kPrepareCapture, by that handler's own check after the turn exists.
bool g_voice_abort_requested = false;
// Last ASR error message from the agent voice stream, stashed by
// OnAgentVoiceSse for the worker's error branch. Reset at the start of every
// capture so a stale message can never be reported for a new turn.
std::string g_voice_last_error;
wqn::runtime::SleepLease g_agent_sleep_lease;
wqn::services::ConnectivityDemand g_connectivity_demand;
// [agent] Set while an observe stream is attached and has seen a frame that
// proves the watched session has work in flight (a delta, a segment, a tool, an
// ask). It exists because the other two run-in-flight signals cannot learn this
// one: the picker snapshot is a fixed point in time, and the device's own run
// is by definition not an observe. Without it, a session that was idle when the
// list was read and started running while the device sat on it would never be
// seen as running again until the picker was reopened.
bool g_observed_run_live = false;

void DiscardOutboundReplies();
// Declared up here: clearing a pending ask is part of every terminal status and
// of every list load, which run long before the definition below.
void ClearPendingPermissionLocked();
void ClearPendingQuestionLocked();
void ClearReplyFlightLocked();
void ClearDeferredQuestionLocked();
void PromoteDeferredQuestionLocked();
void ClearAllAsksLocked();
// Defined down with the other arm paths, but ArmSwitchLocked needs it from the
// top of the file, next to ChainWorkerCommandLocked.
esp_err_t AcquireAgentLeaseLocked();
// Same reason: FinishSwitchedStreamLocked clears the departed run's turn state
// so the view does not keep an open tool block the terminal frame would have
// closed.
void ResetAgentHistoryTurnLocked();
// The lease criterion lives down with ReleaseWorkOwnershipLocked, but
// OnOpenCodeEvent and every worker tail call it from far above its definition.
void RefreshAgentRunLeaseLocked();

void MarkChangedLocked()
{
    g_changed = true;
}

void SetPhaseLocked(wqn::AiFeaturePhase phase, const std::string& status)
{
    g_state.ui.phase = phase;
    g_state.ui.status_label = status;
    MarkChangedLocked();
}

void ReleaseWorkOwnershipLocked()
{
    g_connectivity_demand.Reset();
    g_agent_sleep_lease.Reset();
}

// [agent] The agent's sleep-lease criterion -- items B1 and B3 (D-lease) of
// doc/1005-opencode-bidi-gap-plan.md §0.1.
//
// B1 makes "a stream is attached" the default state of the AI page, so it can
// no longer be a reason to stay awake: an idle session's stream stays open for
// as long as nothing detaches it. The revision before this one acquired the
// lease on every entry point and only released it in a worker tail, so having
// the AI page open at all pinned the device awake -- the 60-second idle deep
// sleep the rest of the product is built around never ran once a session was
// locked.
//
// The criterion is therefore NOT "a stream is attached" and NOT "the AI page is
// open". It is "a run this device can still see is in flight", from three
// sources:
//
//   1. the device's own submitted run, from its first frame to its terminal one;
//   2. the gateway's `outcome` for the session being watched, which it computed
//      from two upstream reads and which contract 2.2 now delivers with the
//      list (see OpenCodeSessionOutcome);
//   3. a frame on the attached observe stream that carries actual work.
//
// (2) is a snapshot and cannot learn that a session which was idle when the
// list was read has since started running -- which is exactly the case where
// the device is sitting on an attached stream with nothing else to look at.
// (3) closes that hole from the stream itself, at the cost of no request.
//
// Sources (2) and (3) are both gated on a stream being attached, and that gate
// is what bounds the snapshot. Without it a `running` row read when the picker
// opened would keep claiming a run in flight after the stream that justified it
// had gone, and the lease would be held for the rest of the session's life on a
// session nobody is watching -- the same stuck-awake failure B3 exists to end.
// Re-reading the picker is the only way to refresh the claim, which is the
// honest cost: it costs one request and it happens whenever the user looks.
//
// Two non-run cases are held as well, because they are agent work in progress
// rather than agent work observable: a voice capture owns the codec and an ASR
// turn, and a transcript backfill chained behind a finished run was explicitly
// handed the lease by the tail that armed it. Both clear in seconds.
//
// The failing direction is the one worth stating. kUnknown and an absent
// `outcome` both mean NO run in flight, so an unrecognised or missing value can
// only ever release the lease EARLY: the run keeps executing in the cloud
// untouched, and the next lock re-reads the list. Reading either as running
// would hold the lease on a session that is merely fresh, and the device would
// never sleep again -- the exact failure this function was written to end.
bool AgentRunInFlightLocked()
{
    // A voice capture owns the codec and the ASR turn. This is the case the
    // lease was introduced for and it must not regress.
    if (g_state.ui.phase == wqn::AiFeaturePhase::kLoading ||
        g_state.ui.phase == wqn::AiFeaturePhase::kRecording ||
        g_state.ui.phase == wqn::AiFeaturePhase::kTranscribing) {
        return true;
    }
    // A brief-tier run's digest exists only in the history projection, so the
    // run tail chains a backfill behind itself and hands it the lease rather
    // than releasing it. Refreshing means that handoff happened.
    if (g_history_refresh) {
        return true;
    }
    // The device's own run. `g_observing` is what separates it from an observe:
    // both set stream_active, and only this one is the device's own work.
    if (!g_observing && g_state.stream_active) {
        return true;
    }
    // Everything below is a claim about somebody else's run, which this device
    // can only make while it is attached to that session's stream.
    if (!g_observing || !g_state.stream_active) {
        return false;
    }
    for (const wqn::AgentSessionOption& option : g_state.sessions) {
        if (option.id != g_state.current_session_id) {
            continue;
        }
        if (option.outcome == wqn::OpenCodeSessionOutcome::kRunning) {
            return true;
        }
    }
    return g_observed_run_live;
}

// [run-live] The one place an observe attach decides what it may claim, from the
// only evidence an attach has: the picker row the last list fetch wrote. No frame
// has been read yet, so stream_active and g_observing say only "a stream is being
// opened" -- they cannot answer "is a run in flight", and reading them as if they
// could is what put 长按=中止 on a settled session (the criterion this mirrors
// refuses exactly that conflation: see the !g_observing || !stream_active gate).
//
// kComplete rather than kIdle, because the device is looking at a conversation
// that already finished and kComplete is the terminal state whose owner sets
// 长按确认发起新任务. The long-press gesture itself is NOT gated on the phase --
// TryApplyAgentAiButtonEvent routes on AgentOptionModeFor + session_locked +
// stream_active -- so for the gesture this is only about what the band SAYS.
//
// It is not ONLY that, and the first version of this comment was wrong on
// exactly this point: AiFeatureCanStartVoiceInput (ai_feature.cpp) admits
// kIdle / kAwaitingConfirmation / kComplete / kError and EXCLUDES kRunning, so
// claiming kRunning on a session whose run had already finished also blocked
// voice input for as long as the claim stood. Returning kComplete here
// re-enables it, which is the correct reading -- nothing was running.
//
// The first frame that proves otherwise re-decides through the same criterion:
// a delta raises g_observed_run_live, a terminal status settles the turn, and
// both republish run_live through RefreshAgentRunLeaseLocked.
wqn::AiFeaturePhase ObserveAttachPhaseLocked()
{
    for (const wqn::AgentSessionOption& option : g_state.sessions) {
        if (option.id != g_state.current_session_id) {
            continue;
        }
        return option.outcome == wqn::OpenCodeSessionOutcome::kRunning
                   ? wqn::AiFeaturePhase::kRunning
                   : wqn::AiFeaturePhase::kComplete;
    }
    // No row for this session (a create, or a list that has not been read since
    // the lock). Absent is the same fail-safe direction every other unknown in
    // this file takes: claim nothing destructive.
    return wqn::AiFeaturePhase::kComplete;
}

// Aligns the lease with AgentRunInFlightLocked. Every agent path that used to
// hand ownership back unconditionally calls this instead, so there is exactly
// one place that answers the question and one comment explaining it.
void RefreshAgentRunLeaseLocked()
{
    // [run-live] Same question, second consumer. The lease above/below answers it
    // for the power coordinator; the bottom band's gesture hint answers it for the
    // user, and the renderer used to derive that hint from the phase instead. The
    // phase is a claim, this is the evidence behind it, and publishing both from
    // one function is what stops them drifting apart again.
    //
    // Only on a transition: MarkChangedLocked on every call would repaint the AI
    // frame once per tick for the whole time a session is merely open, and the
    // answer does not change while a stream is quiet.
    const bool live = AgentRunInFlightLocked();
    if (g_state.run_live != live) {
        g_state.run_live = live;
        MarkChangedLocked();
    }
    if (live) {
        AcquireAgentLeaseLocked();
        return;
    }
    g_agent_sleep_lease.Reset();
    // [agent] The connectivity demand goes too, and leaving it behind was a
    // power bug rather than a tidiness one. This function REPLACED
    // ReleaseWorkOwnershipLocked at six call sites, and that function released
    // BOTH resources:
    //
    //     void ReleaseWorkOwnershipLocked() {
    //         g_connectivity_demand.Reset();
    //         g_agent_sleep_lease.Reset();
    //     }
    //
    // ConnectivityDemand owns a runtime::SleepLease of its own
    // (services/connectivity_service.h:89) with no expiry -- only Reset(), its
    // destructor, or the move-assignment of the NEXT command's AcquireNetwork
    // releases it. So after the criterion refactor the first agent command left
    // a demand held for the rest of the boot: no deep sleep AND no light sleep
    // (the demand takes the ESP-PM NO_LIGHT_SLEEP lock), with nothing on screen
    // to explain it, repairable only by an action that still reaches a surviving
    // ReleaseWorkOwnershipLocked -- the picker list or a session create.
    //
    // Releasing here is safe for the same reason the original call sites were:
    // a chained follow-up re-acquires. Every worker handler starts with
    // LoadToken + AcquireNetwork, and AcquireNetwork move-assigns a fresh
    // demand, so the kLoadHistory / kObserveSession chains take their own
    // before they need it. Both sibling tiers release their demand in their
    // stream tail (ai_session.cpp, flash_session.cpp) for the same reason.
    g_connectivity_demand.Reset();
}

// Rewrites the watched session's row in the picker snapshot to what the stream
// just proved about it, and drops the stream's own evidence.
//
// Without this the snapshot keeps saying `running` after the run it described
// has ended, and source (2) of the criterion above reads that as "a run is in
// flight" for as long as the list goes unread -- the device then never sleeps
// again, which is the same failure as the one B3 was written to end. The
// stream is the authority while it is attached; this is the moment it hands
// that authority back.
//
// `running` maps to kUnknown rather than kSucceeded on purpose: a terminal
// status can end in a failure (an `agent.error` frame arrives as error + idle),
// and claiming a success the device never saw would be a lie in the picker.
void SettleWatchedSessionOutcomeLocked()
{
    for (wqn::AgentSessionOption& option : g_state.sessions) {
        if (option.id != g_state.current_session_id) {
            continue;
        }
        if (option.outcome == wqn::OpenCodeSessionOutcome::kRunning) {
            option.outcome = wqn::OpenCodeSessionOutcome::kUnknown;
        }
    }
    g_observed_run_live = false;
}

void SetErrorLocked(const std::string& message)
{
    g_state.ui.phase = wqn::AiFeaturePhase::kError;
    g_state.ui.status_label = "错误";
    g_state.ui.activity_text = message;
    g_state.ui.action_hint = "长按确认重新录音";
    g_state.ui.requires_confirmation = false;
    g_state.confirmation_armed_at_ms = 0;
    g_state.stream_active = false;
    // Every SetErrorLocked call site is turn-terminal or pre-turn, and the
    // stream that would carry a reply is gone: an ask left armed here can never
    // be answered and would hold the option bar against the next turn's asks.
    ClearAllAsksLocked();
    MarkChangedLocked();
    // B3: an error ends this device's view of the run, not the run. The
    // criterion decides what is left to wait for -- and the answer it used to
    // give unconditionally ("release everything") is what pinned the device
    // awake on a session that had been idle for an hour.
    RefreshAgentRunLeaseLocked();
}

bool ArmWorkerLocked(WorkerCommand command)
{
    if (g_command != WorkerCommand::kNone || g_worker == nullptr) {
        return false;
    }
    g_command = command;
    xTaskNotifyGive(g_worker);
    return true;
}

// Chains the next command from inside the worker command that is running now.
// ArmWorkerLocked cannot be used for this: it refuses while g_command is still
// occupied by the running command (WorkerTask only clears it after the handler
// returns), so the history->observe chain used to fall through to its error
// branch on every single observe. The worker is the only caller, which is why
// no g_worker null check is needed.
void ChainWorkerCommandLocked(WorkerCommand command)
{
    g_command = command;
    xTaskNotifyGive(g_worker);
}

// [agent] The entry-point half of a session switch (symptom 2). Called with
// g_lock held when the caller's own command slot is already taken, and returns
// ESP_OK only once the switch is armed -- the caller must NOT arm its own
// command in that case.
//
// Only the picker-open path switches today. The other attach entry points
// (lock / create / observe) perform their state transition *after* they acquire
// the slot, and their handlers do not repeat that transition, so switching them
// would leave the view on a session whose transcript still belongs to the
// previous one. See the comments at each of those call sites. Extending the
// switch to them means hoisting that transition out first, not calling this.
//
// A switch is possible only while a *stream* is attached, and the predicate has
// to name that exactly -- `g_state.stream_active` alone is not enough.
// ObserveOpenCodeSession raises it before arming a transcript backfill
// (kLoadHistory, a bounded read that lasts seconds), so during a backfill it
// looks identical to an attached stream. But only RunPrompt and ObserveSession
// consume a switch: LoadHistory never calls FinishSwitchedStreamLocked, so a
// request armed during a backfill would sit unconsumed with the worker idle,
// and then detach the NEXT observe the user starts while chaining its stale
// follow-up. Guarding on the command that owns the slot rules that out, and
// `stream_active` stays in the test because the tail that consumes the request
// clears it: with both set, the read loop is provably still inside
// ReadAgentEventStream and the tail has not run yet.
//
// A bounded read (a transcript backfill, voice capture) finishes on its own
// within seconds and must not be swapped out from under itself anyway: the read
// it replaced would still be writing into the state the follow-up is about to
// rebuild, and nothing in that chain checks for it.
//
// The lease and the connectivity demand are NOT released here. The follow-up is
// itself network work, so they are handed over rather than dropped and
// re-acquired -- releasing first would leave a window where the device could
// sleep out from under the very request that is replacing the run it just left.
// AcquireAgentLeaseLocked is a no-op when the departing stream already holds it.
esp_err_t ArmSwitchLocked(WorkerCommand follow_up, const std::string& session_id,
                          wqn::OpenCodeRejectReason* reason)
{
    const bool stream_attached =
        (g_command == WorkerCommand::kRunPrompt ||
         g_command == WorkerCommand::kObserveSession) &&
        g_state.stream_active;
    if (!stream_attached) {
        if (reason != nullptr) {
            *reason = wqn::OpenCodeRejectReason::kWorkerBusy;
        }
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = AcquireAgentLeaseLocked();
    if (result != ESP_OK) {
        if (reason != nullptr) {
            *reason = wqn::OpenCodeRejectReason::kLeaseBusy;
        }
        return result;
    }
    g_switch_follow_up = follow_up;
    g_switch_session_id = session_id;
    g_switch_delivered.store(false, std::memory_order_release);
    g_switch_requested.store(true, std::memory_order_release);
    // The stream can be idle for up to kAgentStreamIdleReadTimeoutMs before the
    // worker notices, so say something now rather than leaving the departing
    // run's label on screen looking like nothing happened.
    g_state.ui.status_label = "正在切换";
    g_state.ui.activity_text = "已离开当前 Session，云端任务继续运行";
    MarkChangedLocked();
    return ESP_OK;
}

// [agent] The worker half of a session switch. Called with g_lock held from the
// tail of whichever stream was open, and returns true when it handled the end
// -- the caller must then skip every other terminal branch.
//
// Ownership deliberately survives: the chained follow-up is the next network
// operation and it takes the lease over from here, exactly as the
// history->observe chain already does. ReleaseWorkOwnershipLocked is the
// follow-up's job.
//
// `g_switch_requested` alone is enough to honour the switch. The stream loop
// sets `g_switch_delivered` when it sees the request, but a request that lands
// after the last read has already returned would otherwise leave the follow-up
// armed and never run -- the caller would watch nothing happen. Both streams
// only ever arm this while `stream_active` is true, and both that flag and this
// handshake are written under the same lock, so there is no window in which a
// request is raised for a stream that has already finished.
bool FinishSwitchedStreamLocked()
{
    if (!g_switch_requested.load(std::memory_order_acquire)) {
        return false;
    }
    g_switch_requested.store(false, std::memory_order_release);
    g_switch_delivered.store(false, std::memory_order_release);
    const WorkerCommand follow_up = g_switch_follow_up;
    g_switch_follow_up = WorkerCommand::kNone;
    // The departing stream is gone, so every ask it raised is dead and the
    // outbound queue that would carry its replies is discarded by the caller.
    ClearAllAsksLocked();
    g_observing = false;
    // The stream we just left is the only thing that could still have proven a
    // run was live, and it is gone. Its evidence goes with it: the follow-up
    // that inherits this state decides its own lease from its own facts.
    g_observed_run_live = false;
    g_state.stream_active = false;
    g_state.ui.phase = wqn::AiFeaturePhase::kLoading;
    g_state.ui.status_label = "正在切换";
    g_state.ui.activity_text = "已离开当前 Session，云端任务继续运行";
    g_state.ui.action_hint.clear();
    g_state.ui.response_text.clear();
    g_state.ui.scroll_offset_lines = 0;
    // Hand the target over the same way every other command does: the worker
    // reads it from g_run_session_id, so the tail's own clear must not run. The
    // one current follow-up (kLoadSessions) ignores it, and the clear is what
    // stops the departed run's session id from lingering as the target.
    g_run_session_id = g_switch_session_id;
    g_switch_session_id.clear();
    // The departed session's view state is cleared HERE rather than left to the
    // follow-up, because the follow-up is not obliged to succeed. kLoadSessions
    // clears all of this on its success path but its failure path only calls
    // SetErrorLocked, so a list load that 5xx's would otherwise drop the user
    // straight back onto the session they just left -- locked view, that
    // session's transcript still in the channel, and a tool block the terminal
    // frame that would have closed it never arrived. Clearing it here means the
    // outcome is "no session selected" whether or not the list arrives.
    g_state.session_locked = false;
    g_state.current_session_id.clear();
    g_state.current_session_title.clear();
    g_state.ui.context_label.clear();
    g_state.ui.prompt_text.clear();
    g_state.ui.requires_confirmation = false;
    g_state.confirmation_armed_at_ms = 0;
    g_state.history_loaded_session_id.clear();
    wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).Clear();
    ResetAgentHistoryTurnLocked();
    MarkChangedLocked();
    if (follow_up == WorkerCommand::kNone) {
        // [agent] D-lease: a switch with nothing to switch to. This is what
        // leaving the AI page raises -- detach from the stream, ask upstream to
        // stop nothing, and do not chain another command. Every other follow-up
        // is network work that inherits the lease; there is none here, so the
        // handover the comment above describes would otherwise leave the lease
        // held by an idle worker with no stream and no reason.
        //
        // The phase must be settled BEFORE the criterion reads it, and that is
        // not a detail. This function sets kLoading nine lines above as the
        // switching spinner, and the criterion's FIRST branch answers "a run is
        // in flight" for kLoading -- so the criterion read true here, acquired
        // the lease, and nothing ever released it: ChainWorkerCommandLocked(kNone)
        // arms no handler, so the kLoading phase survived for the rest of the
        // boot and every later call answered true. Leaving the AI page was
        // therefore the one gesture that pinned the device awake forever --
        // exactly what this whole criterion was written to stop, arriving
        // through the comment above it claiming the criterion "now reads no".
        // That comment only reasoned about stream_active and g_observing, which
        // this function does clear, and never looked at the phase it set.
        //
        // kIdle is the settled value the terminal paths use, and there is no
        // stream to be loading. The label settles with it: "正在切换" describes
        // work this path does not do, and leaving it beside kIdle would be the
        // same category of lie item C4 of the plan removed.
        g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
        g_state.ui.status_label = "已离开 Session";
        g_state.ui.activity_text = "云端任务继续运行";
        g_state.ui.action_hint.clear();
        RefreshAgentRunLeaseLocked();
    }
    ChainWorkerCommandLocked(follow_up);
    return true;
}

esp_err_t LoadToken(std::string* token)
{
    if (token == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = wqn::LoadAccessToken(token);
    if (result != ESP_OK || !wqn::IsValidAccessToken(*token)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t AcquireNetwork()
{
    wqn::services::ConnectivityDemand demand =
        wqn::services::AcquireConnectivityDemand(
            wqn::services::ConnectivityDemandReason::kAiInteractive,
            "opencode-agent",
            __FILE__,
            __LINE__);
    if (!demand) {
        return ESP_ERR_INVALID_STATE;
    }
    const wqn::services::ConnectivityDemandTicket ticket = demand.ticket();
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_connectivity_demand = std::move(demand);
    xSemaphoreGive(g_lock);
    return wqn::services::ConnectivityWaitResultToEspErr(
        wqn::services::WaitForConnectivity(ticket, kConnectivityWait));
}

void FinishCommand(WorkerCommand completed)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_command == completed) {
        g_command = WorkerCommand::kNone;
    }
    xSemaphoreGive(g_lock);
}

void LoadSessions()
{
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    std::vector<wqn::OpenCodeSessionInfo> sessions;
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        result = wqn::ListOpenCodeSessions(token, &sessions, &api_result);
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result == ESP_OK) {
        g_state.sessions.clear();
        g_state.sessions.reserve(sessions.size());
        for (wqn::OpenCodeSessionInfo& source : sessions) {
            g_state.sessions.push_back(wqn::AgentSessionOption{
                std::move(source.id), std::move(source.title), source.updated_at,
                source.outcome});
        }
        if (g_state.sessions.empty()) {
            SetErrorLocked("没有可用的 OpenCode Session");
        } else {
            g_state.selected_session = std::min(
                g_state.selected_session, g_state.sessions.size() - 1);
            g_state.session_locked = false;
            g_state.current_session_id.clear();
            g_state.current_session_title.clear();
            ClearAllAsksLocked();
            // A fresh list means no session's transcript is on screen any more,
            // so the next observe must backfill again.
            g_state.history_loaded_session_id.clear();
            g_observing = false;
            g_observed_run_live = false;
            g_state.ui.context_label.clear();
            g_state.ui.prompt_text.clear();
            g_state.ui.response_text.clear();
            g_state.ui.scroll_offset_lines = 0;
            g_state.ui.requires_confirmation = false;
            g_state.confirmation_armed_at_ms = 0;
            g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
            g_state.ui.status_label = "选择 Session";
            g_state.ui.activity_text = "上下选择，确认锁定";
            g_state.ui.action_hint = "↑/↓ 选择 · 确认锁定 · 长按新建";
            // [run-live] This is the only full rewrite of the picker rows, so it
            // is the only place outside a stream that can flip run_live -- the
            // row that seeded it may have settled while the list was stale, or a
            // row this device was attached to may have started running. Marked
            // inside the publish, on transition only.
            RefreshAgentRunLeaseLocked();
            MarkChangedLocked();
            ReleaseWorkOwnershipLocked();
        }
    } else {
        SetErrorLocked(api_result.detail.empty() ? "Session 列表加载失败" : api_result.detail);
    }
    xSemaphoreGive(g_lock);
}

// ---- Voice pipe (WS turn) --------------------------------------------------
// [voice-pipe] The Agent capture rides the shared Std/Pro WebSocket with
// tier=agent: the capture tap (owned by ai_session, armed once in InitAiSession)
// forwards PCM into stdpro_ws, which drops it until a turn reaches kRecording.
// agent_voice_pipe owns the turn itself; these helpers bridge it to the worker
// and the UI.

// UI-side observer for the live voice stream. Runs on the transport task, so
// every state write takes g_lock. The module's trampoline already accumulates
// the transcript for the worker; this only shapes what the screen shows while
// the turn is live.
void OnAgentVoiceSse(const wqn::WqnAiSseEvent& ev, void* /*user_ctx*/)
{
    using Kind = wqn::WqnAiSseEvent::Kind;
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const wqn::AiFeaturePhase phase = g_state.ui.phase;
    const bool capturing = phase == wqn::AiFeaturePhase::kRecording ||
                           phase == wqn::AiFeaturePhase::kTranscribing;
    switch (ev.kind) {
        case Kind::kAsrDelta: {
            if (!capturing) {
                break;
            }
            g_state.ui.voice_partial += ev.delta;
            // Same delta coalescing as the STD consumer (ai_session.cpp
            // OnSseEvent): every change mark costs a full state copy on the UI
            // task, and deltas land far faster than the panel can repaint.
            static int64_t s_last_delta_mark_ms = -1000;
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - s_last_delta_mark_ms >= 50) {
                s_last_delta_mark_ms = now_ms;
                MarkChangedLocked();
            }
            break;
        }
        case Kind::kAsrComplete:
            if (capturing) {
                g_state.ui.voice_partial = ev.text;
                MarkChangedLocked();
            }
            break;
        case Kind::kAsrFailed:
            g_voice_last_error = ev.error_message.empty()
                ? std::string("语音识别失败")
                : ev.error_message;
            break;
        case Kind::kError:
            g_voice_last_error = ev.error_message.empty() ? ev.error_code
                                                          : ev.error_message;
            break;
        default:
            break;
    }
    xSemaphoreGive(g_lock);
}

// Bring up the WS turn that carries this capture. Called from the capture
// worker after the microphone is running; every failure is non-fatal --
// Transcribe() falls back to the batch endpoint when no turn exists.
void StartVoiceTurn(const std::string& token)
{
    const std::string request_id = wqn::GenerateRequestId();
    uint32_t turn_gen = 0;
    const esp_err_t result = wqn::AgentVoiceTurnStart(token, request_id, &turn_gen);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "agent voice turn not started (%s); batch fallback armed",
                 esp_err_to_name(result));
    }
}

// Hard abort: drop the captured audio, tear the WS turn down (a no-op when it
// was never established) and release everything the capture acquired. The UI
// half (idle label) is set by CancelAgentVoiceInput, which is the only caller.
void TearDownVoiceCapture()
{
    wqn::AgentVoiceTurnAbort();
    wqn::AudioCaptureChunk discarded;
    wqn::StopAudioCapture(&discarded);
    wqn::ReleaseAudioCapturePower();

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_recording_requested = false;
    g_voice_abort_requested = false;
    g_state.ui.voice_partial.clear();
    ReleaseWorkOwnershipLocked();
    xSemaphoreGive(g_lock);
}

void PrepareCapture()
{
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    bool should_capture = false;
    bool mic_busy = false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    should_capture = result == ESP_OK && g_recording_requested;
    xSemaphoreGive(g_lock);
    if (should_capture) {
        // [voice-pipe] The Agent capture rides the shared Std/Pro WebSocket
        // (tier=agent), so the capture tap STAYS installed: it forwards PCM
        // into stdpro_ws, which drops it until the turn reaches kRecording.
        // The tap is armed once in InitAiSession and nothing may disable it
        // here -- that is what keeps this capture out of the batch-only path.
        if (wqn::IsAudioCaptureRunning()) {
            mic_busy = true;
            result = ESP_ERR_INVALID_STATE;
        } else {
            result = wqn::StartAudioCapture();
        }
    }
    bool capture_started = false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result != ESP_OK) {
        SetErrorLocked(mic_busy
            ? "麦克风被其他功能占用"
            : (result == ESP_ERR_INVALID_STATE
                ? "设备未配对或网络不可用"
                : "录音启动失败"));
    } else if (!g_recording_requested) {
        wqn::AudioCaptureChunk discarded;
        xSemaphoreGive(g_lock);
        wqn::StopAudioCapture(&discarded);
        wqn::ReleaseAudioCapturePower();
        xSemaphoreTake(g_lock, portMAX_DELAY);
        if (!g_state.ui.prompt_text.empty() && g_state.ui.requires_confirmation) {
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingConfirmation;
            g_state.ui.status_label = "确认后发送";
            g_state.ui.activity_text = "追加录音已取消，原转写尚未执行";
            g_state.ui.action_hint = "↑ 发送 · ↓ 取消 · 长按确认追加";
            g_state.confirmation_armed_at_ms = esp_timer_get_time() / 1000;
        } else {
            g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
            g_state.ui.status_label = "已取消录音";
        }
        MarkChangedLocked();
        ReleaseWorkOwnershipLocked();
    } else {
        g_state.ui.phase = wqn::AiFeaturePhase::kRecording;
        g_state.ui.status_label = "录音中";
        g_state.ui.activity_text = "松开确认键开始转写";
        g_state.ui.action_hint = "松开确认键停止";
        MarkChangedLocked();
        capture_started = true;
    }
    xSemaphoreGive(g_lock);

    if (!capture_started) {
        return;
    }
    // Bring the WS turn up OUTSIDE the lock: EnsureConnected can block for
    // seconds and must never hold the state mutex. The turn belongs to this
    // capture -- Transcribe() finalizes it, and falls back to the batch
    // endpoint when it was never established.
    wqn::AgentVoiceRegisterDeltaCallback(&OnAgentVoiceSse, nullptr);
    StartVoiceTurn(token);

    // [voice-pipe] A Down-cancel that landed while this worker owned the
    // command slot could not arm kCancelVoice; honor it here, now that the
    // turn exists, so no half-open turn is left behind.
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool abort_requested = g_voice_abort_requested;
    xSemaphoreGive(g_lock);
    if (abort_requested) {
        TearDownVoiceCapture();
    }
}

void Transcribe()
{
    wqn::AudioCaptureChunk audio;
    esp_err_t result = wqn::StopAudioCapture(&audio);
    if (result == ESP_OK && (audio.empty() || audio.duration_ms < 1000)) {
        result = ESP_ERR_INVALID_SIZE;
    }
    std::string token;
    if (result == ESP_OK) {
        result = LoadToken(&token);
    }
    std::string transcript;
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        // [voice-pipe] A live WS turn carries this clip: FINAL hands the audio
        // over and the ASR text comes back from the stream. When no turn was
        // established (connect/start failed, or the clip never left the
        // device), ESP_ERR_INVALID_STATE means the batch endpoint is the only
        // safe path. A soft cancel returns ESP_OK with an empty transcript and
        // is handled by the cancel branch below.
        const esp_err_t ws_result = wqn::AgentVoiceTurnFinalize(
            audio.duration_ms, kAgentVoiceFinalizeTimeoutMs, &transcript);
        if (ws_result == ESP_ERR_INVALID_STATE) {
            result = wqn::TranscribeOpenCodeAudio(token, audio, &transcript, &api_result);
        } else {
            result = ws_result;
        }
    }
    wqn::ReleaseAudioCapturePower();

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_recording_requested = false;
    const bool cancelled = wqn::AgentVoiceCancelRequested();
    wqn::AgentVoiceClearCancel();
    g_state.ui.voice_partial.clear();
    if (cancelled) {
        // [voice-pipe] The UI already flipped to idle when the user cancelled;
        // there is nothing to commit and nothing to report. The capture's
        // network demand and sleep lease still have to go back, though.
        ReleaseWorkOwnershipLocked();
    } else if (result == ESP_OK &&
        g_state.ui.prompt_text.size() + transcript.size() +
                (g_state.ui.prompt_text.empty() ? 0 : 1) >
            kMaxPromptBytes) {
        SetErrorLocked("转写内容过长，请取消后缩短输入");
    } else if (result == ESP_OK) {
        if (!g_state.ui.prompt_text.empty()) {
            g_state.ui.prompt_text += "\n";
        }
        g_state.ui.prompt_text += transcript;
        g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingConfirmation;
        g_state.ui.status_label = "确认后发送";
        g_state.ui.activity_text = "语音已转写，尚未执行";
        g_state.ui.action_hint = "↑ 发送 · ↓ 取消 · 长按确认追加";
        g_state.ui.requires_confirmation = true;
        g_state.confirmation_armed_at_ms = esp_timer_get_time() / 1000;
        MarkChangedLocked();
        ReleaseWorkOwnershipLocked();
    } else if (result == ESP_ERR_INVALID_SIZE) {
        SetErrorLocked("录音过短或未检测到语音");
    } else {
        // The stream's error text (when one arrived) beats the generic label;
        // the batch path has its own detail in api_result.
        const std::string& detail = !api_result.detail.empty()
            ? api_result.detail
            : g_voice_last_error;
        SetErrorLocked(detail.empty() ? "语音转写失败" : detail);
    }
    xSemaphoreGive(g_lock);
}

// ---- History mirror --------------------------------------------------------
// [agent] The Agent tier is rendered by the AI page's chat-bubble viewport, so
// every stream event that changes what the user reads is mirrored into the
// kAgent history channel. The channel is chosen explicitly rather than read
// from the visible tier: a run started on one tier must never append to
// another tier's conversation. Everything below runs with g_lock held.

// The renderer draws a thinking bubble from the message kind alone; the marker
// prefix is the caller's, matching ai_session.cpp / flash_session.cpp.
std::string AgentThinkingLabel(const std::string& text)
{
    return text.empty() ? std::string() : std::string("\xE2\x9C\x8D ") + text;
}

wqn::ChatMessageId g_agent_assistant_id = wqn::kInvalidChatMessageId;
wqn::ChatMessageId g_agent_tool_id = wqn::kInvalidChatMessageId;
wqn::ChatMessageId g_agent_thinking_id = wqn::kInvalidChatMessageId;
std::string g_agent_tool_name;
// Call id of the open block, when the gateway sends one. See the kTool branch.
std::string g_agent_tool_call;
// Reasoning has its own channel and its own buffer. It is deliberately NOT
// accumulated into `g_state.ui.response_text`: even if the gateway mapped a
// reasoning frame as `agent.text`, the answer the user reads would stay clean.
std::string g_agent_thinking_text;
// Whether the open tool block succeeded. The gateway's `agent.tool` status is
// the only signal available; `error` is the failure case.
bool g_agent_tool_ok = true;
// Last status/preview seen for the open tool block. The gateway's `agent.tool`
// event carries a free-form status string, not an explicit start/end pair, so
// the newest detail wins and the block is closed by whatever arrives next.
std::string g_agent_tool_detail;
int64_t g_agent_tool_since_ms = 0;

void ResetAgentHistoryTurnLocked()
{
    g_agent_assistant_id = wqn::kInvalidChatMessageId;
    g_agent_tool_id = wqn::kInvalidChatMessageId;
    g_agent_thinking_id = wqn::kInvalidChatMessageId;
    g_agent_tool_name.clear();
    g_agent_tool_call.clear();
    g_agent_thinking_text.clear();
    g_agent_tool_detail.clear();
    g_agent_tool_since_ms = 0;
    g_agent_tool_ok = true;
}

// [follow] Arm the Agent viewport follow for a fresh turn. Every turn-start
// path (submit, permission/question reply, voice capture, observe) calls this
// so the viewport watches the run: the per-tick step
// (UiRuntime::DispatchAiViewportFollow) pins it to the live tail while the run
// executes and retires the follow -- parking the viewport on the answer's first
// line -- as soon as a non-empty answer body lands. `user_moved=false` marks
// this turn's viewport as untouched, which is what the follow step needs to
// decide whether it may move it at all.
//
// Caller must hold g_lock; the caller's own MarkChangedLocked() covers the
// state change (the follow flags are read from the same snapshot).
void ArmAgentFollowLocked()
{
    g_state.follow_active = true;
    g_state.user_moved = false;
}

// Closes the open tool placeholder into a result block. Because the gateway
// has no tool-end event, this is driven by the next event of any kind (text, a
// different tool, run end, error) instead of by guessing the status vocabulary
// -- that keeps exactly one block per tool and never leaks a "running"
// placeholder into the rendered history.
void CloseAgentToolBlockLocked(bool ok, int64_t now_ms)
{
    if (g_agent_tool_id == wqn::kInvalidChatMessageId) {
        return;
    }
    const std::string name = g_agent_tool_name;
    const std::string detail = g_agent_tool_detail;
    const int64_t since_ms = g_agent_tool_since_ms;
    wqn::AiHistory& history = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent);
    if (history.PopLastIf(wqn::ChatMessageKind::kToolStart)) {
        const int32_t elapsed_ms =
            (since_ms > 0 && now_ms >= since_ms) ? static_cast<int32_t>(now_ms - since_ms) : 0;
        history.AppendToolResult(name, std::string_view(), detail, ok, elapsed_ms, now_ms);
    }
    g_agent_tool_id = wqn::kInvalidChatMessageId;
    g_agent_tool_name.clear();
    g_agent_tool_call.clear();
    g_agent_tool_detail.clear();
    g_agent_tool_since_ms = 0;
    // Post-tool text must open a NEW assistant entry. Rewriting the pre-tool
    // entry in place would drag it below the tool block and overwrite its own
    // text with the post-tool text.
    g_agent_assistant_id = wqn::kInvalidChatMessageId;
    // Same for the thinking channel, and this is fixing a live bug rather than
    // adding a feature. The comment in MirrorAgentThinkingLocked below claims
    // "Post-tool reasoning must open a NEW entry for the same reason post-tool
    // text does (see CloseAgentToolBlockLocked)" -- but this function never
    // retired the thinking id, so the claim was false: post-tool reasoning fell
    // through to ReplaceText and overwrote the pre-tool thinking bubble in
    // place. Any run with detail >= 2 that reasons again after a tool call hit
    // it, and nothing covered the case.
    //
    // Retiring the id here is safe for the same reason retiring the assistant id
    // is: the mirror functions call this BEFORE they bind the text they are
    // about to write, and an id is not that text. Clearing the id cannot erase a
    // frame that is already on its way -- it only decides which entry the next
    // write lands in.
    g_agent_thinking_id = wqn::kInvalidChatMessageId;
}

// Applies one agent text/reasoning frame to the live channels. The policy in
// agent_round_policy.h decides what happens; this function performs it against
// the real AiHistory, and it is the only place in the firmware that turns a
// policy flag into a history write. The host matrix in
// test/agent_round_policy_test.cpp exercises the policy half.
//
// Caller must hold g_lock.
void ApplyAgentRoundFrameLocked(const wqn::AgentRoundFrame& frame, int64_t now_ms)
{
    bool assistant_open = (g_agent_assistant_id != wqn::kInvalidChatMessageId);
    bool thinking_open = (g_agent_thinking_id != wqn::kInvalidChatMessageId);
    bool tool_open = (g_agent_tool_id != wqn::kInvalidChatMessageId);
    const wqn::AgentRoundOps ops = wqn::ApplyAgentRoundFrame(
        g_state.ui.response_text, g_agent_thinking_text, assistant_open, thinking_open,
        tool_open, frame,
        wqn::AgentRoundBudgets{g_run_detail, wqn::kMaxAgentTextBytes, wqn::kMaxThinkingBytes});

    // Order matters: the close writes the tool result, and it retires both
    // entries, so it runs before any history write below and the id bookkeeping
    // catches up with it before the Append/Replace chooses an entry.
    if (ops.close_tool_block) {
        CloseAgentToolBlockLocked(g_agent_tool_ok, now_ms);
    }
    if (!assistant_open) {
        g_agent_assistant_id = wqn::kInvalidChatMessageId;
    }
    if (!thinking_open) {
        g_agent_thinking_id = wqn::kInvalidChatMessageId;
    }

    wqn::AiHistory& history = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent);
    if (ops.append_assistant) {
        g_agent_assistant_id = history.AppendAssistant(g_state.ui.response_text, now_ms);
    } else if (ops.replace_assistant) {
        history.ReplaceText(g_agent_assistant_id, wqn::ChatMessageKind::kAssistant,
                            g_state.ui.response_text, now_ms);
    }
    if (ops.append_thinking) {
        g_agent_thinking_id =
            history.AppendThinking(AgentThinkingLabel(g_agent_thinking_text), now_ms);
    } else if (ops.replace_thinking) {
        history.ReplaceText(g_agent_thinking_id, wqn::ChatMessageKind::kThinking,
                            AgentThinkingLabel(g_agent_thinking_text), now_ms);
    }
}

// Records the submitted prompt and arms a fresh turn. Called before the worker
// starts so the user bubble is on screen while the gateway is still connecting.
void AppendAgentUserLocked(std::string_view text, int64_t now_ms)
{
    ResetAgentHistoryTurnLocked();
    if (!text.empty()) {
        wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).AppendUser(text, now_ms);
    }
}

void AppendAgentErrorLocked(std::string_view text, int64_t now_ms)
{
    CloseAgentToolBlockLocked(false, now_ms);
    g_agent_assistant_id = wqn::kInvalidChatMessageId;
    g_agent_thinking_id = wqn::kInvalidChatMessageId;
    g_agent_thinking_text.clear();
    if (!text.empty()) {
        wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).AppendAssistant(text, now_ms);
    }
}

// Drops a pending permission and the session that raised it. The two travel
// together: the reply route is scoped to the owning session, so keeping one
// without the other answers on whatever session happens to be attached next.
void ClearPendingPermissionLocked()
{
    g_state.pending_permission_id.clear();
    g_state.pending_permission_session.clear();
}

void ClearPendingQuestionLocked()
{
    g_state.pending_question_id.clear();
    g_state.pending_question_title.clear();
    g_state.pending_question_options.clear();
    g_state.pending_question_session.clear();
}

// Forget which reply is on the wire once the ask it answers is over. A stream
// that outlives the ask (a permission answered from the OpenCode side, a run
// that ended while a reply was in flight) must not let a later failure restore
// an ask the user already moved past.
void ClearReplyFlightLocked()
{
    g_reply_flight_permission_id.clear();
    g_reply_flight_question_id.clear();
}

// One question the device could not show yet, because a permission ask holds the
// option bar. It is promoted the moment that bar is free, so an ask that arrives
// behind another is answered rather than lost.
bool g_deferred_question_valid = false;
std::string g_deferred_question_id;
std::string g_deferred_question_title;
std::vector<wqn::OpenCodeQuestionOption> g_deferred_question_options;
// The session the deferred question came from, promoted with it below.
std::string g_deferred_question_session;

// Hand the deferred ask the option bar it has been waiting for. Only a
// permission reply calls this, because only a permission reply can free the bar:
// a question arm means the permission slot is already empty, and a terminal
// status clears the deferred ask instead of promoting it.
void PromoteDeferredQuestionLocked()
{
    if (!g_deferred_question_valid || !g_state.pending_permission_id.empty() ||
        !g_state.pending_question_id.empty()) {
        return;
    }
    g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingQuestion;
    g_state.ui.status_label = "等待回答";
    g_state.pending_question_id = std::move(g_deferred_question_id);
    g_state.pending_question_title = std::move(g_deferred_question_title);
    g_state.pending_question_options = std::move(g_deferred_question_options);
    g_state.pending_question_session = std::move(g_deferred_question_session);
    g_state.ui.activity_text = g_state.pending_question_title;
    g_state.ui.action_hint = "↑/↓ 选择 · 确认回答";
    g_deferred_question_valid = false;
    MarkChangedLocked();
}

void ClearDeferredQuestionLocked()
{
    g_deferred_question_valid = false;
    g_deferred_question_id.clear();
    g_deferred_question_title.clear();
    g_deferred_question_options.clear();
    g_deferred_question_session.clear();
}

// Drops every ask the device is holding: both slots, the deferred question and
// the in-flight reply bookkeeping. This is what a turn boundary needs -- an ask
// that outlives its run can never be answered, holds the option bar, and would
// otherwise swallow the next turn's first ask.
void ClearAllAsksLocked()
{
    ClearPendingPermissionLocked();
    ClearPendingQuestionLocked();
    ClearDeferredQuestionLocked();
    ClearReplyFlightLocked();
}

// Replays one session's backfilled turns into the kAgent channel, oldest first.
// The gateway already chose the window and truncated each field, so this is a
// direct projection: no dedupe against live deltas, because the caller resets
// the turn lock first and subscribes to the stream only afterwards.
void BackfillAgentHistoryLocked(const std::vector<wqn::OpenCodeHistoryMessage>& messages,
                                int64_t now_ms)
{
    wqn::AiHistory& history = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent);
    for (const wqn::OpenCodeHistoryMessage& message : messages) {
        if (message.role == "user") {
            if (!message.text.empty()) {
                history.AppendUser(message.text, now_ms);
            }
            continue;
        }
        if (message.role != "assistant") {
            continue;
        }
        if (!message.thinking.empty()) {
            history.AppendThinking(AgentThinkingLabel(message.thinking), now_ms);
        }
        if (!message.text.empty()) {
            history.AppendAssistant(message.text, now_ms);
        }
        for (const wqn::OpenCodeHistoryTool& tool : message.tools) {
            if (tool.name.empty()) {
                continue;
            }
            // Append-then-close keeps one finished block per call. The tool
            // placeholder has no separate start event on this path, so there is
            // no turn lock to maintain either.
            history.AppendToolStart(tool.name, std::string_view(), now_ms);
            history.PopLastIf(wqn::ChatMessageKind::kToolStart);
            history.AppendToolResult(tool.name, std::string_view(), tool.preview,
                                     tool.status != "error", 0, now_ms);
        }
    }
}

void OnOpenCodeEvent(const wqn::OpenCodeEvent& event, void*)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    switch (event.kind) {
        case wqn::OpenCodeEventKind::kAccepted:
            SetPhaseLocked(wqn::AiFeaturePhase::kRunning, "Agent 执行中");
            g_state.ui.activity_text = "OpenCode 已接收任务";
            break;
        case wqn::OpenCodeEventKind::kAttached:
            // [run-live] Not SetPhaseLocked(kRunning, ...) unconditionally any
            // more. kAttached is observe-only, and the attach already claimed the
            // phase the evidence supports (ObserveAttachPhaseLocked); re-claiming
            // kRunning here overwrote that with the same guess, which is why the
            // hint said 中止 on a settled session even after the attach was
            // corrected. Same predicate, so the two can no longer disagree.
            //
            // The label and the activity line are unconditional: they are what
            // this frame actually proves (the stream is connected).
            g_state.ui.status_label = "观察中";
            g_state.ui.activity_text = "已连接 Session 事件流";
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kStatus:
            // [P3c] A pending ask is only cleared by a terminal status. Clearing
            // it on every status frame disarmed a live ask whenever the gateway
            // emitted an incidental one (retry, compaction), and the device then
            // had no way to answer it -- the run blocked until the 30-minute
            // stream timeout and surfaced as stream_incomplete.
            if (event.status == "idle" || event.status == "error") {
                // Includes the deferred question: with the run over it can
                // never be promoted, and leaving it queued would arm a dead
                // ask on the next turn.
                ClearAllAsksLocked();
            }
            if (event.status == "idle") {
                g_state.stream_active = false;
                // Run finished: close a tool block the gateway never closed.
                CloseAgentToolBlockLocked(!g_run_failed, now_ms);
                if (!g_run_failed) {
                    SetPhaseLocked(wqn::AiFeaturePhase::kComplete, "执行完成");
                    if (g_observing) {
                        g_state.ui.status_label = "观察结束";
                        g_state.ui.activity_text = "无运行中任务或任务已结束";
                    }
                    // The observe hint is gone with the observe gesture: B1
                    // attaches by entering, so there is no double-press to
                    // offer. What is left is the ordinary capture, exactly as it
                    // reads on any session the device did not observe.
                    g_state.ui.action_hint = "长按确认发起新任务";
                }
            } else if (event.status == "error") {
                // A terminal error status: v2's gateway does not emit this
                // today (failures arrive as agent.error + idle), but the
                // contract allows it, and without this branch it fell into the
                // generic else below -- a failed run was relabelled
                // "Agent 执行中" and `g_run_failed` stayed false, so the
                // trailing idle then closed the turn as a SUCCESS.
                g_run_failed = true;
                g_state.stream_active = false;
                CloseAgentToolBlockLocked(false, now_ms);
                SetPhaseLocked(wqn::AiFeaturePhase::kError, "执行失败");
                // [B4] Retry, not record: the renderer prefers this hint when the
                // state's owner set one, so the gesture it draws is the one that
                // actually works in this phase.
                g_state.ui.action_hint = "长按确认重试新任务";
                if (!event.text.empty()) {
                    g_state.ui.activity_text = event.text;
                }
            } else if (event.status == "retry") {
                SetPhaseLocked(wqn::AiFeaturePhase::kRunning,
                               g_observing ? "观察中" : "Agent 重试中");
                g_state.ui.activity_text = event.text;
            } else {
                SetPhaseLocked(wqn::AiFeaturePhase::kRunning,
                               g_observing ? "观察中" : "Agent 执行中");
                // Status text (gateway hints, upstream status messages) is
                // transient context; the next tool/text event replaces it.
                if (!event.text.empty()) {
                    g_state.ui.activity_text = event.text;
                }
            }
            break;
        case wqn::OpenCodeEventKind::kTextDelta:
            ApplyAgentRoundFrameLocked(
                wqn::AgentRoundFrame{wqn::AgentRoundFrameKind::kTextDelta, event.text},
                now_ms);
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kText:
            // H1: the cloud sends an EMPTY `agent.text` as its "start this round
            // over" frame at every detail tier now, not just brief. Assigning an
            // empty string IS clearing the buffer, so the clear half was always
            // free; what was missing was retiring the assistant id. Without it,
            // the next round's delta lands on ReplaceText and writes into the
            // entry that is currently holding the previous round's answer, so
            // the two rounds end up stacked in one bubble instead of the new one
            // replacing it.
            //
            // Gated on the detail tier because that is what decides whether the
            // cloud emits this frame at all: brief tier has been sending it all
            // along, and A1/A2/A3 extended it to the tiers that render reasoning
            // and tool blocks -- the tiers where a round boundary is actually
            // visible as a splice. Reading `g_state.detail_level` here instead
            // would let a tier change made mid-run change this run's behaviour.
            //
            // Both halves of that (the clear and the retirement) now live in
            // agent_round_policy.h and are asserted as a pair by the host matrix,
            // which is why the code here is a single call.
            ApplyAgentRoundFrameLocked(
                wqn::AgentRoundFrame{wqn::AgentRoundFrameKind::kText, event.text},
                now_ms);
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kReasoningDelta:
            // Reasoning is bounded by its own budget, never by the answer's.
            ApplyAgentRoundFrameLocked(
                wqn::AgentRoundFrame{wqn::AgentRoundFrameKind::kReasoningDelta, event.text},
                now_ms);
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kReasoning:
            // A5/A6, the thinking channel's twin of A4. An empty `agent.reasoning`
            // is the cloud's "this round's thought starts over" frame: it is
            // emitted at a round boundary immediately before the new round's
            // reasoning, because `applyReasoningDelta` sees the boundary before
            // `applyDelta` does.
            //
            // Without retiring the id, the next reasoning delta lands on
            // ReplaceText and writes into the PREVIOUS round's thinking entry,
            // which sits above any tool block that ran in between. The text
            // would be right and the position wrong: round N's thought would
            // render where round N-1's used to be. Retiring the id is what makes
            // the next reasoning delta AppendThinking at the current tail instead.
            //
            // No detail gate here, unlike A4: reasoning only reaches the device
            // at detail >= 2 at all, because the cloud is what filters it, so
            // there is no tier at which this frame arrives unexpectedly.
            ApplyAgentRoundFrameLocked(
                wqn::AgentRoundFrame{wqn::AgentRoundFrameKind::kReasoning, event.text},
                now_ms);
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kTool: {
            g_state.ui.activity_text = event.tool;
            if (!event.status.empty()) {
                g_state.ui.activity_text += " · " + event.status;
            }
            if (!event.preview.empty()) {
                g_state.ui.activity_text += " · " + event.preview;
            }
            // Coalesce one tool call into one block. The key is the call id when
            // the gateway learned it: a run that reads two files re-emits the
            // same tool name twice, and merging by name alone collapsed both
            // into a single block whose detail was the last call's. When the id
            // is unknown (mid-run attach) fall back to the name, which is the
            // most a v0 conversation can distinguish.
            const bool same_call = g_agent_tool_id != wqn::kInvalidChatMessageId &&
                                   !event.call_id.empty() && g_agent_tool_call == event.call_id;
            const bool same_name = g_agent_tool_id != wqn::kInvalidChatMessageId &&
                                   event.call_id.empty() && g_agent_tool_name == event.tool;
            if (same_call || same_name) {
                // Same block: this status is the one it will be closed with.
                g_agent_tool_ok = event.status != "error";
                g_agent_tool_detail = event.preview.empty() ? event.status : event.preview;
            } else {
                // Close the previous block with the status it ended on. Setting
                // `g_agent_tool_ok` before this close marked a successful tool
                // as failed (and a failed one as successful) whenever two
                // different calls arrived back to back.
                CloseAgentToolBlockLocked(g_agent_tool_ok, now_ms);
                // [agent] Drop the accumulated answer text at a tool boundary.
                // A round change is already caught by the empty `agent.text` the
                // cloud sends (see the kText case), but that only fires between
                // model rounds: the cloud's round key is the assistant message
                // id alone, so "text part, then a tool, then another text part
                // inside the same message" is one round as far as it can tell.
                // The device appends every delta into one buffer, so without
                // this the second part's text is glued behind the first's.
                //
                // Deliberately here and NOT in CloseAgentToolBlockLocked, which
                // would be the tidier spot and is wrong: the mirror functions
                // call that close *after* they have written the current frame's
                // text into the buffer, so clearing there erases the very frame
                // that triggered the close -- and because `endText` replays a
                // whole part when it thinks a delta was lost, what gets eaten
                // can be an entire text part rather than one delta. A tool
                // frame carries no text of its own, so this is the one place
                // the clear cannot cost a frame.
                //
                // Safe to drop the accumulated text: every pre-tool delta has
                // already been mirrored into history, and the close above
                // retired the assistant id, so the next text delta can only
                // AppendAssistant a new entry. It cannot ReplaceText over the
                // pre-tool one, which is what makes this a clear rather than a
                // loss.
                //
                // The clear and the two retirements are AgentRoundToolBoundary
                // in agent_round_policy.h, so the host matrix covers the pair.
                bool assistant_open = (g_agent_assistant_id != wqn::kInvalidChatMessageId);
                bool thinking_open = (g_agent_thinking_id != wqn::kInvalidChatMessageId);
                wqn::AgentRoundToolBoundary(g_state.ui.response_text, assistant_open,
                                            thinking_open);
                // The close above retires both entries only when a block was
                // actually open, so this write-back is load-bearing for the
                // first tool frame of a run rather than being belt and braces.
                if (!assistant_open) {
                    g_agent_assistant_id = wqn::kInvalidChatMessageId;
                }
                if (!thinking_open) {
                    g_agent_thinking_id = wqn::kInvalidChatMessageId;
                }
                g_agent_tool_ok = event.status != "error";
                g_agent_tool_id = wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent)
                                      .AppendToolStart(event.tool, std::string_view(), now_ms);
                g_agent_tool_name = event.tool;
                g_agent_tool_call = event.call_id;
                g_agent_tool_detail = event.preview.empty() ? event.status : event.preview;
                g_agent_tool_since_ms = now_ms;
            }
            MarkChangedLocked();
            break;
        }
        case wqn::OpenCodeEventKind::kPermission:
            // The option bar has one mode, so a permission supersedes a question
            // the device is still holding. The gateway re-arms a question that
            // is still pending once this permission is answered (it re-reads
            // liveness every poll round), so dropping it here loses nothing --
            // and keeping it would leave both asks live at once, with the
            // question unanswerable behind the permission.
            ClearPendingQuestionLocked();
            ClearDeferredQuestionLocked();
            // Only the question flight: a question reply already on the wire
            // must not restore a question this permission has superseded. The
            // permission flight (if any) is a different reply.
            g_reply_flight_question_id.clear();
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingPermission;
            g_state.ui.status_label = "等待权限";
            g_state.pending_permission_id = event.permission_id;
            // Which session the ask belongs to, not which one we attached to. A
            // subagent's permission must be answered on the subagent's own id or
            // the reply 404s.
            g_state.pending_permission_session = event.session_id;
            g_state.ui.activity_text = event.text;
            if (!event.preview.empty()) {
                g_state.ui.activity_text += " · " + event.preview;
            }
            g_state.ui.action_hint = "↑ 批准 · ↓ 拒绝";
            // Deliberately NOT mirrored into history: the ask is a live
            // interaction rendered by the dashed bubble + option bar, and its
            // outcome already shows up as the tool blocks and text that follow.
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kQuestion:
            // A question and a permission can never be live at once: the option
            // bar has one mode, and the session state has one pending ask.
            // Deferring -- not dropping -- is what keeps the run alive: a dropped
            // ask is lost for good, because the gateway has already marked it
            // seen and will not re-send it, so the run sits behind a question the
            // user was never shown.
            if (!g_state.pending_permission_id.empty()) {
                if (g_deferred_question_valid) {
                    ESP_LOGW(kTag, "question %s dropped: a permission and a question are already pending",
                             event.question_id.c_str());
                } else {
                    g_deferred_question_valid = true;
                    g_deferred_question_id = event.question_id;
                    g_deferred_question_title = event.text;
                    g_deferred_question_options = event.question_options;
                    // Deferring carries the session with the ask: the promote
                    // below copies it straight into the pending ask, so a
                    // subagent's question stays answerable on its own id.
                    g_deferred_question_session = event.session_id;
                    ESP_LOGI(kTag, "question %s held until the permission ask is answered",
                             event.question_id.c_str());
                }
                break;
            }
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingQuestion;
            g_state.ui.status_label = "等待回答";
            g_state.pending_question_id = event.question_id;
            g_state.pending_question_title = event.text;
            g_state.pending_question_options = event.question_options;
            g_state.pending_question_session = event.session_id;
            g_state.ui.activity_text = event.text;
            g_state.ui.action_hint = "↑/↓ 选择 · 确认回答";
            // Not mirrored for the same reason a permission ask is not.
            MarkChangedLocked();
            break;
        case wqn::OpenCodeEventKind::kError:
            // An error is not the end of a run by itself: the gateway projects a
            // retryable upstream step failure as `agent.error` and keeps the
            // stream open. Treating every error as terminal locked the run into
            // 失败, so a step that retried successfully still ended shown as a
            // failure -- and `g_run_failed` then closed every later tool block as
            // an error too.
            AppendAgentErrorLocked(event.text, now_ms);
            if (event.fatal) {
                // The run is over, so the ask and its in-flight reply are over
                // with it: nothing can answer them any more.
                ClearAllAsksLocked();
                g_run_failed = true;
                g_state.ui.phase = wqn::AiFeaturePhase::kError;
                g_state.ui.status_label = "执行失败";
                g_state.ui.action_hint = "长按确认重试新任务";
            } else {
                // Keep running: the error is recorded, the run continues. A live
                // ask stays armed on purpose -- disarming it would leave the run
                // blocked behind an ask the gateway has already marked seen and
                // will not re-send.
                g_state.ui.activity_text = event.text;
                if (g_state.pending_permission_id.empty() &&
                    g_state.pending_question_id.empty()) {
                    g_state.ui.phase = wqn::AiFeaturePhase::kRunning;
                    g_state.ui.status_label = g_observing ? "观察中" : "Agent 执行中";
                    g_state.ui.action_hint.clear();
                }
            }
            MarkChangedLocked();
            break;
    }
    // [agent] B1/B3: every frame can change whether the watched session has a
    // run in flight, and this is the only place the device learns it from. An
    // observe attached to a session the picker called idle gets its first
    // status frame here; the moment that session starts working, the deltas
    // arrive here. Deciding at the entry points instead would make the lease
    // answer to "what the picker said when it was opened", which is exactly the
    // staleness B3 exists to remove.
    switch (event.kind) {
        // Frames that ARE a run in progress. A kText with empty text counts:
        // it is the cloud's round-boundary frame and only a live round emits
        // one. Status frames deliberately do not -- a terminal `idle` clears
        // the marks below rather than setting them.
        case wqn::OpenCodeEventKind::kTextDelta:
        case wqn::OpenCodeEventKind::kText:
        case wqn::OpenCodeEventKind::kReasoningDelta:
        case wqn::OpenCodeEventKind::kReasoning:
        case wqn::OpenCodeEventKind::kTool:
        case wqn::OpenCodeEventKind::kPermission:
        case wqn::OpenCodeEventKind::kQuestion:
            if (g_observing) {
                g_observed_run_live = true;
            }
            break;
        default:
            break;
    }
    if (g_observing && (event.kind == wqn::OpenCodeEventKind::kStatus) &&
        (event.status == "idle" || event.status == "error")) {
        // The observed run reached its terminal frame. The stream is the
        // authority while it is attached, so its word is final: the snapshot
        // row that said `running` and any delta evidence collected here both
        // go, or the lease stays held on a run that has ended.
        SettleWatchedSessionOutcomeLocked();
    }
    RefreshAgentRunLeaseLocked();
    xSemaphoreGive(g_lock);
}

void OnOpenCodeReplyFailed(
    const wqn::OpenCodeOutboundReply& reply, esp_err_t error, void*)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    // Match against the reply that is actually in flight, not against the live
    // ask: the option bar is closed as soon as the reply is queued, so the old
    // "is this ask still pending?" test read "superseded" on every outcome and
    // the restore below was unreachable. A failed reply left the run blocked
    // behind an ask the device could no longer answer, until the stream timed
    // out -- which v1 could recover from and v2 could not.
    // The flight id is the evidence, not the phase. Requiring kRunning made the
    // restore unreachable for a permission reply that arrives while a deferred
    // question holds the bar: the permission reply path promotes that question
    // (kRunning -> kAwaitingQuestion) before the POST is even attempted, so the
    // phase was already kAwaitingQuestion by the time the failure came back.
    // The flight id is safe on its own because every terminal path -- run end,
    // fatal error, observe teardown -- calls ClearReplyFlightLocked(), so a live
    // flight id can only mean "our reply, and its ask is not known to be over".
    const bool in_flight = reply.is_question
        ? g_reply_flight_question_id == reply.question_id
        : g_reply_flight_permission_id == reply.permission_id;
    if (in_flight) {
        if (reply.is_question) {
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingQuestion;
            g_state.ui.status_label = "回答失败";
            g_state.ui.activity_text = "回答未送达，可重试";
            g_state.ui.action_hint = "↑/↓ 选择 · 确认回答";
        } else {
            // The permission was never answered, so it takes the bar back. A
            // question that has meanwhile claimed the bar -- promoted from the
            // deferred slot when the reply cleared the permission, or arrived
            // fresh while the reply was on the wire -- goes back to deferred
            // rather than staying armed under a permission bar it cannot be
            // answered from. Leaving it armed would strand it: the pending id
            // blocks the question slot for the rest of the run, and
            // PromoteDeferredQuestionLocked refuses to promote while it is set.
            if (!g_state.pending_question_id.empty()) {
                if (g_deferred_question_valid) {
                    // Two questions plus a permission in one round. The gateway
                    // arms at most one ask, so this cannot happen against a
                    // compliant relay; keeping the armed one is still the safer
                    // choice, because it is the one the user is looking at.
                    ESP_LOGW(kTag,
                             "question %s displaced by %s in the deferred slot",
                             g_deferred_question_id.c_str(),
                             g_state.pending_question_id.c_str());
                }
                g_deferred_question_valid = true;
                g_deferred_question_id = std::move(g_state.pending_question_id);
                g_deferred_question_title = std::move(g_state.pending_question_title);
                g_deferred_question_options =
                    std::move(g_state.pending_question_options);
                g_deferred_question_session =
                    std::move(g_state.pending_question_session);
                ESP_LOGI(kTag, "question %s held again: the permission reply failed",
                         g_deferred_question_id.c_str());
                ClearPendingQuestionLocked();
            }
            g_state.pending_permission_id = reply.permission_id;
            // Restore the owning session along with the id, so the retry POSTs
            // to the session that raised the ask rather than to the one attached
            // when the failure came back.
            g_state.pending_permission_session = reply.session_id;
            g_state.ui.phase = wqn::AiFeaturePhase::kAwaitingPermission;
            g_state.ui.status_label = "权限回复失败";
            g_state.ui.activity_text = reply.approve ? "批准未送达，可重试"
                                                     : "拒绝未送达，可重试";
            g_state.ui.action_hint = "↑ 批准 · ↓ 拒绝";
        }
        MarkChangedLocked();
    } else {
        ESP_LOGW(kTag, "%s reply failed: %s",
                 reply.is_question ? "question" : "permission",
                 esp_err_to_name(error));
    }
    xSemaphoreGive(g_lock);
}

// [C13] True when a run submission was refused because a run is already in
// flight, which is the one upstream refusal that means "look at the run that is
// executing" rather than "your request was bad".
//
// Matched on the error code, not the message: the message is upstream prose
// that has already been translated once ("Another run is already in flight on
// this device") and matches on it break the moment that string is reworded,
// silently turning a downgrade back into a dead end. The status is checked too,
// because a code is only meaningful next to the status that carried it -- and
// `request_id_conflict`, the other 409 this route can return, must NOT reach the
// downgrade: it means the device re-used an idempotency key with a different
// prompt, which is a device bug and belongs in front of the user as an error.
//
// A 503 (run_idempotency_unavailable) is deliberately excluded: nothing is in
// flight in that case, so attaching would show a session with no run.
bool RunConflictsWithInFlightRun(const wqn::OpenCodeResult& result)
{
    return result.http_status == 409 && result.error_code == "run_in_progress";
}

void RunPrompt()
{
    g_interrupt_requested.store(false, std::memory_order_release);
    g_interrupt_delivered.store(false, std::memory_order_release);
    // A switch armed for the *previous* stream must not detach this one: the
    // handshake belongs to the stream it was raised against.
    g_switch_requested.store(false, std::memory_order_release);
    g_switch_delivered.store(false, std::memory_order_release);
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        result = wqn::RunOpenCodePrompt(
            token,
            g_run_session_id,
            g_run_detail,
            g_run_prompt,
            g_run_request_id,
            &g_outbound_replies,
            OnOpenCodeReplyFailed,
            nullptr,
            &g_interrupt_requested,
            &g_interrupt_delivered,
            &g_switch_requested,
            &g_switch_delivered,
            OnOpenCodeEvent,
            nullptr,
            &api_result);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (FinishSwitchedStreamLocked()) {
        // Leaving the session is not a failure and not a cancellation: the run
        // it was watching is still executing in the cloud. The follow-up armed
        // by ArmSwitchLocked now owns the worker and the lease.
        g_run_prompt.clear();
        // [run-id] Known outcome for *this* delivery: the device will not be
        // told how the run ends, so the next submission is a new logical run.
        // The cloud keeps its own in-flight claim for that (device, session).
        //
        // What that costs is stated plainly, because the obvious recovery is
        // NOT available right now: a fresh prompt into the same session gets a
        // 409 before any frame, which reaches SetErrorLocked as an error whose
        // hint tells the user to re-record -- and re-recording 409s again. The
        // run can then be neither watched nor stopped from this device, because
        // observe is only reachable from the picker and the picker clears the
        // current session. Turning that 409 into an automatic observe attach is
        // tracked as item C13 in doc/1005; until it lands, the honest summary is
        // that switching away from a run orphans it as far as this device can
        // tell, and the run itself is fine in the cloud.
        g_run_request_id.clear();
        xSemaphoreGive(g_lock);
        DiscardOutboundReplies();
        return;
    }
    bool refreshing_history = false;
    if (g_interrupt_delivered.load(std::memory_order_acquire)) {
        // Stopping the run on request is a success, not a failure.
        // The run is over, so any ask it raised is dead too -- and the outbound
        // queue that would carry the reply is discarded right below, so a bar
        // left armed here would answer nothing.
        ClearAllAsksLocked();
        g_state.ui.phase = wqn::AiFeaturePhase::kComplete;
        g_state.ui.status_label = "已中止";
        g_state.ui.activity_text = "任务已按确认键中止";
        g_state.ui.action_hint = "长按确认发起新任务";
        g_state.stream_active = false;
        RefreshAgentRunLeaseLocked();
        MarkChangedLocked();
    } else if (result != ESP_OK && !g_run_failed && RunConflictsWithInFlightRun(api_result)) {
        // [C13] The cloud refused this submission because a run is already in
        // flight on this (device, session) -- the ledger Stage C6 made
        // per-session, so this is now only reachable when the device and the
        // cloud disagree about what is running. The old behaviour surfaced the
        // upstream's English sentence as an error and told the user to
        // re-record, which 409s again: an unbreakable loop that left a live run
        // neither watchable nor stoppable from this device, because observe was
        // only reachable from the picker and the picker clears the current
        // session.
        //
        // Downgrading to observe is the only action that is true to what
        // happened: the prompt was NOT necessarily lost -- upstream
        // `delivery:'steer'` accepts a submission into an in-flight session and
        // queues it (§2.2) -- but the device cannot prove it landed, so it must
        // not claim it did. Attaching shows the run that is actually executing
        // and leaves the user watching something real rather than reading an
        // error that tells them to do the thing that just failed.
        //
        // The prompt stays on screen: it is the one thing worth keeping, and
        // re-attaching does not touch prompt_text.
        //
        // stream_active is raised with the attach, exactly as
        // LockSelectedOpenCodeSession does, and for the same reason: the stream
        // being chained is an observe, and ObserveSession never raises the flag.
        // Clearing it here instead left the attached stream invisible to four
        // separate readers -- the lease criterion's own gate, the interrupt
        // gesture, ArmSwitchLocked's "a stream is attached" predicate and
        // therefore LeaveOpenCodeAgentTier -- which is to say this branch
        // reattached successfully and then reproduced the dead end it was written
        // to remove: a stream the user could neither stop nor leave.
        g_state.stream_active = true;
        g_run_prompt.clear();
        g_run_request_id.clear();
        g_observing = true;
        g_run_session_id = g_state.current_session_id;
        // The tail below clears g_run_session_id unless a follow-up owns it.
        // The chained observe reads its target from that slot, exactly as the
        // history-refresh chain does -- so the "a follow-up is running" flag is
        // what keeps this branch's target alive. Clearing it would leave
        // ObserveSession arming a stream for an empty session id.
        refreshing_history = true;
        g_state.ui.phase = wqn::AiFeaturePhase::kRunning;
        g_state.ui.status_label = "执行中";
        g_state.ui.activity_text = "已有任务在运行，正在转为观察";
        g_state.ui.action_hint = "长按=中止";
        g_state.ui.requires_confirmation = false;
        g_state.confirmation_armed_at_ms = 0;
        ClearAllAsksLocked();
        ArmAgentFollowLocked();
        ResetAgentHistoryTurnLocked();
        // The stream supplies the lease criterion from here, so the frames
        // decide rather than inheriting the submission's hold.
        g_observed_run_live = false;
        RefreshAgentRunLeaseLocked();
        ChainWorkerCommandLocked(WorkerCommand::kObserveSession);
        ESP_LOGI(kTag, "run 409 on %s: downgraded to observe",
                 g_run_session_id.c_str());
        MarkChangedLocked();
    } else if (result != ESP_OK && !g_run_failed) {
        SetErrorLocked(api_result.detail.empty() ? "Agent 执行连接失败" : api_result.detail);
    } else {
        g_state.stream_active = false;
        // [detail] The brief tier hides tool work from the live stream, so the
        // digest that stands in for it exists only in the history projection.
        // Refresh the list behind the finished run instead of releasing the
        // session here: the chained kLoadHistory hands the ownership back when
        // it is done, and it needs g_run_session_id until then.
        if (g_run_detail < 1 && !g_run_failed) {
            refreshing_history = true;
            g_history_refresh = true;
            ChainWorkerCommandLocked(WorkerCommand::kLoadHistory);
        } else {
            RefreshAgentRunLeaseLocked();
        }
        MarkChangedLocked();
    }
    if (!refreshing_history) {
        g_run_session_id.clear();
    }
    g_run_prompt.clear();
    if (result == ESP_OK) {
        // [run-id] ESP_OK means the stream reached a terminal frame (or the
        // interrupt was delivered): the outcome is known, so the next
        // submission is a new logical run. A transport failure keeps the id --
        // the cloud may have accepted the run, and the retry must attach to it
        // rather than run the prompt twice.
        g_run_request_id.clear();
    }
    xSemaphoreGive(g_lock);
    DiscardOutboundReplies();
}

void CreateSession()
{
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    wqn::OpenCodeSessionInfo created;
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        result = wqn::CreateOpenCodeSession(token, &created, &api_result);
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (result == ESP_OK) {
        g_state.current_session_id = created.id;
        g_state.current_session_title = created.title;
        g_state.session_locked = true;
        g_observing = false;
        ClearAllAsksLocked();
        // Nothing to backfill in an empty session; mark it loaded so the first
        // observe attaches straight to the stream.
        g_state.history_loaded_session_id = created.id;
        g_state.ui.context_label = created.title;
        g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
        g_state.ui.status_label = "就绪";
        g_state.ui.activity_text = "新 Session 已创建";
        g_state.ui.action_hint = "长按确认录音";
        g_state.ui.prompt_text.clear();
        g_state.ui.response_text.clear();
        g_state.ui.scroll_offset_lines = 0;
        g_state.ui.requires_confirmation = false;
        g_state.confirmation_armed_at_ms = 0;
        // [agent] Same reasoning as LockSelectedOpenCodeSession: the mirrored
        // transcript belongs to the session that produced it. A new session is the
        // strongest form of that switch, and this path is reachable with the
        // channel still holding the previous session's bubbles -- the picker only
        // clears `session_locked` (ui_input.cpp), never the channel, so
        // "lock a session with a transcript, open the picker, long-confirm create"
        // used to render the OLD conversation under the NEW session's title.
        // ResetAgentHistoryTurnLocked only retires the ids being written to; the
        // already-written entries stay on screen without this.
        wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).Clear();
        ResetAgentHistoryTurnLocked();
        MarkChangedLocked();
        ReleaseWorkOwnershipLocked();
    } else {
        SetErrorLocked(api_result.detail.empty() ? "Session 创建失败" : api_result.detail);
    }    xSemaphoreGive(g_lock);
}

void ObserveSession()
{
    // Mirrors RunPrompt: the switch handshake belongs to the stream it was
    // raised against, so an observe never inherits a request armed before it
    // started. Reaching this command means nothing else owns the slot, which
    // makes this a guard rather than a live case -- but it is the difference
    // between "impossible" and "unlikely" for the orphaned-request failure.
    //
    // [interrupt-fix] That comment said "mirrors RunPrompt" while clearing only
    // the switch pair. RunPrompt clears all four; this cleared two. The half it
    // skipped is the destructive one: an interrupt request raised against a
    // PREVIOUS observe survives into this stream, the read loop's first
    // iteration sees it, POSTs /interrupt for the session this observe just
    // attached to, and breaks before reading a single frame. The tail does the
    // same in reverse (see below), so the flag is never cleared anywhere on this
    // path at all -- InterruptOpenCodeRun's !stream_active guard covers the
    // request armed with no stream, and says nothing about the one consumed and
    // left behind.
    //
    // Clearing here rather than only in the tail costs one press in a window no
    // gesture should be in anyway: ObserveOpenCodeSession raises stream_active
    // under the same lock hold that arms this command, so a long-press landing
    // between that release and this line has its request discarded. The stream
    // is not connected yet at that point, so an interrupt POST could not have
    // changed anything upstream -- and the switch pair this function already
    // clears at this exact line has always had the same window.
    g_interrupt_requested.store(false, std::memory_order_release);
    g_interrupt_delivered.store(false, std::memory_order_release);
    g_switch_requested.store(false, std::memory_order_release);
    g_switch_delivered.store(false, std::memory_order_release);
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK && g_run_session_id.empty()) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        result = wqn::WatchOpenCodeSession(
            token,
            g_run_session_id,
            g_run_detail,
            &g_outbound_replies,
            OnOpenCodeReplyFailed,
            nullptr,
            &g_interrupt_requested,
            &g_interrupt_delivered,
            &g_switch_requested,
            &g_switch_delivered,
            OnOpenCodeEvent,
            nullptr,
            &api_result);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    // [interrupt-fix] The interrupt pair belongs to the stream that just ended,
    // so it is read into a local ONCE and disarmed here, in the tail, rather
    // than left for the next command's opening to clear. Both halves are needed:
    // the opening clear above stops an armed request from leaking forward, but
    // it cannot stop `interrupt_delivered` from leaking BACKWARD -- a request
    // this stream consumed leaves that flag set, and the next observe's tail
    // then reads it true and takes this branch for a stream that ran to its
    // natural end, overwriting a settled phase with "已中止" and clearing asks
    // the next turn had just armed.
    const bool interrupt_delivered =
        g_interrupt_delivered.load(std::memory_order_acquire);
    g_interrupt_requested.store(false, std::memory_order_release);
    g_interrupt_delivered.store(false, std::memory_order_release);
    if (FinishSwitchedStreamLocked()) {
        // Same contract as the run path: detaching is an intentional end, the
        // observed run keeps going upstream, and the follow-up takes over.
        xSemaphoreGive(g_lock);
        DiscardOutboundReplies();
        return;
    }
    if (interrupt_delivered) {
        // Same reasoning as the run path: the run is over and the outbound
        // queue is discarded below, so a surviving ask could not be answered.
        ClearAllAsksLocked();
        // [interrupt-fix] Settle the phase. An interrupt ends the stream at the
        // read loop's first iteration, so no agent.status frame ever arrives to
        // settle it, and this branch used to leave kRunning on screen behind a
        // stream that no longer existed -- kRunning, "正在中止", and the
        // "长按=中止" hint. That is the dead end, not a cosmetic one:
        // AiFeatureCanStartVoiceInput excludes kRunning, so voice capture (the
        // only route to a new prompt on this page) was refused, the interrupt
        // gesture no longer had a stream to break, and the only writer that ever
        // cleared the request flag was RunPrompt -- unreachable from here. The
        // device was wedged in a state no gesture could leave.
        //
        // The wording deliberately does NOT claim the upstream run died. The
        // contract's interrupt response reports `interrupted: false` as a
        // successful "that run had already finished", and both cases arrive here
        // with the same flag -- so the one thing this device can state is what it
        // did: it stopped observing.
        g_state.ui.phase = wqn::AiFeaturePhase::kComplete;
        g_state.ui.status_label = "已中止";
        g_state.ui.activity_text = "已停止观察当前 Session";
        g_state.ui.action_hint = "长按确认发起新任务";
        g_state.stream_active = false;
        RefreshAgentRunLeaseLocked();
        MarkChangedLocked();
    } else if (result != ESP_OK && !g_run_failed) {
        SetErrorLocked(api_result.detail.empty() ? "观察连接失败" : api_result.detail);
    } else {
        g_state.stream_active = false;
        RefreshAgentRunLeaseLocked();
        MarkChangedLocked();
    }
    g_run_session_id.clear();
    g_observing = false;
    // The stream that was the only evidence a run was live is gone, so that
    // evidence is gone with it. Clearing it here (rather than leaving it to the
    // next attach) is what stops a detached stream's last delta from holding
    // the lease for the rest of the session's life.
    g_observed_run_live = false;
    xSemaphoreGive(g_lock);
    DiscardOutboundReplies();
}

// Backfills the transcript of the session the caller is about to attach to.
// Failure is a warning, not an error: observing a live run must still work when
// the history read fails, and the live deltas that follow are the authoritative
// view anyway.
void LoadHistory()
{
    std::string token;
    esp_err_t result = LoadToken(&token);
    if (result == ESP_OK) {
        result = AcquireNetwork();
    }
    std::vector<wqn::OpenCodeHistoryMessage> messages;
    wqn::OpenCodeResult api_result;
    if (result == ESP_OK) {
        result = wqn::GetOpenCodeHistory(
            token, g_run_session_id, g_run_detail, &messages, &api_result);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool refresh = g_history_refresh;
    g_history_refresh = false;
    // A lock that switched sessions while this read was in flight invalidates
    // it: the transcript must never claim a session it was not read from.
    if (result == ESP_OK && g_run_session_id != g_state.current_session_id) {
        ESP_LOGW(kTag, "history backfill dropped: %s is no longer current",
                 g_run_session_id.c_str());
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        if (refresh) {
            // Post-run refresh: the channel holds the live transcript of the
            // run that just finished, and the backfill replaces it with the
            // projection -- at the brief tier the digest exists only here.
            // Clearing only on success means a failed read leaves the live
            // transcript on screen.
            wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).Clear();
        }
        // Reset first, then append, then let the caller subscribe: the reverse
        // order lets the first live delta land on a backfilled message id and
        // overwrite history with new text.
        ResetAgentHistoryTurnLocked();
        BackfillAgentHistoryLocked(messages, esp_timer_get_time() / 1000);
        g_state.history_loaded_session_id = g_run_session_id;
        // [agent] B1 deleted the branch that used to sit here. It set phase to
        // kIdle / "就绪" / "长按确认键语音输入" for a non-observing backfill, which
        // was the lie item C4 of doc/1005 exists for: mid-run history arrives
        // with the in-flight messages in it (`finish` empty -- measured, §2.3),
        // so the device had the evidence in hand and labelled the session idle
        // anyway. A user who then long-pressed to send got silence followed by an
        // interrupt, because the send gate reads kRunning and a long press in
        // kIdle is a capture.
        //
        // It is not being re-written to fork on upstream truth, because B1 makes
        // it unreachable: every kLoadHistory armer now sets g_observing (the
        // lock, and ObserveOpenCodeSession with needs_history), so the condition
        // below was always false before the `&& !refresh` was even reached. The
        // honest state is now supplied by the stream the chain below attaches.
        //
        // If this branch is ever revived, it must not be revived as kIdle. The
        // phase is what gates the send gesture, so "idle" on a session with a
        // live run converts a send into an interrupt.
        MarkChangedLocked();
    } else {
        ESP_LOGW(kTag, "history backfill failed for %s: %s (%s)",
                 g_run_session_id.c_str(), api_result.error_code.c_str(),
                 api_result.detail.c_str());
        if (!refresh) {
            // Failure must not leave a busy phase behind: an observing caller
            // would sit on "观察中" with no stream attached, and a lock would
            // stay busy. A post-run refresh has neither problem -- the run
            // already reached its terminal UI and the live transcript is
            // intact -- so its failure is the log line above and nothing more.
            //
            // The two ternaries that used to branch on g_observing here are now
            // single-valued, because B1 made every non-refresh kLoadHistory an
            // observe's backfill. The retry hint stays, because "pick another
            // session" is wrong for a user who is already inside one -- and the
            // picker-open path clears the locked session anyway.
            g_state.ui.phase = wqn::AiFeaturePhase::kError;
            g_state.ui.status_label = "观察失败";
            g_state.ui.activity_text = "历史读取失败，可重新选择 Session 重试";
            g_state.stream_active = false;
            g_observing = false;
            MarkChangedLocked();
        }
    }
    if (result == ESP_OK && g_observing && !refresh) {
        // Chain the observe stream behind the backfill: the stream must not
        // attach until the history it precedes is already in the channel.
        // This runs inside the kLoadHistory command, so the chain must bypass
        // ArmWorkerLocked's "worker is free" guard -- FinishCommand only
        // clears the command it just ran, so it leaves this one armed.
        //
        // `!refresh` is load-bearing and was not before B1: a post-run refresh
        // (RunPrompt's brief-tier branch) inherits g_observing from the session
        // it ran in, and without this guard every brief-tier run would end by
        // re-attaching an observe stream to the session it just finished.
        g_run_session_id = g_state.current_session_id;
        g_state.ui.activity_text = "正在连接 Session 事件流";
        MarkChangedLocked();
        ChainWorkerCommandLocked(WorkerCommand::kObserveSession);
        // [agent] B1: this is the path a lock takes, so it is where the attach
        // becomes the default and where the lease has to stop being automatic.
        // A session that was idle when the list was read gets a stream here and
        // nothing else -- holding the lease on it would pin the device awake for
        // as long as the stream lives, which is until something detaches it, i.e.
        // forever. OnOpenCodeEvent revisits the decision on every frame, which
        // is the only place that can learn an idle session has started running.
        RefreshAgentRunLeaseLocked();
        xSemaphoreGive(g_lock);
        return;
    }
    g_run_session_id.clear();
    xSemaphoreGive(g_lock);
    ReleaseWorkOwnershipLocked();
}

void WorkerTask(void*)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(g_lock, portMAX_DELAY);
        const WorkerCommand command = g_command;
        xSemaphoreGive(g_lock);
        switch (command) {
            case WorkerCommand::kLoadSessions:
                LoadSessions();
                break;
            case WorkerCommand::kCreateSession:
                CreateSession();
                break;
            case WorkerCommand::kPrepareCapture:
                PrepareCapture();
                break;
            case WorkerCommand::kTranscribe:
                Transcribe();
                break;
            case WorkerCommand::kCancelVoice:
                TearDownVoiceCapture();
                break;
            case WorkerCommand::kRunPrompt:
                RunPrompt();
                break;
            case WorkerCommand::kObserveSession:
                ObserveSession();
                break;
            case WorkerCommand::kLoadHistory:
                LoadHistory();
                break;
            case WorkerCommand::kNone:
                break;
        }
        FinishCommand(command);
    }
}

esp_err_t AcquireAgentLeaseLocked()
{
    if (g_agent_sleep_lease) {
        return ESP_OK;
    }
    wqn::runtime::SleepLease lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kAiSession,
        "opencode-agent",
        __FILE__,
        __LINE__);
    if (!lease) {
        return ESP_ERR_INVALID_STATE;
    }
    g_agent_sleep_lease = std::move(lease);
    return ESP_OK;
}

void DiscardOutboundReplies()
{
    wqn::OpenCodeOutboundReply discard;
    while (g_outbound_replies.Pop(&discard)) {
    }
}

}  // namespace

namespace wqn {

esp_err_t InitOpenCodeSession()
{
    if (g_lock == nullptr) {
        g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
        if (g_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        g_state.ui.title = "OpenCode";
        g_state.ui.status_label = "未加载";
        g_state.ui.activity_text = "进入页面后加载 Session";
        g_state.ui.action_hint = "长按上下键切换页面";
        g_changed = true;
    }
    if (g_worker == nullptr) {
        g_worker = xTaskCreateStatic(
            WorkerTask,
            "wqn_agent",
            kWorkerStackBytes,
            nullptr,
            5,
            g_worker_stack,
            &g_worker_tcb);
        if (g_worker == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

// [agent] B3 / D-lease: leaving the AI page drops the agent's claim on the
// device's sleep, and detaches from the stream that justified it.
//
// It is a switch with no follow-up, which is why it goes through
// ArmSwitchLocked rather than releasing anything itself: detaching must ask
// upstream to stop NOTHING (the run keeps executing in the cloud, which is the
// whole point of the switch contract) and it must go through the same handshake
// every other detach uses. FinishSwitchedStreamLocked does the release.
//
// Nothing to detach is the common case -- the user left while nothing was
// attached -- and then this is only the lease, which the criterion decides. A
// bounded read in flight (a backfill behind a lock) is deliberately NOT counted
// as attached: it finishes on its own within seconds and must not be swapped out
// from under itself, and the follow-up it chains reaches the same decision.
//
// Idempotent and safe to call on every tier change: with no stream it is a
// lease re-decision, and a stream already detached has nothing to re-arm.
void LeaveOpenCodeAgentTier()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool stream_attached =
        (g_command == WorkerCommand::kRunPrompt ||
         g_command == WorkerCommand::kObserveSession) &&
        g_state.stream_active;
    if (stream_attached) {
        ArmSwitchLocked(WorkerCommand::kNone, std::string(), nullptr);
    } else {
        RefreshAgentRunLeaseLocked();
    }
    xSemaphoreGive(g_lock);
}

esp_err_t RequestOpenCodeSessionList(OpenCodeRejectReason* reason)
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_command != WorkerCommand::kNone) {
        // [agent] Opening the picker is the switch entry point: the user asking
        // for the session list during a run is asking to leave that run, so the
        // list request detaches from it and then loads.
        const esp_err_t busy = ArmSwitchLocked(WorkerCommand::kLoadSessions, "", reason);
        xSemaphoreGive(g_lock);
        return busy;
    }
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK && !ArmWorkerLocked(WorkerCommand::kLoadSessions)) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        g_state.ui.phase = AiFeaturePhase::kLoading;
        g_state.ui.status_label = "加载 Session";
        g_state.ui.activity_text = "正在连接 WQN Agent 网关";
        g_state.ui.action_hint.clear();
        MarkChangedLocked();
    } else {
        ReleaseWorkOwnershipLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

esp_err_t MoveOpenCodeSessionSelection(int direction)
{
    if (g_lock == nullptr || direction == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.session_locked || g_state.sessions.empty()) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const int count = static_cast<int>(g_state.sessions.size());
    const int current = static_cast<int>(g_state.selected_session);
    g_state.selected_session = static_cast<size_t>((current + direction + count) % count);
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t LockSelectedOpenCodeSession(OpenCodeRejectReason* reason)
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.sessions.empty() || g_state.selected_session >= g_state.sessions.size()) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_NOT_FOUND;
    }
    // Ownership first: locking arms the backfill, so a lock that cannot start
    // one must not touch any state at all. Mutating first and failing later
    // would leave the view pointing at a session whose transcript and stream
    // still belong to the previous one.
    const AgentSessionOption& selected = g_state.sessions[g_state.selected_session];
    if (g_command != WorkerCommand::kNone) {
        // [agent] Deliberately NOT a switch, unlike RequestOpenCodeSessionList.
        // The switch could detach the stream, but the state transition a lock
        // performs (current_session_id, session_locked, dropping the mirrored
        // transcript and the loaded-session marker) lives *after* the slot is
        // acquired, and LoadHistory does none of it -- it only reads whatever
        // g_run_session_id says. Applying that mutation before the slot is free
        // would put the view on a session whose transcript is still the
        // previous one's, which is exactly the hazard the comment above guards
        // against. The picker-open path is the switch: reaching the lock at all
        // means the user already came through it, so the slot is free by then.
        if (reason != nullptr) {
            *reason = wqn::OpenCodeRejectReason::kWorkerBusy;
        }
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK) {
        // The worker reads the target session from g_run_session_id, the same
        // handoff slot ObserveOpenCodeSession uses.
        g_run_session_id = selected.id;
        g_run_detail = g_state.detail_level;
        if (!ArmWorkerLocked(WorkerCommand::kLoadHistory)) {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result != ESP_OK) {
        g_run_session_id.clear();
        g_state.stream_active = false;
        ReleaseWorkOwnershipLocked();
        xSemaphoreGive(g_lock);
        return result;
    }
    g_state.current_session_id = selected.id;
    g_state.current_session_title = selected.title;
    g_state.session_locked = true;
    // [agent] B1: locking a session IS attaching to it. Observe used to be a
    // separate gesture, which made "open a session that already has a run in
    // flight" the one shape that showed nothing -- the picker could not tell the
    // user which session was running (D-which, contract 2.2), so the only way to
    // find out was to lock one and watch it say nothing. Setting g_observing
    // here makes LoadHistory chain kObserveSession behind the backfill, which
    // is the whole of the change: the observe state machine, the switch
    // handshake and the terminal handling are all reused untouched.
    //
    // What it costs is the lease: the attach is now unconditional, so
    // AgentRunInFlightLocked -- not "a stream is open" -- is what decides
    // whether this device stays awake. See the criterion's comment for why
    // that is the only workable answer.
    g_observing = true;
    ClearAllAsksLocked();
    // The mirrored transcript belongs to the session that produced it: drop the
    // backfill marker so the load armed above re-reads it for this session.
    g_state.history_loaded_session_id.clear();
    g_state.ui.context_label = selected.title;
    g_state.ui.phase = AiFeaturePhase::kLoading;
    g_state.ui.status_label = "读取历史";
    g_state.ui.activity_text = "正在读取历史对话";
    // B1 removed the observe gesture, so the hint no longer offers one. The
    // second confirm on the status bar's detail row keeps its own meaning
    // (edit the tier), which is exactly what it says here.
    g_state.ui.action_hint = "长按确认录音 · ↑/↓ 滚动";
    g_state.ui.prompt_text.clear();
    g_state.ui.response_text.clear();
    g_state.ui.requires_confirmation = false;
    g_state.ui.scroll_offset_lines = 0;
    g_state.confirmation_armed_at_ms = 0;
    // [agent] Switching sessions switches conversation: the mirrored transcript
    // belongs to the session that produced it, so drop it rather than letting
    // the new session render the previous one's bubbles.
    wqn::GetAiHistory(wqn::AiHistoryChannel::kAgent).Clear();
    ResetAgentHistoryTurnLocked();
    // [agent] Raise stream_active with the attach, exactly as
    // ObserveOpenCodeSession does. The chained observe it arms goes straight to
    // ObserveSession, which never raises it -- so without this the default
    // attach (B1) would run with stream_active false for its whole life, and
    // two things that read that flag would silently not apply to it:
    // ArmSwitchLocked's "a stream is attached" predicate (the user could not
    // switch away from a default attach at all), and AgentRunInFlightLocked's
    // lease criterion. LoadHistory clears it again on its own failure path.
    g_state.stream_active = true;
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t CreateNewOpenCodeSession(OpenCodeRejectReason* reason)
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_command != WorkerCommand::kNone) {
        // Same scoping decision as LockSelectedOpenCodeSession: the picker-open
        // path is the switch, and a create is only reachable from the picker.
        if (reason != nullptr) {
            *reason = wqn::OpenCodeRejectReason::kWorkerBusy;
        }
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK && !ArmWorkerLocked(WorkerCommand::kCreateSession)) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        g_observing = false;
        g_state.ui.phase = AiFeaturePhase::kLoading;
        g_state.ui.status_label = "创建 Session";
        g_state.ui.activity_text = "正在通过 WQN 网关新建";
        g_state.ui.action_hint.clear();
        MarkChangedLocked();
    } else {
        ReleaseWorkOwnershipLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

esp_err_t ObserveOpenCodeSession(OpenCodeRejectReason* reason)
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.current_session_id.empty()) {
        if (reason != nullptr) {
            *reason = wqn::OpenCodeRejectReason::kNoSession;
        }
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    // Backfill runs once per session, before the stream attaches. Repeating it
    // on every reconnect would append the same turns twice and fight the local
    // copy of the prompt the device already appended when it was submitted.
    const bool needs_history =
        g_state.history_loaded_session_id != g_state.current_session_id;
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK && g_command != WorkerCommand::kNone) {
        // Same scoping decision as the lock and create paths -- with one extra
        // reason: observe's own follow-up depends on g_run_detail and on
        // g_observing surviving into it, both of which this function writes
        // below. Return rather than falling through: the tail below releases
        // ownership, and that ownership belongs to whatever command still holds
        // the slot, not to this call.
        if (reason != nullptr) {
            *reason = wqn::OpenCodeRejectReason::kWorkerBusy;
        }
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        g_run_failed = false;
        g_observing = true;
        // The worker reads the target session from g_run_session_id, the same
        // handoff slot ConfirmOpenCodePrompt uses.
        g_run_session_id = g_state.current_session_id;
        // [detail] The chained kObserveSession reuses this value: the command
        // gate is single-slot, so nothing can re-arm between the backfill and
        // the attach that follows it.
        g_run_detail = g_state.detail_level;
        // Attaching is a fresh turn: an ask the previous attach left armed is
        // stale, and a gateway re-attach re-discovers its own pending asks --
        // holding one would swallow the fresh copy for the slot it occupies.
        ClearAllAsksLocked();
        // Observe locks the session so the interaction view (not the picker)
        // renders while the stream is attached; the lock persists afterwards
        // so the observed session can immediately be prompted as well.
        g_state.session_locked = true;
        // [run-live] Claim the phase the row can support, NOT kRunning
        // unconditionally. "观察中" was always true here; kRunning is the part
        // that was a guess, and the bottom band turned it into 长按=中止 -- a
        // destructive gesture offered for a session whose run had already
        // finished. See ObserveAttachPhaseLocked.
        g_state.ui.phase = ObserveAttachPhaseLocked();
        g_state.ui.status_label = "观察中";
        g_state.ui.response_text.clear();
        g_state.ui.activity_text = needs_history
            ? "正在读取历史对话"
            : (g_state.ui.phase == wqn::AiFeaturePhase::kRunning
                   ? "正在连接 Session 事件流"
                   : "无运行中任务或任务已结束");
        g_state.ui.action_hint = g_state.ui.phase == wqn::AiFeaturePhase::kRunning
                                     ? std::string()
                                     : std::string("长按确认发起新任务");
        g_state.ui.scroll_offset_lines = 0;
        // [follow] Attaching is a fresh turn (see ArmAgentFollowLocked): the
        // viewport watches the stream and parks on the newest answer once it
        // has a body. A session whose newest entry is already a full answer
        // retires the follow on the very next tick, so nothing moves.
        ArmAgentFollowLocked();
        g_state.stream_active = true;
        // Attaching mid-stream: any assistant id from the previous run is stale.
        ResetAgentHistoryTurnLocked();
        // The stream has not produced a frame yet, so a previous attach's
        // evidence must not be inherited by this one -- it would hold the lease
        // on a session this attach has seen nothing from.
        g_observed_run_live = false;
        if (!ArmWorkerLocked(needs_history ? WorkerCommand::kLoadHistory
                                           : WorkerCommand::kObserveSession)) {
            g_run_session_id.clear();
            result = ESP_ERR_INVALID_STATE;
        }
        // [run-live] Publish before the worker starts, so the picker row's claim
        // is on screen from the very first frame of UI state rather than one
        // snapshot later. stream_active is set just above, which is what lets the
        // criterion's observe branch reach the row at all.
        RefreshAgentRunLeaseLocked();
        MarkChangedLocked();
    }
    if (result != ESP_OK) {
        g_observing = false;
        g_state.stream_active = false;
        RefreshAgentRunLeaseLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

esp_err_t ReplyPendingOpenCodePermission(bool approve)
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.ui.phase != AiFeaturePhase::kAwaitingPermission ||
        g_state.pending_permission_id.empty()) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (g_switch_requested.load(std::memory_order_acquire)) {
        // A switch is pending: the stream carrying this reply is about to detach
        // and DiscardOutboundReplies will destroy whatever is queued, so pushing
        // would tell the user "已批准" for an answer the upstream run never
        // receives -- leaving it blocked behind a permission nobody can answer.
        // Refuse instead; the detach clears the ask a moment later anyway.
        ESP_LOGW(kTag, "permission reply refused: a session switch is pending");
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_outbound_replies.Push(wqn::OpenCodeOutboundReply{
        g_state.pending_permission_id, approve, false, {},
        g_state.pending_permission_session, {}});
    // The reply on the wire is the only thing that proves the id was delivered.
    // The UI closes its option bar here, so the live id is deliberately not it.
    g_reply_flight_permission_id = g_state.pending_permission_id;
    ClearPendingPermissionLocked();
    g_state.ui.phase = AiFeaturePhase::kRunning;
    g_state.ui.status_label = g_observing ? "观察中" : "Agent 执行中";
    g_state.ui.activity_text = approve ? "已批准权限" : "已拒绝权限";
    g_state.ui.action_hint.clear();
    // [follow] The run continues after the ask: watch it again from the tail
    // and park on the answer body when it lands.
    ArmAgentFollowLocked();
    PromoteDeferredQuestionLocked();
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t ReplyPendingOpenCodeQuestion(int index)
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.ui.phase != AiFeaturePhase::kAwaitingQuestion ||
        g_state.pending_question_id.empty() ||
        index < 0 || index >= static_cast<int>(g_state.pending_question_options.size())) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_ARG;
    }
    // The answer is the option's value, never its label and never a field id:
    // the gateway is what knows which upstream field the option came from.
    if (g_switch_requested.load(std::memory_order_acquire)) {
        // Same as the permission reply: queuing an answer for a stream that is
        // about to detach strands the run behind a question nobody can answer.
        ESP_LOGW(kTag, "question reply refused: a session switch is pending");
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const std::string answer = g_state.pending_question_options[index].value;
    wqn::OpenCodeOutboundReply reply;
    reply.is_question = true;
    reply.question_id = g_state.pending_question_id;
    // The owning session travels with the ask, so a subagent's question is
    // answered on the subagent's own id rather than on the attached one.
    reply.session_id = g_state.pending_question_session;
    reply.answer = answer;
    // Capture the id before the queue takes the reply. `Push` takes it by
    // value, and a libstdc++ string move empties the source, so reading
    // `reply.question_id` after the push below yields "". The flight id would
    // then never match the id inside the queue, `in_flight` would be false
    // forever, and a failed question reply could not restore the bar -- the one
    // path that never had this bug until the shared id was introduced.
    const std::string flight_id = reply.question_id;
    g_outbound_replies.Push(std::move(reply));
    g_reply_flight_question_id = flight_id;
    g_state.ui.phase = AiFeaturePhase::kRunning;
    g_state.ui.status_label = g_observing ? "观察中" : "Agent 执行中";
    g_state.ui.activity_text = "已回答：" + answer;
    g_state.ui.action_hint.clear();
    // [follow] Same as the permission reply: the run resumes, so watch it.
    ArmAgentFollowLocked();
    ClearDeferredQuestionLocked();
    // The pending ask stays armed until a terminal status: if the reply POST
    // fails, OnOpenCodeReplyFailed restores exactly this state for a retry.
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

void InterruptOpenCodeRun()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_state.stream_active) {
        // No stream to break: an interrupt for a run nobody started is a no-op,
        // and the worker would consume the flag on its next (unrelated) stream.
        g_interrupt_requested.store(false, std::memory_order_release);
        xSemaphoreGive(g_lock);
        return;
    }
    // Flag only. The streaming worker makes the interrupt POST itself, so the
    // UI thread never opens a connection of its own while a stream is attached.
    g_interrupt_requested.store(true, std::memory_order_release);
    // [agent] A pending switch is deliberately LEFT armed: the user asked for
    // the picker and then for a cancel, and both can be served -- the interrupt
    // POST stops the run and the tail still chains the follow-up, so the user
    // lands on the picker they asked for instead of back on the session they
    // were leaving. Clearing it here would revoke an already-acknowledged
    // switch and strand them with "已中止".
    g_state.ui.status_label = "正在中止";
    g_state.ui.activity_text = "已请求中止当前任务";
    MarkChangedLocked();
    xSemaphoreGive(g_lock);
}

esp_err_t StartOpenCodeVoiceInput()
{
    ESP_RETURN_ON_ERROR(InitOpenCodeSession(), kTag, "init OpenCode session");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_state.session_locked || !AiFeatureCanStartVoiceInput(g_state.ui.phase)) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (g_command != WorkerCommand::kNone) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool append_to_pending_prompt =
        g_state.ui.phase == AiFeaturePhase::kAwaitingConfirmation;
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK) {
        g_recording_requested = true;
        if (!ArmWorkerLocked(WorkerCommand::kPrepareCapture)) {
            g_recording_requested = false;
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result == ESP_OK) {
        // [voice-pipe] A fresh capture starts from a clean voice slate: no
        // partial text, no stale stream error, no abort left armed.
        g_voice_last_error.clear();
        g_voice_abort_requested = false;
        g_state.ui.voice_partial.clear();
        if (!append_to_pending_prompt) {
            g_state.ui.prompt_text.clear();
            g_state.ui.scroll_offset_lines = 0;
        }
        // [run-id] Both branches change the prompt (replace or append), and
        // the cloud fingerprints the prompt text: a pending idempotency key
        // belongs to a text that no longer exists, so it must not be reused.
        g_run_request_id.clear();
        // [follow] Capture arms the follow so the viewport is already watching
        // when the transcript is confirmed. The per-tick step skips the capture
        // phases outright (the codec is being configured and must not share the
        // panel with a repaint), so this only takes effect once the transcript
        // is back -- by which point the newest content is what the user needs.
        ArmAgentFollowLocked();
        g_state.confirmation_armed_at_ms = 0;
        g_state.ui.phase = AiFeaturePhase::kLoading;
        g_state.ui.status_label = "准备录音";
        g_state.ui.activity_text = "正在连接 WiFi 与麦克风";
        g_state.ui.action_hint = "保持按住确认键";
        MarkChangedLocked();
    } else {
        ReleaseWorkOwnershipLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

esp_err_t StopOpenCodeVoiceInput()
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.ui.phase == AiFeaturePhase::kLoading && g_recording_requested) {
        g_recording_requested = false;
        xSemaphoreGive(g_lock);
        return ESP_OK;
    }
    if (g_state.ui.phase != AiFeaturePhase::kRecording) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_recording_requested = false;
    g_command = WorkerCommand::kTranscribe;
    g_state.ui.phase = AiFeaturePhase::kTranscribing;
    g_state.ui.status_label = "语音转写中";
    g_state.ui.activity_text = "转写完成后必须确认才会发送";
    g_state.ui.action_hint.clear();
    MarkChangedLocked();
    xTaskNotifyGive(g_worker);
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

void CancelAgentVoiceInput()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const wqn::AiFeaturePhase phase = g_state.ui.phase;
    if (phase == wqn::AiFeaturePhase::kRecording) {
        // Hard abort: the audio is discarded and the WS turn is torn down. The
        // worker usually owns no command in this phase, but the tail of
        // kPrepareCapture (bringing the WS turn up) still does -- it checks the
        // flag after the turn exists and tears it down itself.
        g_voice_abort_requested = true;
        if (g_command == WorkerCommand::kNone) {
            ArmWorkerLocked(WorkerCommand::kCancelVoice);
        }
        g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
        g_state.ui.status_label = "已取消录音";
        g_state.ui.activity_text = "录音已丢弃";
        g_state.ui.action_hint = "长按确认重新录音";
        g_state.ui.voice_partial.clear();
        MarkChangedLocked();
    } else if (phase == wqn::AiFeaturePhase::kTranscribing) {
        // Soft cancel: the finalize stops waiting and drops whatever the
        // stream returns; the turn is left to finish on the transport's own
        // time. A prompt re-record falls back to the batch endpoint when the
        // transport is still busy.
        wqn::AgentVoiceRequestCancel();
        g_state.ui.phase = wqn::AiFeaturePhase::kIdle;
        g_state.ui.status_label = "已取消";
        g_state.ui.activity_text = "语音内容未发送";
        g_state.ui.action_hint = "长按确认重新录音";
        g_state.ui.voice_partial.clear();
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

esp_err_t ConfirmOpenCodePrompt(int64_t confirmed_at_ms)
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!AiFeatureCanSubmit(g_state.ui) || g_state.current_session_id.empty() ||
        g_state.confirmation_armed_at_ms <= 0 ||
        confirmed_at_ms < g_state.confirmation_armed_at_ms) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (g_command != WorkerCommand::kNone) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = AcquireAgentLeaseLocked();
    if (result == ESP_OK) {
        g_run_session_id = g_state.current_session_id;
        g_run_prompt = g_state.ui.prompt_text;
        g_run_detail = g_state.detail_level;
        // [run-id] Minted once per logical submission; a transport retry of
        // the same prompt reuses it. Anything that changes the prompt clears
        // it (StartOpenCodeVoiceInput), because the cloud fingerprints the
        // text: the same id with a different prompt is a conflict, not a
        // retry. A known terminal outcome also clears it (RunPrompt).
        if (g_run_request_id.empty()) {
            g_run_request_id = wqn::GenerateRequestId();
        }
        if (!ArmWorkerLocked(WorkerCommand::kRunPrompt)) {
            g_run_session_id.clear();
            g_run_prompt.clear();
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result == ESP_OK) {
        g_run_failed = false;
        // A submitted run is a fresh turn. Ask state the previous turn left
        // behind cannot be answered any more -- the gateway it belonged to is
        // gone -- and the re-attached gateway re-discovers its own pending asks.
        // That includes the two live slots: a bar left armed from the previous
        // turn would hide this run's first ask and answer nothing.
        ClearAllAsksLocked();
        g_state.ui.phase = AiFeaturePhase::kSubmitting;
        g_state.ui.status_label = "正在提交";
        g_state.ui.response_text.clear();
        g_state.ui.activity_text = "WQN 正在中转到 OpenCode";
        g_state.ui.action_hint.clear();
        g_state.ui.requires_confirmation = false;
        g_state.confirmation_armed_at_ms = 0;
        g_state.ui.scroll_offset_lines = 0;
        // [follow] Submit starts the turn the viewport should watch.
        ArmAgentFollowLocked();
        g_state.stream_active = true;
        // [agent] Mirror the submitted prompt into the kAgent channel before the
        // worker starts, so the user bubble is on screen while the gateway is
        // still connecting.
        AppendAgentUserLocked(g_run_prompt, esp_timer_get_time() / 1000);
        MarkChangedLocked();
    } else {
        ReleaseWorkOwnershipLocked();
    }
    xSemaphoreGive(g_lock);
    return result;
}

void CancelOpenCodePrompt()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.ui.phase == AiFeaturePhase::kAwaitingConfirmation) {
        g_state.ui.prompt_text.clear();
        g_state.ui.phase = AiFeaturePhase::kIdle;
        g_state.ui.status_label = "已取消";
        g_state.ui.activity_text = "语音内容未发送";
        g_state.ui.action_hint = "长按确认重新录音";
        g_state.ui.requires_confirmation = false;
        g_state.confirmation_armed_at_ms = 0;
        // Nothing was sent, so the armed turn never reached history.
        ResetAgentHistoryTurnLocked();
        MarkChangedLocked();
        xSemaphoreGive(g_lock);
        return;
    }
    const bool attached = g_state.stream_active;
    xSemaphoreGive(g_lock);
    if (attached) {
        // A submitted run is stopped through the interrupt path; the stream
        // worker performs the POST and reports back through ReadAgentEventStream.
        InterruptOpenCodeRun();
    }
}

// [scroll-clamp] The offset lives in the AI page viewport's coordinate space,
// where 0 is the anchor (newest question at the top) and NEGATIVE values scroll
// further down to the tail of the newest reply. Only the renderer knows how tall
// the history is, so the backend cannot clamp this itself: the caller passes the
// bounds it already computed for the STD/Pro path (GetAiScrollBounds) and both
// tiers share one clamp.
//
// The old `std::max<int32_t>(0, next)` was correct for the retired standalone
// page, which measured the offset as "distance from latest" and therefore
// wanted a floor of 0. On the AI page that same floor made the bottom of the
// newest reply unreachable, which read as "cannot scroll to the very bottom".
void ScrollOpenCodeResponse(int direction, int32_t min_scroll, int32_t max_scroll)
{
    if (g_lock == nullptr || direction == 0) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    int32_t next = g_state.ui.scroll_offset_lines + direction * 4;
    if (next < min_scroll) {
        next = min_scroll;
    }
    if (next > max_scroll) {
        next = max_scroll;
    }
    // Only mark changed when the offset actually moved: a no-op press at either
    // bound must not cost an EPD refresh.
    if (next != g_state.ui.scroll_offset_lines) {
        g_state.ui.scroll_offset_lines = next;
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

bool CopyOpenCodeSessionToUi(AgentSessionState* state)
{
    if (g_lock == nullptr || state == nullptr) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool changed = g_changed;
    if (changed) {
        *state = g_state;
        g_changed = false;
    }
    xSemaphoreGive(g_lock);
    return changed;
}

void SetOpenCodeScrollOffsetClamped(int32_t target, int32_t min_scroll, int32_t max_scroll)
{
    if (g_lock == nullptr) {
        return;
    }
    if (min_scroll > max_scroll) {
        return;  // degenerate bounds: fail open, leave the offset untouched
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    // Read-clamp-write stays inside ONE lock hold: a split Get/Set across locks
    // would let a streaming auto-follow interleave between them.
    if (target > max_scroll) {
        target = max_scroll;
    }
    if (target < min_scroll) {
        target = min_scroll;
    }
    constexpr int32_t kMaxScrollRows = 4096;
    if (target > kMaxScrollRows) {
        target = kMaxScrollRows;
    }
    if (target < -kMaxScrollRows) {
        target = -kMaxScrollRows;
    }
    // Only mark changed when the offset actually moved: the per-tick follow
    // calls this every 50 ms while it is pinned, and a redundant mark would
    // repaint the panel on every tick.
    if (g_state.ui.scroll_offset_lines != target) {
        g_state.ui.scroll_offset_lines = target;
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

void SetOpenCodeFollowState(bool active, bool user_moved)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.follow_active != active || g_state.user_moved != user_moved) {
        g_state.follow_active = active;
        g_state.user_moved = user_moved;
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

void SetOpenCodeDetailLevel(uint8_t level)
{
    if (g_lock == nullptr || level > 2) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.detail_level != level) {
        g_state.detail_level = level;
        // The transcript cache key is the session id alone, so a tier change
        // invalidates what is mirrored: clearing it makes the next Lock/Observe
        // take the needs_history branch and re-read the session at the new tier.
        // The already-mirrored turns keep the old tier until then -- deliberately
        // no eager clear, so the user does not watch the panel empty out.
        g_state.history_loaded_session_id.clear();
        MarkChangedLocked();
    }
    xSemaphoreGive(g_lock);
}

}  // namespace wqn

#else  // !CONFIG_WQN_AGENT_ENABLE

// [agent] Agent tier compiled out. The public API keeps its signatures so the
// AI page's tier switch, status-bar slots, scroll branch and history mirror all
// build unchanged -- they simply never fire, because NextAiTier does not offer
// kAgent in this configuration. Every entry point refuses instead of touching
// the network, so a stale call cannot reach a gateway this build has no client
// for. Mirrors the stub half of ai_session.cpp.
namespace wqn {

esp_err_t InitOpenCodeSession() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t RequestOpenCodeSessionList(OpenCodeRejectReason*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t MoveOpenCodeSessionSelection(int) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t LockSelectedOpenCodeSession(OpenCodeRejectReason*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t CreateNewOpenCodeSession(OpenCodeRejectReason*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ObserveOpenCodeSession(OpenCodeRejectReason*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ReplyPendingOpenCodePermission(bool) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t StartOpenCodeVoiceInput() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t StopOpenCodeVoiceInput() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ConfirmOpenCodePrompt(int64_t) { return ESP_ERR_NOT_SUPPORTED; }
void CancelOpenCodePrompt() {}
void CancelAgentVoiceInput() {}
void ScrollOpenCodeResponse(int, int32_t, int32_t) {}
void SetOpenCodeScrollOffsetClamped(int32_t, int32_t, int32_t) {}
void SetOpenCodeFollowState(bool, bool) {}
void SetOpenCodeDetailLevel(uint8_t) {}
bool CopyOpenCodeSessionToUi(AgentSessionState*) { return false; }

}  // namespace wqn

#endif  // CONFIG_WQN_AGENT_ENABLE
