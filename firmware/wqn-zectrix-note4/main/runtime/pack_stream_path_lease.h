#pragma once

#include <cstddef>
#include <cstdint>

namespace wqn::runtime {

constexpr size_t kPackStreamPathCapacity = 8;
constexpr size_t kPackStreamPathMaxBytes = 96;

// Task-context admission for a pack's temporary path. The caller must keep
// path valid and immutable until this object is destroyed (declare the lease
// after the context owning the string). Empty/oversized/duplicate/full
// admission is false. The length bound keeps critical-section compares short.
// Acquire before Begin is queued and retain through Commit/Abort. This guards
// competing streams; StorageTempFileLease separately protects generic reclaim.
class PackStreamPathLease {
public:
    explicit PackStreamPathLease(const char* path);
    ~PackStreamPathLease();
    PackStreamPathLease(const PackStreamPathLease&) = delete;
    PackStreamPathLease& operator=(const PackStreamPathLease&) = delete;
    explicit operator bool() const { return slot_ != kInvalidSlot; }

private:
    static constexpr uint8_t kInvalidSlot = UINT8_MAX;
    uint8_t slot_ = kInvalidSlot;
};

// A destructive pack-cache reset must defer while any path is registered.
// Check inside its StorageService transaction before deleting any file.
bool HasActivePackStreamPath();

}  // namespace wqn::runtime
