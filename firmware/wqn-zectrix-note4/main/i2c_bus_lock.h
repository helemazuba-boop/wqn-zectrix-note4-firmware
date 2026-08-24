#ifndef I2C_BUS_LOCK_H
#define I2C_BUS_LOCK_H

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

// The Note4 shares one I2C_NUM_0 bus between the ES8311 codec, the PCF8563
// RTC and (physically) the ST25DV NFC tag. esp-idf i2c_master transactions
// are not safe to issue from two tasks on the same bus handle: the loser
// fails with ESP_ERR_INVALID_STATE while the bus is busy. Every driver that
// talks to the shared bus must hold this lock around each synchronous
// transaction. Mirrors the official zectrix-note4-epd-demo i2c_bus_lock.
esp_err_t LockI2cBus(const char* owner, TickType_t timeout_ticks = portMAX_DELAY);
void UnlockI2cBus();

class ScopedI2cBusLock {
public:
    explicit ScopedI2cBusLock(const char* owner, TickType_t timeout_ticks = portMAX_DELAY);
    ~ScopedI2cBusLock();

    bool locked() const { return status_ == ESP_OK; }
    esp_err_t status() const { return status_; }

private:
    esp_err_t status_ = ESP_FAIL;
};

#endif  // I2C_BUS_LOCK_H
