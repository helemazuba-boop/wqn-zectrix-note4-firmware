// Production owner consumer and teardown; SDK/queue/ledger publication ports.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sse_chunk.h"
#include "stdpro_ws_transport.h"
#include "voice_diagnostics.h"
#include "device_protocol/json_depth_guard.h"

@@TYPES@@

static int checks = 0;
static std::vector<std::string> log_lines;
int64_t esp_timer_get_time() { return 0; }
void HostLog(const char* tag, const char* format, ...) {
    char line[1536];
    va_list args; va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args); va_end(args);
    log_lines.emplace_back(std::string(tag) + " " + line);
}
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
constexpr char kTag[] = "ws-test";
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4;
static char psram_block, internal_block;
static bool psram_available = true, internal_available = true;
static std::vector<int> alloc_caps;
static void* heap_caps_malloc(size_t, int caps) {
    alloc_caps.push_back(caps);
    if (caps & MALLOC_CAP_SPIRAM) return psram_available ? &psram_block : nullptr;
    return internal_available ? &internal_block : nullptr;
}
static bool esp_ptr_external_ram(void* ptr) { return ptr == &psram_block; }
static size_t heap_caps_get_largest_free_block(int caps) {
    return caps == MALLOC_CAP_INTERNAL ? 8192 : 65536;
}
static std::atomic<ConnectionState> g_conn_state{ConnectionState::kReady};
static std::atomic<TurnState> g_turn_state{TurnState::kIdle};
static std::atomic<bool> g_is_connected{true}, g_session_ready{true};
static uint32_t g_active_conn_op_gen = 0, g_active_turn_gen = 0;
static int64_t g_conn_deadline_ms = 0;
static char g_active_turn_req_id[64]{};
static std::string g_current_req_id, g_control_reassembly_buf;
static wqn::stdpro_ws::TurnStreamObservation g_active_turn_stream_obs{};
static wqn::stdpro_ws::FinalHandoffResult g_final_result{};
static wqn::SseFrameBuffer g_sse_parser;
static wqn::WqnAiSseCallback g_sse_callback = nullptr;
static void* g_sse_user_ctx = nullptr;
static int handle;
static void* g_ws_client = nullptr;
static void* g_text_stream = &handle;
static void* g_pcm_queue = &handle;
static void* g_final_sem = &handle;
static void* g_transport_task = &handle;
constexpr int pdTRUE = 1, eSetBits = 1, kEvtBitResetRequired = 1;
struct PcmBlock {};
static std::string input;
static size_t offset = 0, packet_limit = 512;
static int stopped = 0, destroyed = 0, signalled = 0, notified = 0;
struct Terminal {
    uint32_t gen;
    std::string request_id;
    wqn::stdpro_ws::TurnReleaseState release;
    wqn::stdpro_ws::TurnStreamObservation observation;
};
static std::vector<Terminal> terminals;
static std::vector<wqn::WqnAiSseEvent> events;
static void esp_websocket_client_stop(void*) { ++stopped; }
static void esp_websocket_client_destroy(void*) { ++destroyed; }
static void PublishCompletion(uint32_t, esp_err_t, CmdDisposition) {}
static void PublishTurnTerminalRecord(uint32_t gen, const char* request_id,
        wqn::stdpro_ws::TurnReleaseState release, wqn::stdpro_ws::TurnStreamObservation observation) {
    terminals.push_back({gen, request_id, release, observation});
}
static void xStreamBufferReset(void*) { offset = input.size(); }
static int xQueueReceive(void*, PcmBlock*, int) { return 0; }
static void LogTransportMemory(const char*, const char* = nullptr, uint32_t = 0) {}
static void xSemaphoreGive(void*) { ++signalled; }
[[maybe_unused]] static void xTaskNotify(void*, int, int) { ++notified; }
static size_t xStreamBufferReceive(void*, char* dst, size_t size, int) {
    const size_t count = std::min({size, packet_limit, input.size() - offset});
    std::memcpy(dst, input.data() + offset, count); offset += count; return count;
}

@@PRODUCTION@@

