#pragma once

namespace wqn::runtime {

// Protect temporary files held across StorageService transactions. Publish
// before queuing the open, and retain until commit/abort has closed the file.
// New opens cannot execute during the reclaimer's owner transaction, so a
// lease acquired after its zero check cannot expose a new file to that scan.
// Task context only; no allocation, Flash access or sleep-policy effect.
class StorageTempFileLease {
public:
    StorageTempFileLease();
    ~StorageTempFileLease();
    StorageTempFileLease(const StorageTempFileLease&) = delete;
    StorageTempFileLease& operator=(const StorageTempFileLease&) = delete;
};

// Generic stale-temp reclamation must skip its scan while any lease is held.
// Cached-image eviction and bounded GC may still run on the storage owner.
bool HasActiveStorageTempFileLease();

}  // namespace wqn::runtime
