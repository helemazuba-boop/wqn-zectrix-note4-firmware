#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace wqn::recovery_usb {
// Called only after boot-recovery device consent, by one serialized owner.
// Reserves USB for evidence until reboot; routes ESP_LOG and ROM secondary
// output to the existing primary UART. Never initializes another UART.
esp_err_t Reserve();
int Write(const uint8_t* data, size_t size, uint32_t wait_ms);
int Read(uint8_t* data, size_t size, uint32_t wait_ms);
}  // namespace wqn::recovery_usb
