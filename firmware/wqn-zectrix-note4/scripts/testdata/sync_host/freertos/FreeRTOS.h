#pragma once
#include <cstdint>
using TickType_t = uint32_t;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
constexpr uint32_t portTICK_PERIOD_MS=1;
#define pdMS_TO_TICKS(value) (value)
