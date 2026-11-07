#include "recovery_usb.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include "sdkconfig.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wqn::recovery_usb {
namespace {
std::atomic<TaskHandle_t> owner{nullptr};
std::atomic<FILE*> primary{nullptr};
int PrimaryLog(const char* format, va_list args) {
    FILE* stream = primary.load(std::memory_order_acquire);
    return stream != nullptr ? std::vfprintf(stream, format, args) : 0;
}
TickType_t Ticks(uint32_t ms) {
    const TickType_t ticks = pdMS_TO_TICKS(std::min<uint32_t>(ms, 50));
    return ms != 0 && ticks == 0 ? 1 : ticks;
}
}
esp_err_t Reserve()
{
#if CONFIG_ESP_CONSOLE_UART && CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    const TaskHandle_t task = owner.load(std::memory_order_acquire);
    if (task != nullptr) return task == xTaskGetCurrentTaskHandle() ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (usb_serial_jtag_is_driver_installed()) return ESP_ERR_INVALID_STATE;
    if (primary.load(std::memory_order_acquire) == nullptr) {
#if CONFIG_ESP_CONSOLE_UART_NUM == 0
        FILE* uart = std::fopen("/dev/uart/0", "w");
#elif CONFIG_ESP_CONSOLE_UART_NUM == 1
        FILE* uart = std::fopen("/dev/uart/1", "w");
#elif CONFIG_ESP_CONSOLE_UART_NUM == 2
        FILE* uart = std::fopen("/dev/uart/2", "w");
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
        if (uart == nullptr) return ESP_FAIL;
        // One-way reservation, kept even if driver allocation fails. A retry
        // reuses this stream instead of leaking/replacing a live log target.
        setvbuf(uart, nullptr, _IONBF, 0);
        primary.store(uart, std::memory_order_release);
        esp_log_set_vprintf(PrimaryLog);
        esp_rom_install_channel_putc(2, nullptr);
    }
    usb_serial_jtag_driver_config_t config{512, 512};
    const esp_err_t error = usb_serial_jtag_driver_install(&config);
    if (error != ESP_OK) return error;
    owner.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    // Driver remains installed until reboot: uninstalling from a migrated task
    // would violate IDF's same-CPU ISR teardown rule. Bounded TX/RX buffers and
    // a new session's leading newline safely delimit any old queued prefix.
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
int Write(const uint8_t* data, size_t size, uint32_t ms)
{
    if (owner.load(std::memory_order_acquire) != xTaskGetCurrentTaskHandle() ||
        owner.load(std::memory_order_acquire) == nullptr || data == nullptr || size == 0) return -1;
    return usb_serial_jtag_write_bytes(data, std::min<size_t>(size, 128), Ticks(ms));
}
int Read(uint8_t* data, size_t size, uint32_t ms)
{
    if (owner.load(std::memory_order_acquire) != xTaskGetCurrentTaskHandle() ||
        owner.load(std::memory_order_acquire) == nullptr || data == nullptr || size == 0) return -1;
    return usb_serial_jtag_read_bytes(data, std::min<size_t>(size, 128), Ticks(ms));
}
}  // namespace wqn::recovery_usb
