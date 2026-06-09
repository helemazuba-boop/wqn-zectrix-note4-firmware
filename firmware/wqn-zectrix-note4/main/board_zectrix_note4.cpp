#include "board_zectrix_note4.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "power_manager.h"

namespace {

constexpr char kTag[] = "wqn_board";

constexpr gpio_num_t kEpdPower = GPIO_NUM_6;
constexpr gpio_num_t kEpdBusy = GPIO_NUM_8;
constexpr gpio_num_t kEpdReset = GPIO_NUM_9;
constexpr gpio_num_t kEpdDc = GPIO_NUM_10;
constexpr gpio_num_t kEpdCs = GPIO_NUM_11;
constexpr gpio_num_t kEpdSck = GPIO_NUM_12;
constexpr gpio_num_t kEpdMosi = GPIO_NUM_13;

constexpr gpio_num_t kAudioPower = GPIO_NUM_42;
constexpr gpio_num_t kAudioAmp = GPIO_NUM_46;
constexpr gpio_num_t kLed = GPIO_NUM_3;
constexpr gpio_num_t kNfcPower = GPIO_NUM_21;
constexpr gpio_num_t kBoardPowerLatch = GPIO_NUM_17;

constexpr gpio_num_t kPageUp = GPIO_NUM_39;
constexpr gpio_num_t kPageDownAndPowerDetect = GPIO_NUM_18;
constexpr gpio_num_t kConfirm = GPIO_NUM_0;
constexpr gpio_num_t kChargeDetect = GPIO_NUM_2;
constexpr gpio_num_t kChargeFull = GPIO_NUM_1;
constexpr gpio_num_t kRtcInt = GPIO_NUM_5;
constexpr gpio_num_t kNfcFd = GPIO_NUM_7;

esp_err_t ConfigureOutputs(uint64_t pin_mask)
{
    gpio_config_t config = {};
    config.pin_bit_mask = pin_mask;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

esp_err_t ConfigureInputs(uint64_t pin_mask, gpio_pullup_t pull_up = GPIO_PULLUP_ENABLE)
{
    gpio_config_t config = {};
    config.pin_bit_mask = pin_mask;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = pull_up;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

}  // namespace

namespace wqn {

esp_err_t InitZectrixNote4SafePins()
{
    wqn::ReleaseDeepSleepHolds();

    const uint64_t disabled_power_outputs =
        (1ULL << kEpdPower) |
        (1ULL << kAudioPower) |
        (1ULL << kAudioAmp) |
        (1ULL << kNfcPower) |
        (1ULL << kLed) |
        (1ULL << kBoardPowerLatch);

    ESP_RETURN_ON_ERROR(ConfigureOutputs(disabled_power_outputs), kTag, "configure power outputs");

    gpio_set_level(kEpdPower, 0);
    gpio_set_level(kAudioPower, 0);
    gpio_set_level(kAudioAmp, 0);
    gpio_set_level(kNfcPower, 0);
    gpio_set_level(kLed, 1);
    gpio_set_level(kBoardPowerLatch, 1);

    const uint64_t epd_bus_pins =
        (1ULL << kEpdBusy) |
        (1ULL << kEpdReset) |
        (1ULL << kEpdDc) |
        (1ULL << kEpdCs) |
        (1ULL << kEpdSck) |
        (1ULL << kEpdMosi);
    ESP_RETURN_ON_ERROR(ConfigureInputs(epd_bus_pins, GPIO_PULLUP_DISABLE), kTag, "configure EPD pins");

    const uint64_t button_and_status_inputs =
        (1ULL << kPageUp) |
        (1ULL << kPageDownAndPowerDetect) |
        (1ULL << kConfirm) |
        (1ULL << kChargeDetect) |
        (1ULL << kChargeFull) |
        (1ULL << kRtcInt) |
        (1ULL << kNfcFd);
    ESP_RETURN_ON_ERROR(ConfigureInputs(button_and_status_inputs), kTag, "configure input pins");

    ESP_LOGI(kTag, "safe pins ready: PWR_ON latch on, EPD/audio/amp/NFC off, LED off");
    return ESP_OK;
}

}  // namespace wqn
