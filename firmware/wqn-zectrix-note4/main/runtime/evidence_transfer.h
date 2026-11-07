#pragma once
#include <array>
#include "runtime/storage_evidence.h"

namespace wqn::runtime {
struct EvidenceTransferIo {
    esp_err_t (*open)(void*) = nullptr;
    int (*write)(const uint8_t*, size_t, uint32_t, void*) = nullptr;
    int (*read)(uint8_t*, size_t, uint32_t, void*) = nullptr;
    void* context = nullptr;
};
struct EvidenceTransferResult {
    esp_err_t error = ESP_FAIL;
    uint32_t persisted_frames = 0, persisted_data_bytes = 0;
    bool commit_acknowledged = false;
    bool source_complete_reported = false;
    std::array<uint8_t, 32> manifest_sha256{};
};
// A single owner owns the whole synchronous session. Keep this 11 KiB object
// in static internal RAM, not on the storage-owner or coordinator stack.
class EvidenceTransfer {
public:
    esp_err_t Begin(const EvidenceTransferIo&, const StorageEvidenceIdentity&, char kind,
                    bool (*cancelled)(void*), void*, int64_t deadline_us);
    esp_err_t Emit(const uint8_t*, size_t);
    void Abort(esp_err_t error);
    const EvidenceTransferResult& result() const { return result_; }
private:
    esp_err_t Check(int64_t until) const;
    esp_err_t Write(const char*, int64_t);
    esp_err_t Line(int64_t);
    esp_err_t Flush(bool final);
    void Yield(bool stalled);
    EvidenceTransferIo io_;
    bool (*cancelled_)(void*) = nullptr;
    void* cancel_context_ = nullptr;
    int64_t deadline_ = 0;
    int64_t last_yield_ = 0;
    char kind_ = 0, capture_[33]{}, mac_[13]{}, firmware_[65]{};
    std::array<std::array<uint8_t, kStorageEvidenceMaxFrameBytes>, 16> window_{};
    std::array<uint16_t, 16> sizes_{};
    uint32_t next_ = 0, count_ = 0, pending_data_bytes_ = 0;
    bool pending_source_complete_ = false;
    char line_[1024]{};
    uint8_t input_[128]{};
    size_t input_at_ = 0, input_size_ = 0;
    EvidenceTransferResult result_;
};
}  // namespace wqn::runtime
