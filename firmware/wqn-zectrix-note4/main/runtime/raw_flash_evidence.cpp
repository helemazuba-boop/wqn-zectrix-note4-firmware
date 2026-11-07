#include "runtime/raw_flash_evidence.h"

#include <algorithm>
#include <cstring>
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "runtime/pack_stream_path_lease.h"
#include "runtime/storage_temp_file_lease.h"

namespace wqn::runtime {
namespace {
class Hash {
public:
    Hash() { mbedtls_sha256_init(&value_); good_ = mbedtls_sha256_starts(&value_, 0) == 0; }
    ~Hash() { mbedtls_sha256_free(&value_); }
    bool Add(const uint8_t* data, size_t size) {
        good_ = good_ && mbedtls_sha256_update(&value_, data, size) == 0;
        return good_;
    }
    bool Finish(std::array<uint8_t, 32>* out) {
        return good_ && mbedtls_sha256_finish(&value_, out->data()) == 0;
    }
private:
    mbedtls_sha256_context value_;
    bool good_ = false;
};
struct Bytes {
    std::array<uint8_t, 512> data{};
    size_t size = 0;
    void U(uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) data[size++] = value >> (8 * i);
    }
    void Byte(uint8_t value) { data[size++] = value; }
    void Raw(const void* value, size_t count) {
        if (count != 0) std::memcpy(data.data() + size, value, count);
        size += count;
    }
};
class Writer {
public:
    explicit Writer(const StorageEvidenceSink& sink, RawFlashEvidenceSummary* summary)
        : sink_(sink), summary_(summary) {}
    esp_err_t Poll() {
        if (sink_.cancelled != nullptr && sink_.cancelled(sink_.cancel_context)) return ESP_ERR_INVALID_STATE;
        int64_t now = esp_timer_get_time();
        if (now >= sink_.deadline_us) return ESP_ERR_TIMEOUT;
        if (now - last_yield_ >= 20000) {
            vTaskDelay(1);
            last_yield_ = esp_timer_get_time();
            if (sink_.cancelled != nullptr && sink_.cancelled(sink_.cancel_context)) return ESP_ERR_INVALID_STATE;
            now = last_yield_;
        }
        return now >= sink_.deadline_us ? ESP_ERR_TIMEOUT : ESP_OK;
    }
    esp_err_t Send(uint8_t kind, const Bytes& body) {
        esp_err_t error = Poll();
        if (error != ESP_OK) return error;
        if (body.size > 512 || summary_->frames == UINT32_MAX) return ESP_ERR_INVALID_SIZE;
        std::array<uint8_t, kStorageEvidenceMaxFrameBytes> frame{};
        std::memcpy(frame.data(), "WQR1", 4); frame[4] = kind;
        frame[5] = body.size & 255; frame[6] = body.size >> 8;
        for (unsigned i = 0; i < 4; ++i) frame[7 + i] = summary_->frames >> (8 * i);
        std::memcpy(frame.data() + 11, body.data.data(), body.size);
        if (kind != 5 && !transcript_.Add(frame.data(), 11 + body.size)) return ESP_FAIL;
        error = sink_.emit(frame.data(), 11 + body.size, sink_.context);
        if (error != ESP_OK) return error;
        ++summary_->frames;
        return Poll();
    }
    bool Digest(std::array<uint8_t, 32>* value) { return transcript_.Finish(value); }
private:
    const StorageEvidenceSink& sink_;
    RawFlashEvidenceSummary* summary_;
    Hash transcript_;
    int64_t last_yield_ = 0;
};
template<size_t N> bool Nonzero(const std::array<uint8_t, N>& value) {
    return std::any_of(value.begin(), value.end(), [](uint8_t c) { return c != 0; });
}
}  // namespace

