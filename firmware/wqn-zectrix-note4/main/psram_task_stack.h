#pragma once

#include <cstddef>

#include "freertos/FreeRTOS.h"

namespace wqn {

// Allocate a task stack, preferring PSRAM.
//
// Internal SRAM on this board is also the DMA-capable pool, and it is the
// binding constraint: the ledger added for this investigation reports
// `allocated 296016 / free 13095 / largest_free_block 12288` with only six
// free blocks, and after one AI turn `largest` falls to 800 B, which is below
// what the I2S driver needs -- every recording after the first fails with
// `i2s_alloc_dma_desc: allocate DMA buffer failed` / ESP_ERR_NO_MEM.
// 23 of 24 tasks have their stacks there, so moving the ones that can afford
// it to PSRAM is the only lever that returns a usable amount.
//
// PSRAM is NOT usable while the cache is disabled (see the note in
// AllocTaskStack's definition about which tasks may use this), so callers must
// have verified the task never performs flash or NVS access on its own stack.
//
// `bytes` is a byte count. Returns nullptr only if both PSRAM and internal
// allocation fail. `out_in_psram` may be nullptr.
StackType_t* AllocTaskStack(size_t bytes, const char* owner, bool* out_in_psram);

// xTaskCreateStatic takes its depth in StackType_t WORDS while xTaskCreate
// takes BYTES; this project has got that wrong more than once (see the
// kTransportTaskStackWords note in stdpro_ws_transport.cpp, and the
// kCaptureTaskStackBytes depth that used to be passed here as bytes). Every
// call site should route the conversion through this so the unit is stated
// once, at the point of use.
constexpr uint32_t TaskStackWords(size_t bytes)
{
    return static_cast<uint32_t>(bytes / sizeof(StackType_t));
}

}  // namespace wqn
