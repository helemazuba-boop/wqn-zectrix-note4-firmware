#include "flash_uplink_queue.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using FlashUplink = wqn::FlashUplinkQueue<192, 360>;
constexpr int kSampleRate = 24000, kStreamChunkFrames = 360, kStreamChunkBytes = 720;
constexpr int kAudioFrameMagic = 0x57464c56, kFrameVersion = 2, kFlagStream = 1, kFlagFinal = 2;
constexpr int64_t kUplinkDrainBudgetUs = 3000000;
constexpr char kTag[] = "host";
constexpr int portMAX_DELAY = -1, pdTRUE = 1, ESP_OK = 0, ESP_ERR_TIMEOUT = 1;
constexpr int MALLOC_CAP_DMA = 1, MALLOC_CAP_INTERNAL = 2, MALLOC_CAP_SPIRAM = 4;
#define pdMS_TO_TICKS(x) (x)
template <typename... Args> void HostLog(const char*, Args...) {}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ((void)(x))
#define ESP_RETURN_ON_ERROR(x, ...) do { const int code = (x); if (code != ESP_OK) return code; } while (0)
using esp_err_t = int;
namespace wqn::services { uint32_t AudioDuplexRxOverflowCount() { return 0; } }
using TaskHandle_t = void*;
struct Client { bool alive = true; } client;
using esp_websocket_client_handle_t = Client*;
enum class InternalStatus { kIdle, kConnecting, kStreaming, kError };
enum class FlashTerminalReason { kNone, kTransportError, kUplinkOverflow, kFinalUnconfirmed };
struct {
    void* mutex = reinterpret_cast<void*>(1);
    InternalStatus status = InternalStatus::kStreaming;
    bool capture_started = false, ws_connected = true, button_pressed = true;
    bool response_in_flight = false, response_started = false;
    bool playback_abort_requested = false, amp_idle_armed = false;
    bool stream_audio_powered = false;
    void* stream_i2c_bus = nullptr;
    void* stream_task = nullptr;
    void* stream_rx = reinterpret_cast<void*>(1);
    void* stream_tx = reinterpret_cast<void*>(2);
    int64_t status_since_ms = 0;
    std::string pending_text;
    std::string tool_label, error_message, audio_reassembly_buf;
    Client* ws_client = &client;
    Client* ws_client_to_destroy = nullptr;
    std::atomic<uint32_t> uplink_seq{0}, active_ws_sends{0};
    uint32_t capture_base_seq = 0;
} g_flash;
FlashUplink g_uplink;
FlashUplink::Block storage[FlashUplink::kStorageSlots]{};
TaskHandle_t g_uplink_task = reinterpret_cast<void*>(3);
std::atomic<uint32_t> g_session_generation{1};
std::atomic<bool> g_teardown_pending{false}, g_intentional_stop{false};
int g_flash_audio_session = 1;
int lock_depth = 0, network_calls = 0, final_calls = 0, stop_requests = 0;
int destroyed_clients = 0, ended_audio_sessions = 0, teardown_calls = 0;
int64_t clock_us = 0, capture_stop_cost = 0;
int64_t first_network_stall_us = 0;
bool short_pcm = false, short_final = false, disconnect_during_send = false;
bool drain_on_stop = true, capture_indicator = false;
bool producer_ack = true;
bool partial_first_read = false, malformed_first_read = false, fatal_first_read = false;
int pcm_blocks = 0, read_calls = 0;
std::vector<int> packet_first_samples;
std::vector<uint32_t> wire_sequences;
std::vector<size_t> packet_sizes;
std::vector<std::string> text_messages;
FlashTerminalReason terminal_reason = FlashTerminalReason::kNone;
std::function<void()> wait_hook;
void xSemaphoreTake(void*, int) { assert(lock_depth == 0); ++lock_depth; }
void xSemaphoreGive(void*) { assert(lock_depth == 1); --lock_depth; }
void xTaskNotifyGive(void*) {}
int64_t esp_timer_get_time() { return clock_us; }
void vTaskDelay(int ms) { clock_us += ms * 1000; if (wait_hook) wait_hook(); }
unsigned uxTaskGetStackHighWaterMark(void*) { return 2048; }
unsigned heap_caps_get_free_size(int) { return 8192; }
unsigned heap_caps_get_largest_free_block(int) { return 4096; }
const char* esp_err_to_name(int) { return "fixture"; }
void MarkChanged() {}
void SetFlashCaptureIndicatorLocked(bool ready) { capture_indicator = ready && g_flash.capture_started; }
void SetStreamAudioAmp(bool enabled) { assert(!enabled); }
void SetErrorLocked(const std::string&) { g_flash.status = InternalStatus::kError; }
void RequestFlashTerminalStop(FlashTerminalReason reason) {
    ++stop_requests; terminal_reason = reason; g_teardown_pending = true;
}
int StopFlashSession() { g_uplink.Cancel(g_uplink.Generation()); return ESP_OK; }
struct { void Reset() {} } g_flash_connectivity_demand;
void DrainPlaybackRingbuf() {}
int TearDownStreamChannels() {
    assert(g_flash.stream_task == nullptr && g_uplink.ProducerFinished() && g_uplink.SenderFinished());
    ++teardown_calls; g_flash.stream_rx = nullptr; g_flash.stream_tx = nullptr;
    return ESP_OK;
}
int esp_websocket_client_stop(Client* target) {
    assert(target == &client && client.alive && lock_depth == 0);
    return ESP_OK;
}
int esp_websocket_client_destroy(Client* target) {
    assert(target == &client && client.alive && g_flash.active_ws_sends == 0 && lock_depth == 0);
    assert(g_uplink.ProducerFinished() && g_uplink.SenderFinished());
    client.alive = false; ++destroyed_clients; return ESP_OK;
}
struct WorkerWait {};
void vTaskDelete(void*) { throw WorkerWait{}; } // baseline only; current tasks persist
int notifications = 0;
unsigned ulTaskNotifyTake(int, int timeout) {
    assert(timeout == portMAX_DELAY); // no per-chunk polling while idle
    if (notifications++ == 0) return 1;
    throw WorkerWait{};
}
void RunSender();
namespace wqn::services {
int EndAudioActivity(int* session) {
    assert(g_uplink.ProducerFinished() && g_uplink.SenderFinished());
    if (*session) ++ended_audio_sessions;
    *session = 0; return ESP_OK;
}
esp_err_t ReadAudioChannel(int, void*, void* output, size_t size, size_t* bytes, int) {
    assert(size == 1440);
    const int index = std::max(0, read_calls - 4);
    auto* samples = static_cast<int16_t*>(output);
    for (size_t i = 0; i < size / 4; ++i) {
        samples[i * 2] = static_cast<int16_t>(index + 100);
        samples[i * 2 + 1] = 0;
    }
    *bytes = size;
    ++read_calls;
    clock_us += 15000;
    if (read_calls >= pcm_blocks + 4) g_flash.capture_started = false;
    if (read_calls == 5) {
        if (partial_first_read) { *bytes = size / 2; return ESP_ERR_TIMEOUT; }
        if (malformed_first_read) *bytes = size - 1;
        if (fatal_first_read) return 2;
    }
    return ESP_OK;
}
}
namespace services = wqn::services;
@@REGISTRATION@@

