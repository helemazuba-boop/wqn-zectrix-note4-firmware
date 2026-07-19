#include "audio_sleep.h"

#include "audio_capture.h"
#include "audio_player.h"
#include "esp_timer.h"
#include "flash_session.h"
#include "runtime/sleep_coordinator.h"
#include "services/audio_service.h"

namespace {

bool DeadlineExpired(int64_t deadline_us)
{
    return deadline_us > 0 && esp_timer_get_time() >= deadline_us;
}

}  // namespace

namespace wqn {

esp_err_t PrepareAudioForSleep(const power::PrepareSleepCommand& command)
{
    if (DeadlineExpired(command.deadline_us)) {
        return ESP_ERR_TIMEOUT;
    }

    if (command.mode == power::SleepMode::kBatteryEmergency) {
        // Emergency shutdown has a hard hardware deadline. Do not wait for
        // WebSocket/task teardown here: AudioService first rejects new driver
        // operations, drains current I/O, and physically disables PA/codec.
        // Deep sleep then terminates the remaining software tasks.
        return services::PrepareAudioServiceForSleep(command);
    }

    if (IsAudioCaptureRunning() || IsFlashSessionActive() ||
        runtime::ActiveSleepBlockerCount(runtime::SleepBlocker::kAudio) != 0) {
        return DeadlineExpired(command.deadline_us)
            ? ESP_ERR_TIMEOUT
            : ESP_ERR_INVALID_STATE;
    }

    const esp_err_t playback_result = StopAudioPlayback();
    if (playback_result != ESP_OK) {
        return playback_result;
    }
    if (DeadlineExpired(command.deadline_us)) {
        return ESP_ERR_TIMEOUT;
    }

    return services::PrepareAudioServiceForSleep(command);
}

void RollbackAudioAfterSleepAbort(uint32_t generation)
{
    services::RollbackAudioServiceAfterSleepAbort(generation);
}

void ReleaseAudioDeepSleepHolds()
{
    services::ReleaseAudioServiceDeepSleepHolds();
}

}  // namespace wqn
