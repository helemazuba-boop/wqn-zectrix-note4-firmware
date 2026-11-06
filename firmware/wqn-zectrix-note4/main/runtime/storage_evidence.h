#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace wqn::runtime {

// Logical Word/Note files and selected typed NVS values, never a Flash/NVS image.
// All numeric fields in the wire format use little endian, without C padding.
constexpr size_t kStorageEvidenceMaxEntities = 512;
constexpr size_t kStorageEvidenceMaxFrameBytes = 523;
constexpr uint64_t kStorageEvidenceMaxFileBytes = 16U * 1024U * 1024U;
constexpr uint64_t kStorageEvidenceMaxTotalBytes = 32U * 1024U * 1024U;
constexpr size_t kStorageEvidenceMaxNvsBytes = 64U * 1024U;

struct StorageEvidenceIdentity {
    std::array<uint8_t, 16> capture_id{};
    std::array<uint8_t, 6> mac{};
    std::array<uint8_t, 32> firmware_elf_sha256{};
};

struct StorageEvidenceSink {
    // Called synchronously on the internal-stack storage owner. Must copy or
    // consume the bytes before return; must not reenter any storage API.
    esp_err_t (*emit)(const uint8_t*, size_t, void*) = nullptr;
    void* context = nullptr;
    bool (*cancelled)(void*) = nullptr;
    void* cancel_context = nullptr;
    // Cooperative absolute deadline: checked between reads/frames. It cannot
    // bound a VFS/NVS call or a blocking sink. Transport must enforce its budget.
    int64_t deadline_us = 0;
};

struct StorageEvidenceSummary {
    esp_err_t error = ESP_FAIL;
    uint32_t frames = 0;
    uint32_t entities = 0;
    uint64_t data_bytes = 0;
    bool capture_complete = false;  // Not transport ACK, identity or repair authority.
    std::array<uint8_t, 32> transcript_sha256{};
};

// One-way publication at EnterStorageRecoveryMode, which never resumes business
// startup. Does no I/O and creates no task. A second publication is rejected.
esp_err_t EnterStorageEvidenceBootRecovery(esp_err_t error, uint32_t observed_generation);

// Caller must already own ONE StorageService transaction for the ENTIRE call.
// Rejects normal business mode, unmounted SPIFFS and active stream/temp leases.
// Never mounts, initializes, migrates, repairs, writes, erases or reads keys
// outside the explicit allowlist. Reads and rehashes all retained source bytes.
// The supplied identity is only a claim; receiver must pin it independently.
// No serial/UI/network trigger is installed by this API.
esp_err_t CaptureStorageEvidenceOnOwner(const StorageEvidenceIdentity& identity,
    const StorageEvidenceSink& sink, StorageEvidenceSummary* summary);

}  // namespace wqn::runtime
