#include "runtime/pack_stream_path_lease.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wqn::runtime {
namespace {
static_assert(kPackStreamPathCapacity < UINT8_MAX);
const char* g_paths[kPackStreamPathCapacity] = {};
portMUX_TYPE g_path_lock = portMUX_INITIALIZER_UNLOCKED;
}

PackStreamPathLease::PackStreamPathLease(const char* path)
{
    if (path == nullptr || path[0] == '\0' ||
        ::strnlen(path, kPackStreamPathMaxBytes) == kPackStreamPathMaxBytes) return;
    taskENTER_CRITICAL(&g_path_lock);
    size_t available = kPackStreamPathCapacity;
    for (size_t i = 0; i < kPackStreamPathCapacity; ++i) {
        if (g_paths[i] != nullptr && std::strcmp(g_paths[i], path) == 0) {
            taskEXIT_CRITICAL(&g_path_lock);
            return;
        }
        if (g_paths[i] == nullptr && available == kPackStreamPathCapacity) {
            available = i;
        }
    }
    if (available < kPackStreamPathCapacity) {
        g_paths[available] = path;
        slot_ = static_cast<uint8_t>(available);
    }
    taskEXIT_CRITICAL(&g_path_lock);
}

PackStreamPathLease::~PackStreamPathLease()
{
    if (slot_ == kInvalidSlot) return;
    taskENTER_CRITICAL(&g_path_lock);
    g_paths[slot_] = nullptr;
    taskEXIT_CRITICAL(&g_path_lock);
}

bool HasActivePackStreamPath()
{
    taskENTER_CRITICAL(&g_path_lock);
    bool active = false;
    for (const char* path : g_paths) active = active || path != nullptr;
    taskEXIT_CRITICAL(&g_path_lock);
    return active;
}

}  // namespace wqn::runtime