uint32_t Read32(const uint8_t* data) {
    return data[0] | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}
int BeforeNetwork(Client* target) {
    assert(target == &client && client.alive && lock_depth == 0);
    assert(g_flash.active_ws_sends.load() == 1);
    clock_us += first_network_stall_us;
    first_network_stall_us = 0;
    if (disconnect_during_send) {
        g_flash.ws_client = nullptr;
        g_flash.ws_connected = false;
        g_teardown_pending = true;
        // Lifecycle must defer deletion until the send registration is released.
        assert(g_flash.active_ws_sends.load() != 0);
    }
    return ++network_calls;
}
int esp_websocket_client_send_bin(Client* target, const char* data, size_t length, int timeout) {
    BeforeNetwork(target);
    const auto* frame = reinterpret_cast<const uint8_t*>(data);
    assert(Read32(frame) == uint32_t(kAudioFrameMagic));
    assert(Read32(frame + 12) == 24000 && Read32(frame + 16) == 1);
    wire_sequences.push_back(Read32(frame + 8));
    packet_sizes.push_back(length);
    if (length == 24) {
        assert(frame[6] == 3 && timeout == 1000);
        ++final_calls;
        return short_final ? int(length) - 1 : int(length);
    }
    assert(frame[6] == 1 && length > 24 && timeout == 2500);
    int16_t first; std::memcpy(&first, frame + 24, sizeof(first));
    packet_first_samples.push_back(first);
    return short_pcm ? int(length) - 1 : int(length);
}
int esp_websocket_client_send_text(Client* target, const char* data, size_t length, int timeout) {
    BeforeNetwork(target);
    text_messages.emplace_back(data, length);
    const bool append = text_messages.back().find("input_audio_buffer.append") != std::string::npos;
    assert(timeout == (append ? 2500 : 1000));
    if (!append) ++final_calls;
    return (append ? short_pcm : short_final) ? int(length) - 1 : int(length);
}
@@HEADER@@
@@ENCODE@@
@@CAPTURE@@
@@SENDER@@
@@WAIT@@
void StopAudioStreaming() {
    g_flash.capture_started = false;
    SetFlashCaptureIndicatorLocked(false);
    clock_us += capture_stop_cost;
    if (producer_ack) {
        g_uplink.ProducerDone(g_uplink.Generation());
        g_flash.stream_task = nullptr;
    }
    if (drain_on_stop) RunSender();
}
@@RELEASE@@
@@STOP@@