static const std::string release_frame = "event: turn.released\ndata: {\"request_id\":\"req\"}\n\n";
static const std::string done_frame = "event: turn.done\ndata: {\"request_id\":\"req\"}\n\n";
static void Callback(const wqn::WqnAiSseEvent& event, void*) { events.push_back(event); }
static void Reset(TurnState state) {
    DoDestroyWs();
    CHECK(!g_sse_parser.overflowed());
    stopped = destroyed = signalled = notified = 0;
    events.clear(); terminals.clear(); input.clear(); offset = 0;
    g_ws_client = &handle; g_session_ready.store(true); g_is_connected.store(true);
    g_conn_state.store(ConnectionState::kReady); g_turn_state.store(state);
    g_active_turn_gen = 7; std::strcpy(g_active_turn_req_id, "req"); g_current_req_id = "req";
    g_active_turn_stream_obs = wqn::stdpro_ws::TurnStreamObservation::kNone;
    g_sse_callback = Callback;
}
static void Feed(const std::string& text) { input = text; offset = 0; ProcessTextStream(); }
int main() {
    using Release = wqn::stdpro_ws::TurnReleaseState;
    using Observation = wqn::stdpro_ws::TurnStreamObservation;
    // Actual allocator: fallback success differs from allocation failure, and
    // diagnostic metadata does not change the preference/order of allocations.
    bool in_psram = false;
    CHECK(AllocPrefersPsram(32, &in_psram, "owner_stack", "req") == &psram_block);
    CHECK(in_psram && alloc_caps == std::vector<int>{MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT});
    CHECK(log_lines.empty());
    psram_available = false; alloc_caps.clear();
    CHECK(AllocPrefersPsram(32, &in_psram, "owner_stack", "req") == &internal_block);
    CHECK(!in_psram && alloc_caps.size() == 2 && alloc_caps.back() == MALLOC_CAP_8BIT);
    CHECK(log_lines.size() == 1);
    CHECK(log_lines.back().find("req=req resource=owner_stack bytes=32") != std::string::npos);
    CHECK(log_lines.back().find("result=ok placement=internal") != std::string::npos);
    internal_available = false; log_lines.clear(); alloc_caps.clear();
    CHECK(AllocPrefersPsram(32, nullptr, "pcm_queue", "unsafe id\n") == nullptr);
    CHECK(alloc_caps.size() == 2 && log_lines.size() == 1);
    CHECK(log_lines.back().find("req=redacted resource=pcm_queue") != std::string::npos);
    CHECK(log_lines.back().find("result=failed placement=none internal_largest=8192 psram_largest=65536") != std::string::npos);
    CHECK(log_lines.back().find("unsafe id") == std::string::npos);
    for (size_t chunk : {size_t(1), size_t(17), size_t(512)}) {
        packet_limit = chunk;
        Reset(TurnState::kRemoteHandoff);
        Feed(done_frame + release_frame);
        CHECK(events.size() == 2 && terminals.size() == 1);
        CHECK(terminals[0].gen == 7 && terminals[0].request_id == "req");
        CHECK(terminals[0].release == Release::kReleased && terminals[0].observation == Observation::kTurnDoneObserved);
        CHECK(g_turn_state.load() == TurnState::kIdle && destroyed == 0);
        for (TurnState state : {TurnState::kRecording, TurnState::kFinalInFlight, TurnState::kRemoteHandoff}) {
            for (bool multiline : {false, true}) {
                Reset(state);
                std::string bad;
                if (multiline) {
                    for (int i = 0; i < 20; ++i) bad += "data: " + std::string(1000, 'x') + "\n";
                } else bad.assign(20000, 'x');
                Feed(bad + "\n\n" + done_frame + release_frame);
                CHECK(events.empty()); CHECK(terminals.size() == 1);
                CHECK(stopped == 1 && destroyed == 1 && notified == 0);
                CHECK(g_turn_state.load() == TurnState::kIdle && !g_session_ready.load());
                CHECK(!g_sse_parser.overflowed());  // failed connection ended, next can recover
                if (state == TurnState::kRecording) {
                    CHECK(terminals[0].release == Release::kPreFinalFailed);
                    CHECK(signalled == 1 && g_final_result == wqn::stdpro_ws::FinalHandoffResult::kDefinitelyNotSent);
                } else if (state == TurnState::kFinalInFlight) {
                    CHECK(terminals[0].release == Release::kHandoffAmbiguous);
                    CHECK(signalled == 1 && g_final_result == wqn::stdpro_ws::FinalHandoffResult::kAmbiguous);
                } else {
                    CHECK(terminals[0].release == Release::kTransportLost);
                    CHECK(terminals[0].observation == Observation::kTransportLost && signalled == 0);
                }
                // Actual owner reset must permit the next healthy session.
                Reset(TurnState::kRemoteHandoff); Feed(done_frame + release_frame);
                CHECK(terminals.size() == 1 && terminals[0].release == Release::kReleased);
            }
        }
    }
    // clear() still preserves the parser contract; only reset() starts anew.
    wqn::SseFrameBuffer parser;
    std::string name, data; uint64_t id = 0;
    for (int i = 0; i < 20 && !parser.overflowed(); ++i) {
        const std::string line = "data: " + std::string(1000, 'x') + "\n";
        CHECK(parser.feed(line.data(), line.size())); parser.extract(&name, &id, &data);
    }
    CHECK(parser.overflowed()); parser.clear(); CHECK(parser.overflowed());
    parser.reset(); CHECK(!parser.overflowed());
    std::printf("WS consumer/FINAL teardown: %d checks passed (host ports; not HIL)\n", checks);
}
