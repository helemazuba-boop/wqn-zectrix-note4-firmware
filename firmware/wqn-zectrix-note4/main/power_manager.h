#pragma once

#include "esp_err.h"

namespace wqn {

void LogWakeupCause();
void ReleaseDeepSleepHolds();
void NoteUserActivity();
void NoteEpdActivity();
bool IsUiIdleForSleep();
esp_err_t PrepareForDeepSleep();
void EnterDeepSleepIfEnabled();
void PowerOffEpdAfterIdleIfNeeded();
void ShutdownForBatteryDepleted();

}  // namespace wqn
