#include "runtime/evidence_transfer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

namespace wqn::runtime {
namespace {
void Hex(const uint8_t* data, size_t size, char* output) {
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) { output[2*i] = digits[data[i] >> 4]; output[2*i+1] = digits[data[i] & 15]; }
    output[2*size] = 0;
}
bool Unhex(const char* text, uint8_t* output, size_t count) {
    if (std::strlen(text) != count*2) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1; };
    bool nonzero = false;
    for (size_t i = 0; i < count; ++i) {
        const int high = digit(text[2*i]), low = digit(text[2*i+1]);
        if (high < 0 || low < 0) return false;
        output[i] = high*16+low; nonzero = nonzero || output[i] != 0;
    }
    return nonzero;
}
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24); }
}
esp_err_t EvidenceTransfer::Check(int64_t until) const {
    if (cancelled_ != nullptr && cancelled_(cancel_context_)) return ESP_ERR_INVALID_STATE;
    return esp_timer_get_time() >= std::min(until, deadline_) ? ESP_ERR_TIMEOUT : ESP_OK;
}
void EvidenceTransfer::Yield(bool stalled) {
    const int64_t now = esp_timer_get_time();
    if (stalled || now-last_yield_ >= 20000) { vTaskDelay(1); last_yield_ = esp_timer_get_time(); }
}
esp_err_t EvidenceTransfer::Write(const char* text, int64_t until) {
    const size_t size = std::strlen(text);
    size_t offset = 0;
    while (offset < size) {
        esp_err_t error = Check(until); if (error != ESP_OK) return error;
        const int n = io_.write(reinterpret_cast<const uint8_t*>(text)+offset, size-offset, 50, io_.context);
        if (n < 0 || static_cast<size_t>(n) > size-offset) return ESP_FAIL;
        offset += n; Yield(n == 0);
    }
    return Check(until);
}
esp_err_t EvidenceTransfer::Line(int64_t until) {
    size_t count = 0;
    bool overflow = false;
    while (true) {
        esp_err_t error = Check(until); if (error != ESP_OK) return error;
        if (input_at_ == input_size_) {
            const int n = io_.read(input_, sizeof(input_), 50, io_.context);
            if (n < 0 || static_cast<size_t>(n) > sizeof(input_)) return ESP_FAIL;
            input_at_ = 0; input_size_ = n; Yield(n == 0);
            if (n == 0) continue;
        }
        const char c = input_[input_at_++];
        if (c == '\n') {
            if (overflow) { count = 0; overflow = false; continue; }
            if (count != 0 && line_[count-1] == '\r') --count;
            line_[count] = 0;
            char quit[48]; std::snprintf(quit, sizeof(quit), "WQNT1 Q %s", capture_);
            return std::strcmp(line_, quit) == 0 ? ESP_ERR_INVALID_STATE : ESP_OK;
        }
        if (c == '\0') overflow = true;  // Reject the whole malformed line.
        else if (count+1 >= sizeof(line_)) overflow = true;
        else if (!overflow) line_[count++] = c;
    }
}
esp_err_t EvidenceTransfer::Begin(const EvidenceTransferIo& io, const StorageEvidenceIdentity& identity,
    char kind, bool (*cancelled)(void*), void* context, int64_t deadline)
{
    result_ = {}; count_ = next_ = pending_data_bytes_ = 0; input_at_ = input_size_ = 0;
    pending_source_complete_ = false;
    io_ = io; cancelled_ = cancelled; cancel_context_ = context; deadline_ = deadline; kind_ = kind; last_yield_ = esp_timer_get_time();
    if (io.open == nullptr || io.read == nullptr || io.write == nullptr || (kind != 'R' && kind != 'L'))
        return result_.error = ESP_ERR_INVALID_ARG;
    esp_err_t error = Check(deadline); if (error != ESP_OK) return result_.error = error;
    Hex(identity.capture_id.data(), 16, capture_); Hex(identity.mac.data(), 6, mac_);
    Hex(identity.firmware_elf_sha256.data(), 32, firmware_);
    error = io_.open(io_.context); if (error != ESP_OK) return result_.error = error;
    error = Write("\n", std::min(deadline_, esp_timer_get_time()+2000000));
    if (error != ESP_OK) return result_.error = error;
    const int64_t ready_until = std::min(deadline_, esp_timer_get_time()+60000000);
    while ((error = Check(ready_until)) == ESP_OK) {
        char offer[192]; std::snprintf(offer, sizeof(offer), "WQNT1 O %s %s %s %c\n", capture_, mac_, firmware_, kind_);
        error = Write(offer, std::min(ready_until, esp_timer_get_time()+2000000));
        if (error != ESP_OK) break;
        const int64_t poll_until = std::min(ready_until, esp_timer_get_time()+1000000);
        while ((error = Line(poll_until)) == ESP_OK) {
            char expected[80]; std::snprintf(expected, sizeof(expected), "WQNT1 R %s", capture_);
            if (std::strcmp(line_, expected) == 0) return result_.error = ESP_OK;
        }
        if (error != ESP_ERR_TIMEOUT) break;
    }
    return result_.error = error;
}
esp_err_t EvidenceTransfer::Emit(const uint8_t* data, size_t size)
{
    if (result_.error != ESP_OK) return result_.error;
    if (result_.commit_acknowledged) return result_.error = ESP_ERR_INVALID_STATE;
    if (data == nullptr || size < 11 || size > kStorageEvidenceMaxFrameBytes ||
        std::memcmp(data, kind_ == 'R' ? "WQR1" : "WQE1", 4) != 0 || U32(data+7) != next_ ||
        size != size_t(data[5] | (unsigned(data[6])<<8))+11 || next_ == UINT32_MAX)
        return result_.error = ESP_ERR_INVALID_RESPONSE;
    std::memcpy(window_[count_].data(), data, size); sizes_[count_++] = size; ++next_;
    if (data[4] == 5) pending_source_complete_ = data[size-1] == 1;
    if (data[4] == 3) {
        const size_t prefix = kind_ == 'R' ? 4 : 10;
        if (size < 11+prefix) return result_.error = ESP_ERR_INVALID_RESPONSE;
        pending_data_bytes_ += size-11-prefix;
    }
    if (count_ == 16 || data[4] == 5) return result_.error = Flush(data[4] == 5);
    return result_.error = Check(deadline_);
}
esp_err_t EvidenceTransfer::Flush(bool final)
{
    mbedtls_sha256_context group;
    mbedtls_sha256_init(&group);
    int hash_error = mbedtls_sha256_starts(&group, 0);
    for (uint32_t i = 0; i < count_ && hash_error == 0; ++i)
        hash_error = mbedtls_sha256_update(&group, window_[i].data(), sizes_[i]);
    uint8_t digest[32]{};
    if (hash_error == 0) hash_error = mbedtls_sha256_finish(&group, digest);
    mbedtls_sha256_free(&group);
    if (hash_error != 0) return ESP_FAIL;
    char group_hex[65]; Hex(digest, 32, group_hex);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        for (uint32_t i = 0; i < count_; ++i) {
            uint8_t frame_hash[32];
            if (mbedtls_sha256(window_[i].data(), sizes_[i], frame_hash, 0) != 0) return ESP_FAIL;
            char frame_hex[65]; Hex(frame_hash, 32, frame_hex);
            char encoded[704]; size_t encoded_size = 0;
            if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(encoded), sizeof(encoded)-1, &encoded_size,
                                      window_[i].data(), sizes_[i]) != 0) return ESP_FAIL;
            encoded[encoded_size] = 0;
            const uint32_t sequence = next_-count_+i;
            std::snprintf(line_, sizeof(line_), "WQNT1 D %s %08lx %04x %s %s\n", capture_,
                static_cast<unsigned long>(sequence), static_cast<unsigned>(sizes_[i]), frame_hex, encoded);
            const esp_err_t error = Write(line_, std::min(deadline_, esp_timer_get_time()+2000000));
            if (error != ESP_OK) return error;
        }
        const int64_t until = std::min(deadline_, esp_timer_get_time()+(final ? 30000000 : 2000000));
        esp_err_t error;
        while ((error = Line(until)) == ESP_OK) {
            char expected[192]; std::snprintf(expected, sizeof(expected), "WQNT1 %c %s %08lx %s",
                final ? 'C' : 'A', capture_, static_cast<unsigned long>(next_-1), group_hex);
            const size_t prefix = std::strlen(expected);
            if (!final && std::strcmp(line_, expected) == 0) {
                result_.persisted_frames = next_; result_.persisted_data_bytes += pending_data_bytes_;
                count_ = pending_data_bytes_ = 0; return ESP_OK;
            }
            if (final && std::strncmp(line_, expected, prefix) == 0 && line_[prefix] == ' ' &&
                Unhex(line_+prefix+1, result_.manifest_sha256.data(), 32)) {
                result_.persisted_frames = next_; result_.persisted_data_bytes += pending_data_bytes_;
                count_ = pending_data_bytes_ = 0; result_.commit_acknowledged = true;
                result_.source_complete_reported = pending_source_complete_;
                char finish[192]; std::snprintf(finish, sizeof(finish), "WQNT1 F %s %s\n", capture_, line_+prefix+1);
                return Write(finish, std::min(deadline_, esp_timer_get_time()+2000000));
            }
        }
        if (error != ESP_ERR_TIMEOUT) return error;
    }
    return ESP_ERR_TIMEOUT;
}
void EvidenceTransfer::Abort(esp_err_t error)
{
    if (result_.commit_acknowledged) return;
    char line[96]; std::snprintf(line, sizeof(line), "WQNT1 E %s %08lx\n", capture_,
        static_cast<unsigned long>(static_cast<uint32_t>(error)));
    (void)Write(line, std::min(deadline_, esp_timer_get_time()+500000));
}
}  // namespace wqn::runtime
