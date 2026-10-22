#include "pcf8563.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_lock.h"
#include "error_recorder.h"

namespace {

constexpr char kTag[] = "wqn_pcf8563";
// The driver takes milliseconds; only the shared FreeRTOS mutex takes ticks.
// These are per-acquisition/per-transfer limits, not an end-to-end wake budget.
constexpr int kI2cTransferTimeoutMs = 100;
constexpr TickType_t kI2cLockWaitTicks = pdMS_TO_TICKS(100);
static_assert(kI2cLockWaitTicks > 0);

constexpr uint8_t kControlStatus1Reg = 0x00;
constexpr uint8_t kControlStatus2Reg = 0x01;
constexpr uint8_t kTimerControlReg = 0x0E;
constexpr uint8_t kTimerValueReg = 0x0F;
constexpr uint8_t kStopBit = 0x20;
constexpr uint8_t kTimerEnable = 0x80;
constexpr uint8_t kTimerClock1Hz = 0x02;
constexpr uint8_t kAlarmFlag = 0x08;
constexpr uint8_t kTimerFlag = 0x04;
constexpr uint8_t kTimerInterruptEnable = 0x01;

i2c_master_bus_handle_t g_bus = nullptr;
i2c_master_dev_handle_t g_dev = nullptr;
bool g_initialized = false;

int DecodeBcd(uint8_t bcd)
{
    return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

uint8_t EncodeBcd(int value)
{
    const int tens = value / 10;
    const int ones = value % 10;
    return static_cast<uint8_t>((tens << 4) | ones);
}

bool DeadlineExpired(int64_t deadline_us)
{
    return deadline_us > 0 && esp_timer_get_time() >= deadline_us;
}

TickType_t LockWaitTicks(int64_t deadline_us)
{
    if (deadline_us <= 0) return kI2cLockWaitTicks;
    const int64_t remaining_us = std::max<int64_t>(0, deadline_us - esp_timer_get_time());
    const int64_t remaining_ms = std::min<int64_t>(remaining_us / 1000, 100);
    return std::min(kI2cLockWaitTicks, pdMS_TO_TICKS(remaining_ms));
}

int TransferTimeoutMs(int64_t deadline_us)
{
    if (deadline_us <= 0) return kI2cTransferTimeoutMs;
    return static_cast<int>(std::clamp<int64_t>(
        (deadline_us - esp_timer_get_time()) / 1000, 0, kI2cTransferTimeoutMs));
}

esp_err_t I2cWriteReg(uint8_t reg, uint8_t data, int64_t deadline_us = 0)
{
    if (DeadlineExpired(deadline_us)) return ESP_ERR_TIMEOUT;
    uint8_t buf[2] = {reg, data};
    // [i2c-bus-lock] Shared with the ES8311 codec; serialize every
    // synchronous transaction (see i2c_bus_lock.h).
    ScopedI2cBusLock bus_lock("pcf8563_write", LockWaitTicks(deadline_us));
    if (!bus_lock.locked()) {
        return bus_lock.status();
    }
    const int timeout_ms = TransferTimeoutMs(deadline_us);
    if (timeout_ms == 0 || DeadlineExpired(deadline_us)) return ESP_ERR_TIMEOUT;
    const esp_err_t result = i2c_master_transmit(g_dev, buf, sizeof(buf), timeout_ms);
    return DeadlineExpired(deadline_us) ? ESP_ERR_TIMEOUT : result;
}

esp_err_t I2cReadReg(uint8_t reg, uint8_t* data, size_t len, int64_t deadline_us = 0)
{
    if (DeadlineExpired(deadline_us)) return ESP_ERR_TIMEOUT;
    ScopedI2cBusLock bus_lock("pcf8563_read", LockWaitTicks(deadline_us));
    if (!bus_lock.locked()) {
        return bus_lock.status();
    }
    const int timeout_ms = TransferTimeoutMs(deadline_us);
    if (timeout_ms == 0 || DeadlineExpired(deadline_us)) return ESP_ERR_TIMEOUT;
    const esp_err_t result = i2c_master_transmit_receive(g_dev, &reg, 1, data, len, timeout_ms);
    return DeadlineExpired(deadline_us) ? ESP_ERR_TIMEOUT : result;
}

}  // namespace

namespace wqn {

bool Pcf8563InitWithBus(i2c_master_bus_handle_t bus)
{
    if (g_initialized) {
        return true;
    }
    if (bus == nullptr) {
        ESP_LOGE(kTag, "null I2C bus handle");
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kPcf8563Addr;
    dev_cfg.scl_speed_hz = 100000;

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &g_dev);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "add device failed: %s", esp_err_to_name(err));
        return false;
    }

