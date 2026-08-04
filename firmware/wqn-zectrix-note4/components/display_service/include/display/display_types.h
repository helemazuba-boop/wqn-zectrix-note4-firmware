#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "esp_err.h"

namespace wqn::display {

using DisplayRevision = uint64_t;

constexpr DisplayRevision kInvalidDisplayRevision = 0;
constexpr std::size_t kDisplayResultQueueDepth = 16;

// The drawing schedule describes what changed; this requirement describes the
// weakest waveform that is still safe after several intents are coalesced.
enum class WaveformRequirement : uint8_t {
    kAuto = 0,
    kPartial = 1,
    kFull = 2,
};

struct DisplayIntent {
    DisplayRevision revision = kInvalidDisplayRevision;
    WaveformRequirement waveform = WaveformRequirement::kAuto;
    uint32_t deadline_tick = 0;
    uint32_t reason_mask = 0;
};

enum class DisplayStatus : uint8_t {
    kPresented,
    kSuperseded,
    kFailed,
};

// Trivially copyable: values are transported through a bounded FreeRTOS queue.
// presented_revision identifies the pixels believed to be on the panel when
// this terminal result was emitted. replacement_revision is only populated for
// kSuperseded. dropped_below_revision is the authoritative watermark from the
// EPD owner: every accepted revision <= it lost its terminal result (e.g. the
// result queue was full or the bounded wait timed out) and consumers must
// evict matching ledger entries. kInvalidDisplayRevision means "no loss".
struct DisplayResult {
    DisplayRevision revision = kInvalidDisplayRevision;
    DisplayStatus status = DisplayStatus::kFailed;
    DisplayRevision presented_revision = kInvalidDisplayRevision;
    DisplayRevision replacement_revision = kInvalidDisplayRevision;
    DisplayRevision dropped_below_revision = kInvalidDisplayRevision;
    esp_err_t error = ESP_FAIL;
};

struct DisplaySubmission {
    bool accepted = false;
    DisplayRevision revision = kInvalidDisplayRevision;
    WaveformRequirement waveform = WaveformRequirement::kAuto;
    uint32_t deadline_tick = 0;
};

static_assert(std::is_trivially_copyable_v<DisplayResult>,
              "DisplayResult must remain safe for FreeRTOS queue copies");

}  // namespace wqn::display
