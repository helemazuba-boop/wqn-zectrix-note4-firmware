#pragma once

#include "esp_err.h"
#include "power/sleep_protocol.h"

namespace wqn {

// Compatibility surface used by PowerCoordinator. Runtime audio GPIO and
// state changes are delegated to AudioService; callers outside this adapter
// must not manipulate the Note4 audio rails during sleep transitions.
esp_err_t PrepareAudioForSleep(const power::PrepareSleepCommand& command);
void RollbackAudioAfterSleepAbort(uint32_t generation);
void ReleaseAudioDeepSleepHolds();

}  // namespace wqn
