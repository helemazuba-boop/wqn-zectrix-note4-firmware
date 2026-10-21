#include "agent_voice_pipe.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <atomic>
#include <string>

#include "ai_session.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "stdpro_ws_transport.h"

namespace {

constexpr char kTag[] = "wqn_agent_voice";
// Finalize wait slice: short enough that a soft cancel and the deadline are
// observed promptly, long enough to keep the polling loop cheap.
constexpr TickType_t kWaitSlice = pdMS_TO_TICKS(100);
constexpr uint32_t kConnectTimeoutMs = 2500;
constexpr uint32_t kFinalSendTimeoutMs = 2000;
// kAmbiguous means the FINAL send is indeterminate: a batch retry could
// duplicate the turn, so the wait is extended instead of falling back. Same
// bound as the STD driver (ai_session.cpp SubmitSession).
constexpr uint32_t kAmbiguousWaitMs = 5000;
// Release observation on the no-result path only (see finalize). Long enough
// for a healthy turn to release, short enough not to stall the worker.
constexpr uint32_t kReleaseObserveMs = 3000;
constexpr uint32_t kDefaultFinalizeTimeoutMs = 15000;

StaticSemaphore_t g_result_sem_storage = {};
SemaphoreHandle_t g_result_sem = nullptr;
StaticSemaphore_t g_lock_storage = {};
SemaphoreHandle_t g_lock = nullptr;

std::string g_transcript;     // accumulated asr.delta text
std::string g_complete_text;  // authoritative asr.complete text
uint64_t g_last_event_id = 0;
bool g_saw_complete = false;
bool g_saw_error = false;
bool g_turn_active = false;
std::string g_active_req_id;
uint32_t g_active_turn_gen = 0;

std::atomic<bool> g_cancel_requested{false};

wqn::WqnAiSseCallback g_forward_cb = nullptr;
void* g_forward_ctx = nullptr;

void EnsurePrimitives()
{
    if (g_result_sem == nullptr) {
        g_result_sem = xSemaphoreCreateBinaryStatic(&g_result_sem_storage);
    }
    if (g_lock == nullptr) {
        g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
    }
}

void ResetTurnStateLocked()
{
    g_transcript.clear();
    g_complete_text.clear();
    g_last_event_id = 0;
    g_saw_complete = false;
    g_saw_error = false;
    g_turn_active = false;
    g_active_req_id.clear();
    g_active_turn_gen = 0;
}

void ClearActiveTurnLocked()
{
    g_turn_active = false;
    g_active_req_id.clear();
    g_active_turn_gen = 0;
}

// Runs on the transport task. Accumulates the ASR text for the worker and
// forwards every event of the active turn to the caller's observer. Events
// that arrive after the turn was finalized or aborted are ignored: the UI
// already moved on, and forwarding them would resurrect a stale bubble.
void AgentVoiceSseTrampoline(const wqn::WqnAiSseEvent& ev, void* /*user_ctx*/)
{
    using Kind = wqn::WqnAiSseEvent::Kind;
    if (g_lock == nullptr) {
        return;
    }
    bool forward = false;
    bool signal = false;
    wqn::WqnAiSseCallback forward_cb = nullptr;
    void* forward_ctx = nullptr;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_turn_active) {
        if (ev.event_id != 0 && ev.event_id <= g_last_event_id) {
            xSemaphoreGive(g_lock);
            return;  // replayed/duplicate frame
        }
        if (ev.event_id != 0) {
            g_last_event_id = ev.event_id;
        }
        forward = true;
        forward_cb = g_forward_cb;
        forward_ctx = g_forward_ctx;
        switch (ev.kind) {
            case Kind::kAsrDelta:
                g_transcript += ev.delta;
                break;
            case Kind::kAsrComplete:
                g_complete_text = ev.text;
                g_saw_complete = true;
                signal = true;
                break;
            case Kind::kAsrFailed:
            case Kind::kError: {
                const std::string& msg = !ev.error_message.empty()
                    ? ev.error_message
                    : ev.error_code;
                ESP_LOGW(kTag, "turn error: %s", msg.c_str());
                g_saw_error = true;
                signal = true;
                break;
            }
            default:
                break;
        }
    }
    xSemaphoreGive(g_lock);