    g_bus = bus;
    g_initialized = true;
    ESP_LOGI(kTag, "PCF8563 attached to shared I2C bus");
    return true;
}

bool Pcf8563ReadTime(int* year, int* month, int* day, int* hour, int* min, int* sec)
{
    if (!g_initialized) {
        return false;
    }

    uint8_t regs[7] = {};
    if (I2cReadReg(0x02, regs, 7) != ESP_OK) {
        return false;
    }

    const uint8_t vl_bit = regs[0] & 0x80;
    if (vl_bit != 0) {
        ESP_LOGW(kTag, "PCF8563 clock integrity lost (VL flag set)");
        // [dev-diag] Wall clock untrusted — a top user-visible condition.
        wqn::RecordError("rtc", "clock integrity lost (VL)");
        return false;
    }

    if (sec != nullptr) *sec = DecodeBcd(regs[0] & 0x7F);
    if (min != nullptr) *min = DecodeBcd(regs[1] & 0x7F);
    if (hour != nullptr) *hour = DecodeBcd(regs[2] & 0x3F);
    if (day != nullptr) *day = DecodeBcd(regs[3] & 0x3F);
    if (month != nullptr) *month = DecodeBcd(regs[5] & 0x1F) - 1;
    if (year != nullptr) *year = DecodeBcd(regs[6]) + 100;

    return true;
}