void RunSender() {
    notifications = 0;
    try { FlashUplinkTask(nullptr); } catch (const WorkerWait&) {}
}
uint32_t Reset(unsigned session = 1) {
    g_uplink.~FlashUplink();
    new (&g_uplink) FlashUplink();
    g_uplink.Configure(storage);
    g_flash.capture_started = true; g_flash.button_pressed = true;
    g_flash.status = InternalStatus::kStreaming;
    g_flash.ws_connected = true; g_flash.ws_client = &client;
    g_flash.stream_rx = reinterpret_cast<void*>(1); g_flash.stream_tx = reinterpret_cast<void*>(2);
    g_flash.stream_task = reinterpret_cast<void*>(4);
    g_flash.uplink_seq = 0; g_flash.capture_base_seq = 0;
    g_flash.active_ws_sends = 0; g_flash.response_in_flight = false;
    client.alive = true; g_teardown_pending = false; g_intentional_stop = false;
    g_flash.ws_client_to_destroy = nullptr; g_flash_audio_session = 1;
    g_session_generation = session; lock_depth = 0;
    short_pcm = short_final = disconnect_during_send = false;
    partial_first_read = malformed_first_read = fatal_first_read = false;
    clock_us = capture_stop_cost = 0;
    first_network_stall_us = 0;
    network_calls = final_calls = stop_requests = read_calls = 0;
    pcm_blocks = 0; wait_hook = {}; drain_on_stop = true;
    producer_ack = true; destroyed_clients = ended_audio_sessions = teardown_calls = 0;
    packet_first_samples.clear(); wire_sequences.clear(); packet_sizes.clear(); text_messages.clear();
    terminal_reason = FlashTerminalReason::kNone;
    return g_uplink.Begin(session);
}
void Queue(unsigned count, uint32_t gen) {
    int16_t samples[360];
    for (unsigned i = 0; i < count; ++i) {
        std::fill(std::begin(samples), std::end(samples), static_cast<int16_t>(i));
        assert(g_uplink.Push(gen, samples, 360));
    }
}
void TestCapture() {
    const auto gen = Reset();
    pcm_blocks = 170; // 2.55 seconds with no sender progress (2.5s send budget).
    first_network_stall_us = 2500000;
    notifications = 0;
    try { AudioStreamingTask(nullptr); } catch (const WorkerWait&) {}
    assert(network_calls == 0 && g_uplink.Accepted() == 170);
    assert(clock_us - 4 * 15000 == 2550000);
    std::printf("CAPTURE_SIMULATION current blocks=170 capture_loop_ms=2550 network_calls_during_capture=0 retained_blocks=%u\n", g_uplink.Accepted());
    assert(g_uplink.ProducerFinished() && g_flash.stream_task == nullptr);
    assert(!capture_indicator);
    RunSender();
    assert(g_uplink.Sent() == 170 && g_uplink.SenderFinished());
    assert(g_flash.active_ws_sends == 0);
#if CONFIG_WQN_FLASH_PROTOCOL_V2
    for (unsigned i = 0; i < 170; ++i) {
        assert(packet_first_samples[i] == int(i + 100));
        assert(wire_sequences[i] == i + 1);
    }
#endif
    assert(g_uplink.ClaimFinal(gen));
    g_uplink.FinalDone(gen, true);
}
void TestRelease() {
    auto gen = Reset(); Queue(3, gen);
    OnFlashButtonReleased(true);
    assert(g_uplink.State() == FlashUplink::Phase::kFinalSent);
    assert(g_uplink.Sent() == 3 && g_flash.active_ws_sends == 0);
    assert(g_flash.response_in_flight);
#if CONFIG_WQN_FLASH_PROTOCOL_V2
    assert(final_calls == 1 && wire_sequences.back() == 4);
#else
    assert(final_calls == 2);
#endif
    const auto calls = network_calls;
    OnFlashButtonReleased(true); assert(network_calls == calls);

    gen = Reset(); Queue(2, gen); short_pcm = true;
    OnFlashButtonReleased(true);
    assert(final_calls == 0 && g_uplink.Error() == FlashUplink::Failure::kTransport);
    assert(g_flash.active_ws_sends == 0 && stop_requests > 0);

    gen = Reset(); Queue(2, gen); short_final = true;
    OnFlashButtonReleased(true);
    assert(terminal_reason == FlashTerminalReason::kFinalUnconfirmed);
    assert(g_flash.active_ws_sends == 0 && g_uplink.State() == FlashUplink::Phase::kFailed);
    const auto after_failure = network_calls;
    OnFlashButtonReleased(true); assert(network_calls == after_failure);

    gen = Reset(); Queue(2, gen); g_flash.response_in_flight = true;
    OnFlashButtonReleased(true); assert(final_calls == 0 && stop_requests > 0);

    Reset(); OnFlashButtonReleased(true);
    assert(network_calls == 0 && final_calls == 0);
    assert(g_uplink.Begin(1) != 0); // empty tap did not dirty the upstream buffer

    gen = Reset(); Queue(2, gen);
    OnFlashButtonReleased(false);
    assert(network_calls == 0 && final_calls == 0 && stop_requests > 0);
    assert(g_uplink.Begin(1) == 0 && g_uplink.Begin(2) != 0);

    gen = Reset(); Queue(2, gen); disconnect_during_send = true;
    OnFlashButtonReleased(true);
    assert(final_calls == 0 && g_flash.active_ws_sends == 0);

    gen = Reset(); Queue(2, gen); drain_on_stop = false; capture_stop_cost = 2800000;
    OnFlashButtonReleased(true);
    assert(clock_us == 3000000 && final_calls == 0 && network_calls == 0);
    assert(g_uplink.Stopped());
}
void TestCaptureFailures() {
    Reset(); pcm_blocks = 2; partial_first_read = true; notifications = 0;
    try { AudioStreamingTask(nullptr); } catch (const WorkerWait&) {}
    RunSender();
    assert(g_uplink.Accepted() == 2 && g_uplink.Sent() == 2);
#if CONFIG_WQN_FLASH_PROTOCOL_V2
    assert(packet_sizes[0] == 384 && packet_sizes[1] == 744);
#endif
    for (int fault = 0; fault < 2; ++fault) {
        Reset(); pcm_blocks = 2; notifications = 0;
        malformed_first_read = fault == 0; fatal_first_read = fault == 1;
        try { AudioStreamingTask(nullptr); } catch (const WorkerWait&) {}
        RunSender();
        assert(g_uplink.Accepted() == 0 && g_uplink.Error() == FlashUplink::Failure::kInvalidAudio);
        assert(stop_requests > 0 && final_calls == 0);
    }
    Reset(); pcm_blocks = 205; notifications = 0;
    try { AudioStreamingTask(nullptr); } catch (const WorkerWait&) {}
    RunSender();
    assert(g_uplink.Accepted() == 192 && g_uplink.Error() == FlashUplink::Failure::kOverflow);
    assert(terminal_reason == FlashTerminalReason::kUplinkOverflow && final_calls == 0);
}
void TestOverflowAndGenerations() {
    auto gen = Reset(); Queue(192, gen);
    const int16_t sample = 7;
    assert(!g_uplink.Push(gen, &sample, 1));
    assert(g_uplink.Error() == FlashUplink::Failure::kOverflow);
    g_uplink.ProducerDone(gen); RunSender();
    assert(!g_uplink.ClaimFinal(gen) && final_calls == 0 && g_uplink.Peak() == 192);
    assert(g_uplink.Begin(1) == 0);
    auto next = g_uplink.Begin(2); assert(next != 0);
    g_uplink.ProducerDone(gen); g_uplink.Cancel(gen); g_uplink.RecordSent(gen);
    assert(!g_uplink.Push(gen, &sample, 1));
    assert(g_uplink.State() == FlashUplink::Phase::kRecording && !g_uplink.ProducerFinished());
    assert(g_uplink.Push(next, &sample, 1));
    g_uplink.Cancel(next);
    assert(g_uplink.SenderDone(next));
    assert(g_uplink.Begin(3) == 0); // producer has not acknowledged cancellation
    g_uplink.ProducerDone(next);
    assert(g_uplink.Begin(3) != 0);
}
void TestTeardown() {
    auto gen = Reset(); Queue(3, gen);
    assert(StopFlashSessionNow() == ESP_OK);
    assert(network_calls == 0 && destroyed_clients == 1 && ended_audio_sessions == 1);
    assert(g_flash.ws_client == nullptr && !client.alive);

    gen = Reset(); Queue(3, gen); drain_on_stop = false;
    assert(StopFlashSessionNow() == ESP_ERR_TIMEOUT);
    assert(client.alive && g_flash.ws_client == &client && destroyed_clients == 0 && ended_audio_sessions == 0);
    RunSender(); assert(StopFlashSessionNow() == ESP_OK);
    assert(destroyed_clients == 1 && ended_audio_sessions == 1);

    gen = Reset(); Queue(3, gen); producer_ack = false; capture_stop_cost = 3000000;
    assert(StopFlashSessionNow() == ESP_ERR_TIMEOUT);
    assert(client.alive && destroyed_clients == 0 && ended_audio_sessions == 0);
    g_uplink.ProducerDone(gen); g_flash.stream_task = nullptr;
    producer_ack = true;
    assert(StopFlashSessionNow() == ESP_OK);
    assert(destroyed_clients == 1 && ended_audio_sessions == 1);

    gen = Reset(); Queue(3, gen); g_flash.active_ws_sends = 1;
    wait_hook = [] { if (clock_us >= 400000) g_flash.active_ws_sends = 0; };
    assert(StopFlashSessionNow() == ESP_OK);
    assert(clock_us >= 400000 && destroyed_clients == 1 && ended_audio_sessions == 1);
}
void TestConcurrentRing() {
    wqn::FlashUplinkQueue<64, 8> queue;
    decltype(queue)::Block slots[decltype(queue)::kStorageSlots]{};
    queue.Configure(slots);
    const auto gen = queue.Begin(1);
    constexpr unsigned count = 40000;
    std::atomic<unsigned> consumed{0};
    std::thread producer([&] {
        int16_t data[8];
        for (unsigned i = 0; i < count; ++i) {
            while (i - consumed.load(std::memory_order_acquire) >= 32) std::this_thread::yield();
            std::fill(std::begin(data), std::end(data), static_cast<int16_t>(i));
            assert(queue.Push(gen, data, 8));
        }
        queue.ProducerDone(gen);
    });
    std::thread consumer([&] {
        decltype(queue)::Block block;
        for (unsigned i = 0; i < count;) {
            if (!queue.Pop(gen, &block)) { std::this_thread::yield(); continue; }
            for (auto sample : block.samples) assert(sample == static_cast<int16_t>(i));
            queue.RecordSent(gen); ++i; consumed.store(i, std::memory_order_release);
        }
        while (!queue.ProducerFinished()) std::this_thread::yield();
        assert(queue.SenderDone(gen));
    });
    producer.join(); consumer.join();
    assert(queue.Accepted() == count && queue.Sent() == count && queue.ClaimFinal(gen));
    queue.Cancel(gen); queue.FinalDone(gen, true);
    assert(queue.State() == decltype(queue)::Phase::kCancelled);
}
int main() {
#if BASELINE_CAPTURE
    Reset(); pcm_blocks = 170; first_network_stall_us = 2500000;
    try { AudioStreamingTask(nullptr); } catch (const WorkerWait&) {}
    assert(network_calls == 170 && clock_us - 4 * 15000 == 5050000);
    std::puts("CAPTURE_SIMULATION baseline blocks=170 capture_loop_ms=5050 network_calls_during_capture=170 simulated_first_send_stall_ms=2500");
#else
    TestCapture(); TestCaptureFailures(); TestRelease(); TestOverflowAndGenerations(); TestTeardown(); TestConcurrentRing();
    std::printf("Flash mode=%d production capture/sender/FINAL cases + 40000 ordered blocks passed; storage=%zu bytes\n",
                CONFIG_WQN_FLASH_PROTOCOL_V2, FlashUplink::kStorageBytes);
#endif
}
