#include "services/storage_recovery_export.h"
#include <new>
#include <algorithm>
#include <iterator>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "recovery_usb.h"
#include "runtime/evidence_transfer.h"
#include "runtime/raw_flash_evidence.h"

namespace wqn::services {
namespace {
esp_err_t Open(void*) { return recovery_usb::Reserve(); }
int Write(const uint8_t* data, size_t size, uint32_t ms, void*) { return recovery_usb::Write(data, size, ms); }
int Read(uint8_t* data, size_t size, uint32_t ms, void*) { return recovery_usb::Read(data, size, ms); }
struct EmitContext {
    runtime::EvidenceTransfer* transfer;
    void (*progress)(uint32_t, void*);
    void* context;
};
esp_err_t Emit(const uint8_t* data, size_t size, void* pointer) {
    auto* context = static_cast<EmitContext*>(pointer);
    const esp_err_t error = context->transfer->Emit(data, size);
    if (context->progress != nullptr)
        context->progress(context->transfer->result().persisted_data_bytes, context->context);
    return error;
}
struct TransferOwner {
    runtime::EvidenceTransfer* value = nullptr;
    ~TransferOwner() { if (value != nullptr) { value->~EvidenceTransfer(); heap_caps_free(value); } }
};
}
esp_err_t RunStorageRecoveryExportOnOwner(StorageRecoveryExportKind kind,
    bool (*cancelled)(void*), void* context, int64_t deadline,
    void (*progress)(uint32_t, void*), StorageRecoveryExportResult* result)
{
    if (result == nullptr) return ESP_ERR_INVALID_ARG;
    *result = {};
    if (kind != StorageRecoveryExportKind::kRaw && kind != StorageRecoveryExportKind::kLogical)
        return result->error = ESP_ERR_INVALID_ARG;
    runtime::StorageEvidenceBootContext boot;
    if (runtime::ReadStorageEvidenceBootContextOnOwner(&boot) != ESP_OK)
        return result->error = ESP_ERR_INVALID_STATE;
    if (boot.preservation_only && kind != StorageRecoveryExportKind::kRaw)
        return result->error = ESP_ERR_INVALID_STATE;
    if (cancelled != nullptr && cancelled(context)) return result->error = ESP_ERR_INVALID_STATE;
    if (deadline <= esp_timer_get_time()) return result->error = ESP_ERR_TIMEOUT;
    runtime::StorageEvidenceIdentity identity;
    esp_fill_random(identity.capture_id.data(), identity.capture_id.size());
    const esp_err_t mac = esp_read_mac(identity.mac.data(), ESP_MAC_WIFI_STA);
    if (mac != ESP_OK) return result->error = mac;
    const esp_app_desc_t* app = esp_app_get_description();
    if (app == nullptr) return result->error = ESP_FAIL;
    std::copy(std::begin(app->app_elf_sha256), std::end(app->app_elf_sha256), identity.firmware_elf_sha256.begin());
    result->capture_id = identity.capture_id;
    TransferOwner transfer;
    void* memory = heap_caps_malloc(sizeof(runtime::EvidenceTransfer), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (memory == nullptr) return result->error = ESP_ERR_NO_MEM;
    transfer.value = new(memory) runtime::EvidenceTransfer;
    const runtime::EvidenceTransferIo io{Open, Write, Read, nullptr};
    esp_err_t error = transfer.value->Begin(io, identity, kind == StorageRecoveryExportKind::kRaw ? 'R' : 'L',
                                          cancelled, context, deadline);
    if (error != ESP_OK) { result->transfer_error = error; return result->error = error; }
    EmitContext emit{transfer.value, progress, context};
    const runtime::StorageEvidenceSink sink{Emit, &emit, cancelled, context, deadline};
    if (kind == StorageRecoveryExportKind::kRaw) {
        runtime::RawFlashEvidenceRequest request{identity, true};
        runtime::RawFlashEvidenceSummary summary;
        error = runtime::CaptureRawFlashEvidenceOnOwner(request, sink, &summary);
        result->source_complete = summary.source_rechecked;
    } else {
        runtime::StorageEvidenceSummary summary;
        error = runtime::CaptureStorageEvidenceOnOwner(identity, sink, &summary);
        result->source_complete = summary.capture_complete;
    }
    const auto& transport = transfer.value->result();
    result->source_error = error; result->transfer_error = transport.error;
    result->commit_acknowledged = transport.commit_acknowledged;
    result->manifest_sha256 = transport.manifest_sha256;
    result->persisted_data_bytes = transport.persisted_data_bytes;
    result->persisted_frames = transport.persisted_frames;
    if (transport.commit_acknowledged) result->source_complete = transport.source_complete_reported;
    if (error != ESP_OK) transfer.value->Abort(error);
    return result->error = error;
}
}  // namespace wqn::services