bool Pcf8563WriteTime(int year, int month, int day, int hour, int min,
                      int sec, int weekday)
{
    if (!g_initialized) {
        return false;
    }
    if (year < 100 || year > 199 || month < 0 || month > 11 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        min < 0 || min > 59 || sec < 0 || sec > 59 ||
        weekday < 0 || weekday > 6) {
        ESP_LOGW(kTag,
                 "rejecting out-of-range RTC write: year=%d month=%d day=%d "
                 "hour=%d min=%d sec=%d wday=%d",
                 year, month, day, hour, min, sec, weekday);
        return false;
    }

    // Register payload for the burst starting at 0x02. Writing seconds with
    // bit7 = 0 clears the VL integrity flag; the month byte keeps the century
    // bit low so the stored year stays within 2000-2099.
    const uint8_t time_regs[7] = {
        EncodeBcd(sec),                          // 02 seconds, VL = 0
        EncodeBcd(min),                          // 03 minutes
        EncodeBcd(hour),                         // 04 hours, 24h mode
        EncodeBcd(day),                          // 05 day of month
        EncodeBcd(weekday),                      // 06 weekday, 0 = Sunday
        EncodeBcd(month + 1),                    // 07 month, century = 0
        EncodeBcd(year % 100),                   // 08 year
    };

    // One bus-lock hold covers STOP -> burst -> release so an audio transaction
    // cannot interleave while the prescaler is halted.
    ScopedI2cBusLock bus_lock("pcf8563_settime", kI2cLockWaitTicks);
    if (!bus_lock.locked()) {
        return false;
    }

    uint8_t stop_on[2] = {kControlStatus1Reg, kStopBit};
    esp_err_t err = i2c_master_transmit(g_dev, stop_on, sizeof(stop_on),
                                        kI2cTransferTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "RTC STOP set failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t burst[8] = {0x02};
    std::memcpy(&burst[1], time_regs, sizeof(time_regs));
    err = i2c_master_transmit(g_dev, burst, sizeof(burst), kI2cTransferTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "RTC time burst write failed: %s", esp_err_to_name(err));
        // Best effort: a failed release can still leave the clock stopped.
        uint8_t stop_off[2] = {kControlStatus1Reg, 0x00};
        const esp_err_t release_err = i2c_master_transmit(
            g_dev, stop_off, sizeof(stop_off), kI2cTransferTimeoutMs);
        if (release_err != ESP_OK) {
            ESP_LOGW(kTag, "RTC STOP cleanup failed: %s", esp_err_to_name(release_err));
            wqn::RecordError("rtc", "STOP cleanup failed: %s", esp_err_to_name(release_err));
        }
        return false;
    }

    uint8_t stop_off[2] = {kControlStatus1Reg, 0x00};
    err = i2c_master_transmit(g_dev, stop_off, sizeof(stop_off), kI2cTransferTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "RTC STOP release failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t readback[7] = {};
    err = i2c_master_transmit_receive(g_dev, &burst[0], 1, readback,
                                      sizeof(readback), kI2cTransferTimeoutMs);
    if (err != ESP_OK || std::memcmp(readback, time_regs, sizeof(time_regs)) != 0) {
        ESP_LOGW(kTag, "RTC write verify failed: err=%s regs=%02x%02x%02x%02x%02x%02x%02x",
                 esp_err_to_name(err),
                 readback[0], readback[1], readback[2],
                 readback[3], readback[4], readback[5], readback[6]);
        // [dev-diag] Written time could not be confirmed — keep the detail
        // that the boolean return drops.
        wqn::RecordError("rtc", "time write verify failed");
        return false;
    }

    ESP_LOGI(kTag, "PCF8563 time written and verified");
    return true;
}

bool Pcf8563ConfigureTimerWake(uint8_t seconds)
{
    return Pcf8563ConfigureTimerWake(seconds, 0);
}

bool Pcf8563ConfigureTimerWake(uint8_t seconds, int64_t deadline_us)
{
    if (!g_initialized) {
        return false;
    }

    if (seconds == 0) {
        ESP_LOGW(kTag, "timer wake seconds=0, not configuring");
        return false;
    }

    // Stop the countdown before changing its value. Control_status_2 is
    // deliberately written as a complete value: AF/TF clear when written as
    // zero, unused bits must be zero, and this firmware does not use alarms.
    if (I2cWriteReg(kTimerControlReg, kTimerClock1Hz, deadline_us) != ESP_OK) {
        return false;
    }
    if (I2cWriteReg(kControlStatus2Reg, 0x00, deadline_us) != ESP_OK) {
        return false;
    }
    if (I2cWriteReg(kTimerValueReg, seconds, deadline_us) != ESP_OK) {
        return false;
    }
    if (I2cWriteReg(kControlStatus2Reg, kTimerInterruptEnable, deadline_us) != ESP_OK) {
        return false;
    }
    if (I2cWriteReg(kTimerControlReg, kTimerEnable | kTimerClock1Hz, deadline_us) != ESP_OK) {
        return false;
    }

    uint8_t ctrl2 = 0;
    if (I2cReadReg(kControlStatus2Reg, &ctrl2, 1, deadline_us) != ESP_OK ||
        (ctrl2 & (kAlarmFlag | kTimerFlag)) != 0 ||
        (ctrl2 & kTimerInterruptEnable) == 0) {
        ESP_LOGW(kTag, "timer wake arm verification failed: ctrl2=0x%02x", ctrl2);
        return false;
    }

    ESP_LOGI(kTag, "PCF8563 timer wake configured: %u seconds", static_cast<unsigned>(seconds));
    return true;
}

bool Pcf8563ReadInterruptFlags(Pcf8563InterruptFlags* flags)
{
    if (!g_initialized || flags == nullptr) {
        return false;
    }

    uint8_t ctrl2 = 0;
    if (I2cReadReg(kControlStatus2Reg, &ctrl2, 1) != ESP_OK) {
        return false;
    }
    flags->alarm = (ctrl2 & kAlarmFlag) != 0;
    flags->timer = (ctrl2 & kTimerFlag) != 0;
    return true;
}

bool Pcf8563DisableTimerWakeAndClearFlags()
{
    return Pcf8563DisableTimerWakeAndClearFlags(0);
}

bool Pcf8563DisableTimerWakeAndClearFlags(int64_t deadline_us)
{
    if (!g_initialized) {
        return false;
    }

    if (I2cWriteReg(kTimerControlReg, kTimerClock1Hz, deadline_us) != ESP_OK ||
        I2cWriteReg(kControlStatus2Reg, 0x00, deadline_us) != ESP_OK) {
        return false;
    }

    uint8_t ctrl2 = 0;
    if (I2cReadReg(kControlStatus2Reg, &ctrl2, 1, deadline_us) != ESP_OK) {
        return false;
    }
    const bool cleared = (ctrl2 & (kAlarmFlag | kTimerFlag | kTimerInterruptEnable)) == 0;
    if (!cleared) {
        ESP_LOGW(kTag, "timer wake clear verification failed: ctrl2=0x%02x", ctrl2);
    }
    return cleared;
}

}  // namespace wqn
