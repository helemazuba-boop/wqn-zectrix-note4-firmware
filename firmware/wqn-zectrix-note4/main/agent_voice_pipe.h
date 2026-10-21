#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "wqn_api.h"

// [agent-voice] Agent-tier voice input over the shared Std/Pro WebSocket.
//
// The Agent tier records a clip and transcribes it through the relay's
// tier=agent route -- the same WFLV PCM path the Std/Pro tiers use, with the
// ASR-only pipeline behind it. This module owns the transport-side turn: the
// request identity, the transport's SSE callback slot, the text accumulated
// from the stream and the cancel flags. opencode_session owns the capture and
// the UI around it.
//
// The STD driver in ai_session.cpp keeps its own turn lifecycle. The two never
// overlap (one transport, one turn at a time), and each installs its own SSE
// callback before starting a turn because the transport holds a single,
// never-unregistered callback slot.
namespace wqn {

// Store the caller's observer for the live turn and install this module's SSE
// trampoline on the transport. Must be called before AgentVoiceTurnStart; the
// trampoline forwards every event of the active turn to the observer after
// updating its own accumulation state.
void AgentVoiceRegisterDeltaCallback(WqnAiSseCallback cb, void* ctx);

// Ensure the transport is connected, arm the one-shot preroll replay and start
// a tier=agent turn. Failures are reported so the caller can fall back to the
// batch endpoint; they are never fatal.
esp_err_t AgentVoiceTurnStart(const std::string& token,
                              const std::string& request_id,
                              uint32_t* out_turn_gen);

// Send FINAL and wait for the ASR result. ESP_OK carries the transcript.
// asr.complete is the return point -- the transport observes turn.released on
// its own, so the confirmation gate never waits for it. ESP_ERR_INVALID_STATE
// means no turn was established or FINAL never left the device: the caller
// must use the batch endpoint. A soft cancel returns ESP_OK with an empty
// transcript; the caller checks AgentVoiceCancelRequested() to discard it.
esp_err_t AgentVoiceTurnFinalize(int duration_ms, uint32_t timeout_ms,
                                 std::string* out_transcript);

// Hard abort for the active turn. The relay only accepts voice.turn.abort
// while the turn is still RECEIVING (pre-FINAL); a no-op when nothing is
// active or the turn already moved past that state.
void AgentVoiceTurnAbort();

// Soft cancel: the finalize stops waiting and reports an empty transcript. The
// turn itself is left to finish on the transport's own time.
void AgentVoiceRequestCancel();
bool AgentVoiceCancelRequested();
void AgentVoiceClearCancel();

}  // namespace wqn
