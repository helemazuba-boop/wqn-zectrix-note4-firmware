// Production raw collector + actual boot-mode publication; SDK read ports only.
// No filesystem, NVS, Flash init/write/erase/decrypt symbols are supplied.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "runtime/raw_flash_evidence.h"
#include "esp_flash.h"

std::string scenario;
bool owner = true, temp_lease = false, pack_lease = false, cancelled = false;
int64_t now = 1000000;
constexpr int64_t deadline = 5000000000LL;
uint32_t configured = 8193, physical = 8193;
unsigned reads = 0, queries = 0, size_queries = 0, physical_queries = 0, encryption_queries = 0;
unsigned forbidden = 0, yields = 0;
std::vector<uint8_t> flash;
std::ofstream output;
void Destination(const void*) {}
namespace wqn::services { bool IsStorageServiceTask() { return owner; } }
namespace wqn::runtime {
bool HasActiveStorageTempFileLease() { return temp_lease; }
bool HasActivePackStreamPath() { return pack_lease; }
}
int64_t esp_timer_get_time() { now += 1000; return now; }
void vTaskDelay(unsigned) { ++yields; }
esp_err_t esp_flash_get_size(esp_flash_t* chip, uint32_t* size)
{
    ++queries; ++size_queries; forbidden += chip != nullptr;
    if (scenario == "size-error" || (scenario == "size-error-tail" && size_queries > 1)) return ESP_FAIL;
    *size = configured + (scenario == "size-drift" && size_queries > 1 ? 1 : 0);
    return ESP_OK;
}
esp_err_t esp_flash_get_physical_size(esp_flash_t* chip, uint32_t* size)
{
    ++queries; ++physical_queries; forbidden += chip != nullptr;
    if (scenario == "physical-error" || (scenario == "physical-error-tail" && physical_queries > 1)) return ESP_FAIL;
    *size = physical + (scenario == "physical-drift" && physical_queries > 1 ? 1 : 0);
    return ESP_OK;
}
bool esp_flash_encryption_enabled()
{
    ++encryption_queries;
    return scenario == "encrypted" || (scenario == "encryption-drift" && encryption_queries > 1);
}
esp_err_t esp_flash_read(esp_flash_t* chip, void* buffer, uint32_t address, uint32_t length)
{
    const uint32_t chunks = (configured + 383) / 384;
    const unsigned pass = chunks == 0 ? 0 : reads / chunks;
    ++reads;
    if (chip != nullptr || length == 0 || length > 384 ||
        (reinterpret_cast<uintptr_t>(buffer) & 3) != 0 || address > flash.size() || length > flash.size()-address)
        { ++forbidden; return ESP_FAIL; }
    if ((scenario == "read-error" && address == 1152) ||
        (scenario == "recheck-error" && pass == 1 && address == 384)) return ESP_FAIL;
    if (scenario == "drift-data" && pass == 1 && address == 0) flash[0] ^= 1;
    std::memcpy(buffer, flash.data()+address, length);
    if (scenario == "slow-read") now = deadline + 1;
    if (scenario == "cancel-in-read") cancelled = true;
    return ESP_OK;
}
bool Cancel(void*) { return cancelled; }
esp_err_t Emit(const uint8_t* data, size_t size, void*)
{
    if (scenario == "sink-error" && data[4] == 3) return ESP_FAIL;
    output.write(reinterpret_cast<const char*>(data), size);
    if (scenario == "cancel-mid" && data[4] == 3) cancelled = true;
    if (scenario == "deadline-mid" && data[4] == 3) now = deadline + 1;
    if (scenario == "late-sink-error" && data[4] == 5) return ESP_FAIL;
    if (scenario == "late-sink-deadline" && data[4] == 5) now = deadline + 1;
    return output.good() ? ESP_OK : ESP_FAIL;
}
int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    scenario = argv[1]; output.open(argv[2], std::ios::binary);
    if (scenario == "board-16m") configured = physical = 16U*1024U*1024U;
    if (scenario == "one-byte") configured = physical = 1;
    const uint32_t source_size = configured;
    flash.resize(source_size);
    for (size_t i = 0; i < flash.size(); ++i) flash[i] = static_cast<uint8_t>(17*i+29);
    const char marker[] = "SYNTHETIC_MAIN_FLASH_CREDENTIAL_REGION";
    if (flash.size() >= 4096+sizeof(marker)-1) std::memcpy(flash.data()+4096, marker, sizeof(marker)-1);
    if (scenario == "size-mismatch") physical += 4096;
    if (scenario == "zero-size") configured = physical = 0;
    if (scenario == "too-big") configured = physical = wqn::runtime::kRawFlashEvidenceMaxBytes+1;
    if (scenario == "off-owner") owner = false;
    if (scenario == "temp-lease") temp_lease = true;
    if (scenario == "pack-lease") pack_lease = true;
    if (scenario == "cancel-early") cancelled = true;
    if (scenario != "not-mode") wqn::runtime::EnterStorageEvidenceBootRecovery(ESP_ERR_INVALID_CRC, 3);
    if (scenario == "republish") {
        wqn::runtime::StorageEvidenceBootContext boot;
        if (wqn::runtime::EnterStorageEvidenceBootRecovery(ESP_FAIL, 99) != ESP_ERR_INVALID_STATE ||
            wqn::runtime::ReadStorageEvidenceBootContextOnOwner(&boot) != ESP_OK ||
            boot.error != ESP_ERR_INVALID_CRC || boot.observed_generation != 3) ++forbidden;
    }
    wqn::runtime::RawFlashEvidenceRequest request;
    request.identity.capture_id.fill(0x11); request.identity.mac.fill(0x22);
    request.identity.firmware_elf_sha256.fill(0x33); request.sensitive_main_flash_confirmed = true;
    if (scenario == "no-consent") request.sensitive_main_flash_confirmed = false;
    if (scenario == "empty-capture") request.identity.capture_id.fill(0);
    if (scenario == "empty-mac") request.identity.mac.fill(0);
    if (scenario == "empty-firmware") request.identity.firmware_elf_sha256.fill(0);
    wqn::runtime::StorageEvidenceSink sink{Emit, nullptr, Cancel, nullptr, deadline};
    if (scenario == "no-sink") sink.emit = nullptr;
    if (scenario == "invalid-deadline") sink.deadline_us = 0;
    if (scenario == "expired") sink.deadline_us = 1;
    wqn::runtime::RawFlashEvidenceSummary summary;
    const int error = wqn::runtime::CaptureRawFlashEvidenceOnOwner(request, sink, &summary);
    output.close();
    const bool success = scenario == "normal" || scenario == "encrypted" || scenario == "board-16m" ||
        scenario == "one-byte" || scenario == "republish";
    const bool no_io = scenario == "not-mode" || scenario == "off-owner" || scenario == "temp-lease" ||
        scenario == "pack-lease" || scenario == "no-consent" || scenario == "empty-capture" ||
        scenario == "empty-mac" || scenario == "empty-firmware" || scenario == "no-sink" ||
        scenario == "invalid-deadline" || scenario == "expired" || scenario == "cancel-early";
    const bool no_reads = no_io || scenario == "size-error" || scenario == "physical-error" ||
        scenario == "size-mismatch" || scenario == "zero-size" || scenario == "too-big";
    const bool good = (success ? (error == ESP_OK && summary.capture_complete && summary.source_rechecked &&
        summary.captured_bytes == source_size && summary.rechecked_bytes == source_size) :
        (error != ESP_OK && !summary.capture_complete)) && summary.error == error && forbidden == 0 &&
        (!no_io || (queries == 0 && summary.frames == 0)) && (!no_reads || reads == 0);
    std::printf("{\"passed\":%s,\"error\":%d,\"frames\":%u,\"captured\":%u,\"rechecked\":%u,"
                "\"reads\":%u,\"queries\":%u,\"forbidden\":%u,\"complete\":%s,\"yields\":%u}\n",
        good ? "true" : "false", error, summary.frames, summary.captured_bytes, summary.rechecked_bytes,
        reads, queries, forbidden, summary.capture_complete ? "true" : "false", yields);
    return good ? 0 : 1;
}
