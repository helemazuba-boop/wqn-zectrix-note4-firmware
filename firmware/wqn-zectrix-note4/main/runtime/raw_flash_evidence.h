#pragma once

#include "runtime/storage_evidence.h"

namespace wqn::runtime {
constexpr uint32_t kRawFlashEvidenceMaxBytes = 64U * 1024U * 1024U;
constexpr uint32_t kRawFlashEvidenceChunkBytes = 384;

struct RawFlashEvidenceRequest {
    StorageEvidenceIdentity identity;
    // Caller must obtain fresh device consent for credential-bearing bytes.
    // This defends accidental calls; it is not proof of physical presence.
    bool sensitive_main_flash_confirmed = false;
};

struct RawFlashEvidenceSummary {
    esp_err_t error = ESP_FAIL;
    uint32_t configured_bytes = 0, physical_bytes = 0;
    uint32_t frames = 0, captured_bytes = 0, rechecked_bytes = 0;
    uint32_t failed_offset = UINT32_MAX;
    bool flash_encryption_enabled = false;
    bool source_rechecked = false, capture_complete = false;
    std::array<uint8_t, 32> image_sha256{}, recheck_sha256{}, transcript_sha256{};
};

// ONE storage-owner transaction in boot recovery, mounted or unmounted.
// Reads the default main Flash via esp_flash_read (raw ciphertext where
// encrypted), then rereads its entire reported range. No init/mount, NVS,
// filesystem, write, erase, decrypt, repair, task or transport is installed.
// Rejects unequal configured/physical sizes rather than claiming a full image.
// Cooperative deadline/cancel cannot preempt an SDK read or a blocking sink;
// the future transport must bound its own writes and delivery acknowledgments.
// A matching reread is not an atomic pre-boot snapshot or hardware identity.
esp_err_t CaptureRawFlashEvidenceOnOwner(const RawFlashEvidenceRequest& request,
    const StorageEvidenceSink& sink, RawFlashEvidenceSummary* summary);
}  // namespace wqn::runtime