esp_err_t CaptureRawFlashEvidenceOnOwner(const RawFlashEvidenceRequest& request,
    const StorageEvidenceSink& sink, RawFlashEvidenceSummary* summary)
{
    if (summary == nullptr) return ESP_ERR_INVALID_ARG;
    *summary = {};
    auto fail = [summary](esp_err_t error) { summary->error = error; return error; };
    StorageEvidenceBootContext boot;
    if (ReadStorageEvidenceBootContextOnOwner(&boot) != ESP_OK ||
        HasActiveStorageTempFileLease() || HasActivePackStreamPath()) return fail(ESP_ERR_INVALID_STATE);
    if (!request.sensitive_main_flash_confirmed || sink.emit == nullptr || sink.deadline_us <= 0 ||
        !Nonzero(request.identity.capture_id) || !Nonzero(request.identity.mac) ||
        !Nonzero(request.identity.firmware_elf_sha256)) return fail(ESP_ERR_INVALID_ARG);
    Writer writer(sink, summary);
    esp_err_t error = writer.Poll();
    if (error != ESP_OK) return fail(error);
    error = esp_flash_get_size(nullptr, &summary->configured_bytes);
    if (error == ESP_OK) error = writer.Poll();
    if (error == ESP_OK) error = esp_flash_get_physical_size(nullptr, &summary->physical_bytes);
    if (error == ESP_OK) error = writer.Poll();
    if (error != ESP_OK) return fail(error);
    const uint32_t size = summary->configured_bytes;
    if (size == 0 || size > kRawFlashEvidenceMaxBytes || size != summary->physical_bytes)
        return fail(ESP_ERR_INVALID_SIZE);
    summary->flash_encryption_enabled = esp_flash_encryption_enabled();
    const StorageEvidenceIdentity identity = request.identity;
    Bytes begin;
    begin.Raw(identity.capture_id.data(), 16); begin.Raw(identity.mac.data(), 6);
    begin.Raw(identity.firmware_elf_sha256.data(), 32);
    begin.U(static_cast<uint32_t>(boot.error)); begin.U(boot.observed_generation);
    begin.U(size); begin.U(summary->physical_bytes);
    begin.Byte(summary->flash_encryption_enabled); begin.Byte(1); begin.Byte(1);  // confirmed, raw SDK bytes
    error = writer.Send(1, begin);
    if (error != ESP_OK) return fail(error);
    Hash first, second;
    // The sole storage owner has an internal stack. Keep the SDK destination
    // here, word-aligned, rather than on a PSRAM coordinator's stack.
    alignas(4) std::array<uint8_t, kRawFlashEvidenceChunkBytes> data{};
    while (summary->captured_bytes < size) {
        if ((error = writer.Poll()) != ESP_OK) return fail(error);
        const uint32_t offset = summary->captured_bytes;
        const uint32_t count = std::min<uint32_t>(data.size(), size - offset);
        error = esp_flash_read(nullptr, data.data(), offset, count);
        if (error != ESP_OK) { summary->failed_offset = offset; break; }
        if ((error = writer.Poll()) != ESP_OK) return fail(error);
        if (!first.Add(data.data(), count)) return fail(ESP_FAIL);
        Bytes body; body.U(offset); body.Raw(data.data(), count);
        error = writer.Send(3, body);
        if (error != ESP_OK) return fail(error);
        summary->captured_bytes += count;
    }
    if (!first.Finish(&summary->image_sha256)) return fail(ESP_FAIL);
    while (error == ESP_OK && summary->rechecked_bytes < size) {
        if ((error = writer.Poll()) != ESP_OK) return fail(error);
        const uint32_t offset = summary->rechecked_bytes;
        const uint32_t count = std::min<uint32_t>(data.size(), size - offset);
        error = esp_flash_read(nullptr, data.data(), offset, count);
        if (error != ESP_OK) { summary->failed_offset = offset; break; }
        if ((error = writer.Poll()) != ESP_OK) return fail(error);
        if (!second.Add(data.data(), count)) return fail(ESP_FAIL);
        summary->rechecked_bytes += count;
    }
    if (!second.Finish(&summary->recheck_sha256)) return fail(ESP_FAIL);
    if (error == ESP_OK && summary->image_sha256 != summary->recheck_sha256) error = ESP_ERR_INVALID_STATE;
    uint32_t configured = 0, physical = 0;
    if (error == ESP_OK) error = writer.Poll();
    if (error == ESP_OK) error = esp_flash_get_size(nullptr, &configured);
    if (error == ESP_OK) error = writer.Poll();
    if (error == ESP_OK) error = esp_flash_get_physical_size(nullptr, &physical);
    if (error == ESP_OK && (configured != size || physical != summary->physical_bytes ||
        esp_flash_encryption_enabled() != summary->flash_encryption_enabled)) error = ESP_ERR_INVALID_STATE;
    const esp_err_t poll = writer.Poll();
    if (poll != ESP_OK) return fail(poll);
    summary->source_rechecked = error == ESP_OK && summary->captured_bytes == size && summary->rechecked_bytes == size;
    if (!writer.Digest(&summary->transcript_sha256)) return fail(ESP_FAIL);
    Bytes finish;
    finish.U(summary->captured_bytes); finish.U(static_cast<uint32_t>(error));
    finish.U(summary->failed_offset); finish.U(summary->rechecked_bytes);
    finish.Raw(summary->image_sha256.data(), 32); finish.Raw(summary->recheck_sha256.data(), 32);
    finish.Raw(summary->transcript_sha256.data(), 32); finish.Byte(summary->source_rechecked);
    const esp_err_t delivery = writer.Send(5, finish);
    if (delivery != ESP_OK) return fail(delivery);
    summary->capture_complete = summary->source_rechecked;
    return fail(error);
}
}  // namespace wqn::runtime
