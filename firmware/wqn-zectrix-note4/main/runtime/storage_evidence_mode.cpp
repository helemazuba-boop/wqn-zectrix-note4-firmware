#include "runtime/storage_evidence.h"

#include <atomic>
#include "services/storage_service.h"

namespace wqn::runtime {
namespace {
enum class Mode : uint8_t { kUnset, kPublishing, kBootRecovery };
std::atomic<Mode> g_mode{Mode::kUnset};
StorageEvidenceBootContext g_boot;
}

esp_err_t EnterStorageEvidenceBootRecovery(esp_err_t error, uint32_t observed_generation)
{
    Mode expected = Mode::kUnset;
    if (!g_mode.compare_exchange_strong(expected, Mode::kPublishing, std::memory_order_acq_rel))
        return ESP_ERR_INVALID_STATE;
    g_boot = {error, observed_generation};
    g_mode.store(Mode::kBootRecovery, std::memory_order_release);
    return ESP_OK;
}

esp_err_t ReadStorageEvidenceBootContextOnOwner(StorageEvidenceBootContext* context)
{
    if (context == nullptr) return ESP_ERR_INVALID_ARG;
    *context = {};
    if (!services::IsStorageServiceTask() ||
        g_mode.load(std::memory_order_acquire) != Mode::kBootRecovery)
        return ESP_ERR_INVALID_STATE;
    *context = g_boot;
    return ESP_OK;
}
}  // namespace wqn::runtime