    // Forward before signalling: the worker's error branch reads the message
    // the observer stashes, so the stash must land first.
    if (forward && forward_cb != nullptr) {
        forward_cb(ev, forward_ctx);
    }
    if (signal && g_result_sem != nullptr) {
        xSemaphoreGive(g_result_sem);
    }
}

}  // namespace

namespace wqn {

void AgentVoiceRegisterDeltaCallback(WqnAiSseCallback cb, void* ctx)
{
    g_forward_cb = cb;
    g_forward_ctx = ctx;
    // The transport holds one global callback slot: install the module's
    // trampoline so the text is accumulated even when no observer is set. The
    // STD driver re-installs its own trampoline before each of its turns.
    wqn::stdpro_ws::SetSseCallback(&AgentVoiceSseTrampoline, nullptr);
}

esp_err_t AgentVoiceTurnStart(const std::string& token,
                              const std::string& request_id,
                              uint32_t* out_turn_gen)
{
    EnsurePrimitives();
    if (g_result_sem == nullptr || g_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (request_id.empty()) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    ResetTurnStateLocked();
    xSemaphoreGive(g_lock);

    // A stale soft cancel from a previous turn must not kill this one, and a
    // stale result signal must not satisfy the next finalize.
    g_cancel_requested.store(false, std::memory_order_release);
    xSemaphoreTake(g_result_sem, 0);

    const esp_err_t conn = wqn::stdpro_ws::EnsureConnected(token, kConnectTimeoutMs);
    if (conn != ESP_OK) {
        ESP_LOGW(kTag, "EnsureConnected failed (%s)", esp_err_to_name(conn));
        return conn;
    }

    // Arm the replay BEFORE StartTurn: the transport flips the turn to
    // kRecording internally and the capture task outranks this worker, so the
    // tap can deliver a live block before StartTurn even returns. Arming first
    // is what keeps the backlog ahead of the live audio (same rule as the STD
    // driver, see ai_session.cpp PrepareRecordingSession).
    wqn::ArmAiVoicePreroll();

    uint32_t turn_gen = 0;
    const esp_err_t start = wqn::stdpro_ws::StartTurn(
        request_id, "agent", std::string(), false, nullptr, &turn_gen);
    if (start != ESP_OK) {
        ESP_LOGW(kTag, "StartTurn failed (%s)", esp_err_to_name(start));
        if (start == ESP_ERR_TIMEOUT) {
            // The owner may still commit the turn after the caller timed out;
            // an identity-bound abort no-ops when nothing was committed and
            // tears down a late commit.
            wqn::stdpro_ws::AbortTurn(request_id);
        }
        return start;
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_turn_active = true;
    g_active_req_id = request_id;
    g_active_turn_gen = turn_gen;
    xSemaphoreGive(g_lock);

    if (out_turn_gen != nullptr) {
        *out_turn_gen = turn_gen;
    }
    ESP_LOGI(kTag, "turn started req=%s gen=%lu",
             request_id.c_str(), static_cast<unsigned long>(turn_gen));
    return ESP_OK;
}

esp_err_t AgentVoiceTurnFinalize(int duration_ms, uint32_t timeout_ms,
                                 std::string* out_transcript)
{
    if (out_transcript == nullptr || g_result_sem == nullptr || g_lock == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    out_transcript->clear();

    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool active = g_turn_active;
    const std::string req_id = g_active_req_id;
    const uint32_t turn_gen = g_active_turn_gen;
    xSemaphoreGive(g_lock);
    if (!active || req_id.empty()) {
        return ESP_ERR_INVALID_STATE;  // no turn: batch fallback
    }

    const auto handoff = wqn::stdpro_ws::SendFinalAndWait(
        req_id, duration_ms, kFinalSendTimeoutMs);
    if (handoff == wqn::stdpro_ws::FinalHandoffResult::kDefinitelyNotSent) {
        ESP_LOGW(kTag, "FINAL not sent; batch fallback is safe");
        xSemaphoreTake(g_lock, portMAX_DELAY);
        ClearActiveTurnLocked();
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    const uint32_t wait_ms = timeout_ms > 0 ? timeout_ms : kDefaultFinalizeTimeoutMs;
    int64_t deadline_ms = now_ms + wait_ms;
    if (handoff == wqn::stdpro_ws::FinalHandoffResult::kAmbiguous) {
        ESP_LOGW(kTag, "FINAL ambiguous; waiting %u ms without fallback",
                 static_cast<unsigned>(kAmbiguousWaitMs));
        deadline_ms = std::min(deadline_ms, now_ms + static_cast<int64_t>(kAmbiguousWaitMs));
    }

    bool complete = false;
    bool error = false;
    bool cancelled = false;
    for (;;) {
        if (g_cancel_requested.load(std::memory_order_acquire)) {
            cancelled = true;
            break;
        }
        const int64_t slice_now_ms = esp_timer_get_time() / 1000;
        if (slice_now_ms >= deadline_ms) {
            break;
        }
        const int64_t remaining_ms = deadline_ms - slice_now_ms;
        const TickType_t slice =
            std::min(kWaitSlice, pdMS_TO_TICKS(remaining_ms));
        xSemaphoreTake(g_result_sem, slice);
        xSemaphoreTake(g_lock, portMAX_DELAY);
        complete = g_saw_complete;
        error = g_saw_error;
        xSemaphoreGive(g_lock);
        if (complete || error) {
            break;
        }
    }

    esp_err_t result = ESP_OK;
    if (cancelled) {
        // Soft cancel: the caller discards. The turn is left to finish on the
        // transport's own time -- turn.released resets it to kIdle there, so a
        // prompt re-record only pays the batch fallback, never a wedge.
        ESP_LOGI(kTag, "turn cancelled before a result; discarding");
    } else if (complete) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        *out_transcript = !g_complete_text.empty() ? g_complete_text : g_transcript;
        xSemaphoreGive(g_lock);
        if (out_transcript->empty()) {
            result = ESP_ERR_INVALID_SIZE;  // no speech
        }
    } else if (error) {
        result = ESP_ERR_INVALID_RESPONSE;
    } else {
        // Nothing arrived before the deadline: observe the turn terminal so a
        // stuck transport self-heals (WaitForTurnRelease disconnects on its
        // own timeout) before reporting the failure.
        wqn::stdpro_ws::WaitForTurnRelease(turn_gen, req_id, kReleaseObserveMs);
        result = ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    ClearActiveTurnLocked();
    xSemaphoreGive(g_lock);
    return result;
}

void AgentVoiceTurnAbort()
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool active = g_turn_active;
    const std::string req_id = g_active_req_id;
    const uint32_t turn_gen = g_active_turn_gen;
    ClearActiveTurnLocked();
    xSemaphoreGive(g_lock);

    if (!active || req_id.empty()) {
        return;
    }
    // The relay only accepts voice.turn.abort while the turn is RECEIVING
    // (pre-FINAL). Identity-bound: the transport ignores a stale request id.
    wqn::stdpro_ws::AbortTurn(req_id, turn_gen);
    ESP_LOGI(kTag, "turn aborted req=%s gen=%lu",
             req_id.c_str(), static_cast<unsigned long>(turn_gen));
}

void AgentVoiceRequestCancel()
{
    g_cancel_requested.store(true, std::memory_order_release);
}

bool AgentVoiceCancelRequested()
{
    return g_cancel_requested.load(std::memory_order_acquire);
}

void AgentVoiceClearCancel()
{
    g_cancel_requested.store(false, std::memory_order_release);
}

}  // namespace wqn

#else  // !CONFIG_WQN_AI_ENABLE

// [agent-voice] AI features compiled out. The Agent tier is unreachable in
// this configuration (CONFIG_WQN_AGENT_ENABLE depends on WQN_AI_ENABLE), so
// every entry point refuses without touching the network. Mirrors the stub
// half of ai_session.cpp.
namespace wqn {

void AgentVoiceRegisterDeltaCallback(WqnAiSseCallback, void*) {}

esp_err_t AgentVoiceTurnStart(const std::string&, const std::string&, uint32_t*)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t AgentVoiceTurnFinalize(int, uint32_t, std::string*)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void AgentVoiceTurnAbort() {}
void AgentVoiceRequestCancel() {}
bool AgentVoiceCancelRequested() { return false; }
void AgentVoiceClearCancel() {}

}  // namespace wqn

#endif  // CONFIG_WQN_AI_ENABLE
