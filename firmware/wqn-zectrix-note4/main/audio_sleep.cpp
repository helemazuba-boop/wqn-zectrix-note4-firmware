#include "audio_sleep.h"

#include <atomic>

#include "audio_capture.h"
#include "audio_player.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "flash_session.h"
#include "runtime/sleep_coordinator.h"

namespace {

constexpr char kTag[] = "wqn_audio_sleep";
constexpr gpio_num_t kAudioPower = GPIO_NUM_42;
constexpr gpio_num_t kAudioAmp = GPIO_NUM_46;

std::atomic<bool> g_sleep_prepared{false};

void HoldAudioOutput(gpio_num_t pin, int level)
{
    gpio_hold_dis(pin);
    gpio_set_level(pin, level);
    gpio_hold_en(pin);
}

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
        if (IsFlashSessionActive()) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(StopFlashSession());
        }
        if (IsAudioCaptureRunning()) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(StopAudioCapture(nullptr));
        }
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

    // The codec rail is kept warm during normal runtime, but both codec and
    // amplifier must be off for deep sleep. This adapter is the sole M3 sleep
    // path touching the audio rails; PowerCoordinator no longer does so.
    HoldAudioOutput(kAudioAmp, 0);
    HoldAudioOutput(kAudioPower, 0);
    g_sleep_prepared.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "audio prepared for sleep: generation=%u mode=%s",
             static_cast<unsigned>(command.generation),
             power::SleepModeName(command.mode));
    return ESP_OK;
}

void RollbackAudioAfterSleepAbort()
{
    if (!g_sleep_prepared.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    HoldAudioOutput(kAudioPower, 1);
    HoldAudioOutput(kAudioAmp, 0);
    ESP_LOGI(kTag, "audio sleep preparation rolled back");
}

void ReleaseAudioDeepSleepHolds()
{
    gpio_hold_dis(kAudioPower);
    gpio_hold_dis(kAudioAmp);
    g_sleep_prepared.store(false, std::memory_order_release);
}

}  // namespace wqn
