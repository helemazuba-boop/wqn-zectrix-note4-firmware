#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace wqn {

struct PlatformDiagnosticsSnapshot {
    bool flash_valid = false;
    uint32_t flash_size = 0;
    bool psram_valid = false;
    size_t psram_total = 0;
    size_t psram_free = 0;
    bool wifi_mac_valid = false;
    std::array<uint8_t, 6> wifi_mac = {};
};

void PrintBootDiagnostics();
bool ReadPlatformDiagnosticsSnapshot(PlatformDiagnosticsSnapshot* snapshot);

}  // namespace wqn
