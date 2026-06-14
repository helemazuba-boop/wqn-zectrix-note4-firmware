#include "pcf8563.h"

#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "wqn_pcf8563";

i2c_master_bus_handle_t g_bus = nullptr;
i2c_master_dev_handle_t g_dev = nullptr;
bool g_initialized = false;

int DecodeBcd(uint8_t bcd)
{
    return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

esp_err_t I2cWriteReg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(g_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

esp_err_t I2cReadReg(uint8_t reg, uint8_t* data, size_t len)
{
    return i2c_master_transmit_receive(g_dev, &reg, 1, data, len, pdMS_TO_TICKS(100));
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

bool Pcf8563Init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, int clk_hz)
{
    if (g_initialized) {
        return true;
    }

    if (port != I2C_NUM_0 && port != I2C_NUM_1) {
        port = I2C_NUM_0;
    }

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = port;
    bus_cfg.sda_io_num = sda;
    bus_cfg.scl_io_num = scl;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "new master bus failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!Pcf8563InitWithBus(g_bus)) {
        i2c_del_master_bus(g_bus);
        g_bus = nullptr;
        return false;
    }

    ESP_LOGI(kTag, "PCF8563 initialized with own bus: port=%d SDA=%d SCL=%d clk=%d",
             static_cast<int>(port), static_cast<int>(sda), static_cast<int>(scl), clk_hz);
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

bool Pcf8563ConfigureTimerWake(uint8_t seconds)
{
    if (!g_initialized) {
        return false;
    }

    if (seconds == 0) {
        ESP_LOGW(kTag, "timer wake seconds=0, not configuring");
        return false;
    }

    if (I2cWriteReg(0x0F, seconds) != ESP_OK) {
        return false;
    }

    if (I2cWriteReg(0x0E, 0x82) != ESP_OK) {
        return false;
    }

    uint8_t ctrl2 = 0;
    if (I2cReadReg(0x01, &ctrl2, 1) != ESP_OK) {
        return false;
    }

    ctrl2 = (ctrl2 & 0x1F) | 0x01;
    if (I2cWriteReg(0x01, ctrl2) != ESP_OK) {
        return false;
    }

    ESP_LOGI(kTag, "PCF8563 timer wake configured: %u seconds", static_cast<unsigned>(seconds));
    return true;
}

}  // namespace wqn
