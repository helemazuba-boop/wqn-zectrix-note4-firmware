#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "services/audio_service.h"

constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = 261;
constexpr int ESP_ERR_INVALID_STATE = 259;
unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "FAIL %d at %d Hz: %s\n", __LINE__, \
                 FIXTURE_TICK_RATE_HZ, #expr); std::exit(1); } } while (false)

using namespace wqn::services;
struct Channel {};
using i2s_chan_handle_t = Channel*;
Channel channel;
bool accepting = true;
unsigned active_operations = 0, driver_calls = 0;
uint32_t driver_timeout_ms = 0;
size_t driver_bytes = 0, completed_bytes = 4;
int driver_result = ESP_OK;

// Capability and scheduling ports are intentionally separate from the API
// unit boundary exercised below; AudioService owns the actual guard policy.
struct DriverOperation {
    enum class Kind { kChannel };
    bool valid;
    DriverOperation(const AudioSession& session, Kind, void* handle)
        : valid(accepting && session.id == 1 && handle == &channel) {
        if (valid) ++active_operations;
    }
    ~DriverOperation() { if (valid) --active_operations; }
    explicit operator bool() const { return valid; }
};
i2s_chan_handle_t AsChannel(AudioChannelHandle handle) {
    return static_cast<i2s_chan_handle_t>(handle);
}
int DriverCall(i2s_chan_handle_t handle, size_t bytes, size_t* completed,
               uint32_t timeout_ms) {
    CHECK(handle == &channel);
    CHECK(active_operations == 1);
    ++driver_calls;
    driver_timeout_ms = timeout_ms;
    driver_bytes = bytes;
    *completed = completed_bytes;
    return driver_result;
}
int i2s_channel_read(i2s_chan_handle_t handle, void*, size_t bytes,
                     size_t* read, uint32_t timeout_ms) {
    return DriverCall(handle, bytes, read, timeout_ms);
}
int i2s_channel_write(i2s_chan_handle_t handle, const void*, size_t bytes,
                      size_t* written, uint32_t timeout_ms) {
    return DriverCall(handle, bytes, written, timeout_ms);
}

namespace wqn::services {
@@READ@@
@@WRITE@@
}  // namespace wqn::services

AudioSession session{1, AudioActivity::kCapture};
uint8_t buffer[16] = {};
void ProbeRead(uint32_t timeout_ms, uint32_t expected_ms) {
    size_t read = 0;
    const unsigned calls_before = driver_calls;
    CHECK(ReadAudioChannel(session, &channel, buffer, sizeof(buffer),
                           &read, timeout_ms) == driver_result);
    CHECK(driver_calls == calls_before + 1);
    CHECK(driver_timeout_ms == expected_ms);
    CHECK(driver_bytes == sizeof(buffer));
    CHECK(read == completed_bytes);
    CHECK(active_operations == 0);
}
void ProbeWrite(uint32_t timeout_ms, uint32_t expected_ms) {
    size_t written = 0;
    const unsigned calls_before = driver_calls;
    CHECK(WriteAudioChannel(session, &channel, buffer, sizeof(buffer),
                            &written, timeout_ms) == driver_result);
    CHECK(driver_calls == calls_before + 1);
    CHECK(driver_timeout_ms == expected_ms);
    CHECK(driver_bytes == sizeof(buffer));
    CHECK(written == completed_bytes);
    CHECK(active_operations == 0);
}

int main() {
    // These are final-argument expressions extracted from every actual caller.
    // At 100 Hz the old pdMS_TO_TICKS conversion fails the expected-ms check.
    @@CALLERS@@
    for (uint32_t timeout : {0U, 1U, 2U, 100U, 200U, 250U, 1000U, UINT32_MAX}) {
        for (int error : {ESP_OK, ESP_ERR_TIMEOUT, ESP_FAIL}) {
            for (size_t bytes : {size_t{0}, size_t{3}, sizeof(buffer)}) {
                driver_result = error;
                completed_bytes = bytes;
                ProbeRead(timeout, timeout);
                ProbeWrite(timeout, timeout);
            }
        }
    }
    size_t completed = 0;
    const unsigned calls_before = driver_calls;
    for (bool read : {false, true}) {
        const auto call = [&](AudioSession capability, void* handle,
                              void* data, size_t* result) {
            return read
                ? ReadAudioChannel(capability, handle, data, sizeof(buffer), result, 200)
                : WriteAudioChannel(capability, handle, data, sizeof(buffer), result, 200);
        };
        CHECK(call({}, &channel, buffer, &completed) == ESP_ERR_INVALID_STATE);
        CHECK(call({2, AudioActivity::kCapture}, &channel, buffer, &completed)
              == ESP_ERR_INVALID_STATE);
        CHECK(call(session, nullptr, buffer, &completed) == ESP_ERR_INVALID_STATE);
        CHECK(call(session, &channel, nullptr, &completed) == ESP_ERR_INVALID_STATE);
        CHECK(call(session, &channel, buffer, nullptr) == ESP_ERR_INVALID_STATE);
        accepting = false;
        CHECK(call(session, &channel, buffer, &completed) == ESP_ERR_INVALID_STATE);
        accepting = true;
    }
    CHECK(driver_calls == calls_before);
    CHECK(active_operations == 0);
    std::printf("PASS: audio channel timeout %u checks at %d Hz (host, not HIL)\n",
                checks, FIXTURE_TICK_RATE_HZ);
}
