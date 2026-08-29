#pragma once

#include <cstdint>
#include <string>
#include "esp_err.h"
#include "wqn_api.h"

namespace wqn::stdpro_ws {

enum class FinalHandoffResult {
    kFinalSent,           // FINAL confirmed sent over WS; transition to REMOTE_HANDOFF
    kDefinitelyNotSent,   // No FINAL send attempt began; safe to HTTP fallback
    kAmbiguous,           // Send timed out/indeterminate; DO NOT HTTP fallback
};

enum class TurnStreamObservation : uint8_t {
    kNone,
    kTurnDoneObserved,
    kRemoteErrorObserved,
    kTransportLost,
    kAborted,
};

enum class TurnReleaseState : uint8_t {
    kNone,
    kReleased,
    kTransportLost,
    kAborted,
    kPreFinalFailed,
    kHandoffAmbiguous,
    kLocallyAbandoned,
};

enum class TurnWaitStatus : uint8_t {
    kTerminalObserved,
    kTimedOut,
};

struct TurnWaitResult {
    TurnWaitStatus wait_status = TurnWaitStatus::kTimedOut;
    TurnReleaseState release_state = TurnReleaseState::kNone;
    TurnStreamObservation stream_observation = TurnStreamObservation::kNone;
    uint32_t turn_gen = 0;
};

esp_err_t EnsureConnected(const std::string& token, uint32_t timeout_ms);
bool IsConnected();
void Disconnect();

esp_err_t StartTurn(const std::string& request_id,
                    const std::string& tier,
                    const std::string& conversation_id,
                    bool enable_thinking,
                    const char* reasoning_effort,
                    uint32_t* out_turn_gen = nullptr);

void PushPcm(const int16_t* samples, size_t count);

FinalHandoffResult SendFinalAndWait(const std::string& request_id,
                                   int duration_ms,
                                   uint32_t timeout_ms);

void AbortTurn(const std::string& request_id, uint32_t target_turn_gen = 0);

void SetSseCallback(WqnAiSseCallback cb, void* user_ctx);

TurnWaitResult WaitForTurnRelease(uint32_t expected_turn_gen,
                                 const std::string& expected_request_id,
                                 uint32_t timeout_ms);

bool WaitForTurnComplete(uint32_t timeout_ms);
void SignalTurnComplete();

} // namespace wqn::stdpro_ws
