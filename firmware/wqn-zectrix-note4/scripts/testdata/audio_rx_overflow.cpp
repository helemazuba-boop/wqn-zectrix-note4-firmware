#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#define DRAM_ATTR
#define IRAM_ATTR
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NO_MEM = 1, ESP_ERR_INVALID_ARG = 2, ESP_ERR_INVALID_STATE = 3;
constexpr uint32_t kI2sDmaDescNum = 6;
constexpr size_t kDuplexBytesPerFrame = 4, kDuplexAdmissionInternalHeadroom = 4096;
constexpr size_t kMaxChannelHandles = 2;
constexpr int kI2sDataIn = 16, kI2sDataOut = 45;
constexpr char kTag[] = "host";
struct lldesc_t { uint32_t words[3]; };
template <typename... T> void Log(T...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
const char* esp_err_to_name(int) { return "host"; }
struct AudioSession { uint32_t id = 1; };
struct DriverOperation {
    bool valid;
    explicit DriverOperation(const AudioSession& s) : valid(s.id == 1) {}
    explicit operator bool() const { return valid; }
};
struct Channel { bool alive = false, enabled = false; } receiver, transmitter;
using i2s_chan_handle_t = Channel*;
using AudioChannelHandle = void*;
struct i2s_event_data_t {};
using Callback = bool (*)(i2s_chan_handle_t, i2s_event_data_t*, void*);
struct i2s_event_callbacks_t {
    Callback on_recv = nullptr, on_recv_q_ovf = nullptr, on_sent = nullptr, on_send_q_ovf = nullptr;
};
int callback_result = ESP_OK, resource_result = ESP_OK;
int enable_calls = 0, delete_calls = 0, registered = 0, callback_registrations = 0;
i2s_event_callbacks_t installed;
int i2s_channel_register_event_callback(Channel* channel, const i2s_event_callbacks_t* callbacks, void* context) {
    assert(channel == &receiver && channel->alive && !channel->enabled && context == nullptr);
    assert(callbacks->on_recv == nullptr && callbacks->on_sent == nullptr && callbacks->on_send_q_ovf == nullptr);
    ++callback_registrations;
    if (callback_result == ESP_OK) installed = *callbacks;
    return callback_result;
}
@@COUNTER@@
struct AudioHeapSnapshot { size_t dma_free = 32768, internal_free = 32768, dma_largest = 8192; };
AudioHeapSnapshot CaptureAudioHeapSnapshot() { return {}; }
void LogDuplexHeapPoint(const char*, const AudioSession&, uint32_t, Channel*, Channel*, const AudioHeapSnapshot* = nullptr) {}
struct i2s_chan_config_t {};
struct i2s_std_config_t { struct { int din = 0, dout = 0; } gpio_cfg; };
i2s_chan_config_t MakeChannelConfig(uint32_t) { return {}; }
i2s_std_config_t MakeStandardConfig(uint32_t, bool, bool) { return {}; }
int i2s_new_channel(const i2s_chan_config_t*, Channel** tx, Channel** rx) {
    assert(!receiver.alive && !transmitter.alive);
    receiver.alive = transmitter.alive = true; *tx = &transmitter; *rx = &receiver;
    return ESP_OK;
}
int i2s_channel_init_std_mode(Channel* channel, const i2s_std_config_t*) { assert(channel->alive); return ESP_OK; }
int i2s_channel_enable(Channel* channel) {
    assert(channel->alive && installed.on_recv_q_ovf != nullptr);
    channel->enabled = true; ++enable_calls; return ESP_OK;
}
int i2s_channel_disable(Channel* channel) { assert(channel->alive); channel->enabled = false; return ESP_OK; }
int i2s_del_channel(Channel* channel) { assert(channel->alive && !channel->enabled); channel->alive = false; ++delete_calls; return ESP_OK; }
int g_channel_slots[2];
int RegisterResource(int*, size_t, const AudioSession&, Channel*) { if (resource_result == ESP_OK) ++registered; return resource_result; }
void DiscardRegisteredResource(int*, size_t, uint32_t, Channel*) { registered = 0; }
@@CREATE@@
@@READ@@
void Reset() {
    receiver = {}; transmitter = {}; installed = {};
    callback_result = resource_result = ESP_OK;
    enable_calls = delete_calls = registered = callback_registrations = 0;
}
int main() {
    AudioChannelHandle rx = nullptr, tx = nullptr;
    Reset();
    assert(CreateAudioDuplexChannels({}, 24000, 128, true, &rx, &tx) == ESP_OK);
    assert(rx == &receiver && tx == &transmitter && callback_registrations == 1);
    assert(enable_calls == 2 && registered == 2);
    const auto baseline = AudioDuplexRxOverflowCount();
    assert(!installed.on_recv_q_ovf(&receiver, nullptr, nullptr));
    assert(AudioDuplexRxOverflowCount() - baseline == 1);
    std::thread a([] { for (int i = 0; i < 20000; ++i) CountDuplexRxOverflow(nullptr, nullptr, nullptr); });
    std::thread b([] { for (int i = 0; i < 20000; ++i) CountDuplexRxOverflow(nullptr, nullptr, nullptr); });
    a.join(); b.join();
    assert(AudioDuplexRxOverflowCount() - baseline == 40001);
    g_duplex_rx_overflows = UINT32_MAX;
    CountDuplexRxOverflow(nullptr, nullptr, nullptr);
    assert(AudioDuplexRxOverflowCount() == 0); // unsigned per-turn subtraction wraps safely
    Reset(); rx = tx = nullptr; callback_result = ESP_ERR_INVALID_STATE;
    assert(CreateAudioDuplexChannels({}, 24000, 128, true, &rx, &tx) == ESP_ERR_INVALID_STATE);
    assert(rx == nullptr && tx == nullptr && !receiver.alive && !transmitter.alive);
    assert(callback_registrations == 1 && enable_calls == 0 && delete_calls == 2 && registered == 0);
    Reset(); rx = tx = nullptr; resource_result = ESP_ERR_NO_MEM;
    assert(CreateAudioDuplexChannels({}, 24000, 128, true, &rx, &tx) == ESP_ERR_NO_MEM);
    assert(rx == nullptr && tx == nullptr && !receiver.alive && !transmitter.alive && delete_calls == 2);
    std::puts("Production duplex RX overflow callback: registration, failure rollback, 40001 events and wrap passed; not ISR/HIL proof");
}
