#include "runtime/storage_temp_file_lease.h"

#include <atomic>
#include <cstdint>

namespace wqn::runtime {
namespace {
std::atomic<uint32_t> g_temp_file_leases{0};
}

StorageTempFileLease::StorageTempFileLease()
{
    g_temp_file_leases.fetch_add(1, std::memory_order_acq_rel);
}

StorageTempFileLease::~StorageTempFileLease()
{
    g_temp_file_leases.fetch_sub(1, std::memory_order_acq_rel);
}

bool HasActiveStorageTempFileLease()
{
    return g_temp_file_leases.load(std::memory_order_acquire) != 0;
}

}  // namespace wqn::runtime
