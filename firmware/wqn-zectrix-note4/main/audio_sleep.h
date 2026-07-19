#pragma once

#include "esp_err.h"
#include "power/sleep_protocol.h"

namespace wqn {

// M3 compatibility adapter. M6 will move these GPIO/I2S calls into the final
// AudioService task without changing the PrepareSleep contract.
esp_err_t PrepareAudioForSleep(const power::PrepareSleepCommand& command);
void RollbackAudioAfterSleepAbort();
void ReleaseAudioDeepSleepHolds();

}  // namespace wqn
