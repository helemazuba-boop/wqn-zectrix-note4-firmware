// Real HTTP owner + parser/decoder, with controllable SDK I/O and monotonic time.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "esp_http_client.h"
#include "wqn_api.h"
#include "voice_endpoint.h"

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
struct Scenario {
    int status = 200, write_max = 2048;
    int64_t init_us = 0, open_us = 0, write_us = 0, headers_us = 0, packet_us = 0;
    bool fail_init = false, fail_open = false, fail_write = false, fail_headers = false;
    bool data_in_headers = false, drain_in_read = false, fail_read = false;
    std::vector<std::string> packets;
} scenario;
struct HostClient {
    esp_http_client_config_t cfg{};
    bool closed = false;
    int status = -1;
    int socket_errno = 0;
    size_t packet = 0;
};
static HostClient client;
static int64_t clock_us = 0;
static int cleanup_count = 0, close_count = 0, read_count = 0, write_count = 0, written = 0;
static std::vector<int> timeouts;
static std::map<std::string, std::string> headers;
static std::vector<wqn::WqnAiSseEvent> delivered;
static std::vector<std::string> log_lines;
void HostLog(const char* tag, const char* format, ...) {
    char line[1536];
    va_list args; va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args); va_end(args);
    log_lines.emplace_back(std::string(tag) + " " + line);
}
static int64_t callback_us = 0;
int64_t esp_timer_get_time() { return clock_us; }
esp_err_t esp_read_mac(uint8_t* mac, int) { std::memset(mac, 1, 6); return ESP_OK; }
constexpr char test_token[] = "SYNTHETIC_CREDENTIAL_MUST_NOT_BE_LOGGED";
namespace wqn { bool IsValidAccessToken(const std::string& token) { return token == test_token; } }
static void Emit(esp_http_client_event_id_t id, const std::string& data = {}) {
    esp_http_client_event_t event{id, &client, client.cfg.user_data,
                                 const_cast<char*>(data.data()), static_cast<int>(data.size())};
    // Match IDF: the parser does not use the event callback's return to stop I/O.
    client.cfg.event_handler(&event);
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* cfg) {
    clock_us += scenario.init_us;
    if (scenario.fail_init) return nullptr;
    client = HostClient{}; client.cfg = *cfg; return &client;
}
esp_err_t esp_http_client_set_header(HostClient*, const char* k, const char* v) {
    headers[k] = v; return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(HostClient*, int value) {
    CHECK(value > 0); timeouts.push_back(value); return ESP_OK;
}
esp_err_t esp_http_client_open(HostClient*, int) {
    clock_us += scenario.open_us;
    if (scenario.fail_open) return ESP_FAIL;
    Emit(HTTP_EVENT_ON_CONNECTED); return ESP_OK;
}
int esp_http_client_write(HostClient*, const char*, int len) {
    ++write_count; clock_us += scenario.write_us;
    if (scenario.fail_write) return -1;
    const int count = std::min(len, scenario.write_max); written += count; return count;
}
static int Packet() {
    if (client.packet == scenario.packets.size()) return 0;
    const auto& data = scenario.packets[client.packet++];
    clock_us += scenario.packet_us;
    Emit(HTTP_EVENT_ON_DATA, data);
    return static_cast<int>(data.size());
}
int64_t esp_http_client_fetch_headers(HostClient*) {
    clock_us += scenario.headers_us;
    Emit(HTTP_EVENT_ON_HEADER);  // status still -1 in the SDK here
    client.status = scenario.status;
    if (scenario.data_in_headers) Packet();
    return scenario.fail_headers ? -1 : 0;
}
int esp_http_client_get_status_code(HostClient*) { return client.status; }
int esp_http_client_get_errno(HostClient*) { return client.socket_errno; }
int esp_http_client_read(HostClient*, char*, int) {
    ++read_count;
    if (client.closed) return -1;
    if (scenario.fail_read) {
        client.socket_errno = ECONNRESET; errno = EINVAL; return -1;
    }
    int result = Packet();
    if (scenario.drain_in_read) {
        int guard = 0;
        while (!client.closed && result > 0) {
            CHECK(++guard < 2000); result = Packet();
        }
    }
    return client.closed ? -1 : result;
}
esp_err_t esp_http_client_close(HostClient*) {
    ++close_count;
    CHECK(!client.closed); client.closed = true;
    Emit(HTTP_EVENT_DISCONNECTED); return ESP_OK;
}
esp_err_t esp_http_client_cleanup(HostClient*) { ++cleanup_count; return ESP_OK; }

#include "wqn_api_stream.cpp"

static const std::string final_frame = "event: final\ndata: {\"conversation_id\":\"conv\",\"latency_ms\":12,\"actions\":[{\"type\":\"todo\",\"title\":\"saved\"}]}\n\n";
static const std::string delta_frame = "event: text.delta\ndata: {\"delta\":\"ok\"}\n\n";
static std::vector<int16_t> pcm(5000, 42);
static wqn::WqnAiChatResponse response;
static void Callback(const wqn::WqnAiSseEvent& ev, void*) {
    delivered.push_back(ev); clock_us += callback_us;
}
static wqn::WqnAiStreamRequest Request() {
    wqn::WqnAiStreamRequest request;
    request.token = test_token; request.pcm_data = pcm.data(); request.pcm_sample_count = pcm.size();
    request.duration_ms = 100; request.timeout_ms = 100; request.request_id = "same-id";
    request.reasoning_effort = "low"; request.enable_thinking = false; request.callback = Callback;
    return request;
}
static void Reset() {
    scenario = Scenario{}; clock_us = 0; cleanup_count = close_count = read_count = write_count = written = 0;
    headers.clear(); timeouts.clear(); delivered.clear(); log_lines.clear(); callback_us = 0; errno = 0;
    response = wqn::WqnAiChatResponse{};
}
static esp_err_t Run() {
    const auto result = wqn::UploadAiAudioChatStream(Request(), &response);
    CHECK(cleanup_count == (scenario.fail_init ? 0 : 1));
    CHECK(close_count == cleanup_count);
    return result;
}
static void Timeout() {
    CHECK(Run() == ESP_ERR_TIMEOUT); CHECK(response.error_code == "chat_timeout");
    CHECK(delivered.empty());
}
static size_t LogCount(const std::string& fragment) {
    return std::count_if(log_lines.begin(), log_lines.end(), [&](const std::string& line) {
        return line.find(fragment) != std::string::npos;
    });
}
static bool HasLog(const std::string& fragment) { return LogCount(fragment) != 0; }
int main() {
    Reset(); scenario.write_max = 137; scenario.write_us = 100;
    scenario.open_us = 5000; scenario.headers_us = 7000;
    scenario.packets = {"event: asr.complete\ndata: {\"text\":\"heard\",\"asr\":{\"provider\":\"asr\"}}\n\n", delta_frame, final_frame + delta_frame};
    CHECK(Run() == ESP_OK); CHECK(delivered.size() == 3); CHECK(written == 10000);
    CHECK(response.transcript == "heard" && response.asr.provider == "asr");
    CHECK(response.conversation_id == "conv" && response.latency_ms == 12);
    CHECK(response.actions.size() == 1 && response.actions[0].title == "saved");
    CHECK(headers["X-WQN-Request-Id"] == "same-id");
    CHECK(headers["X-WQN-Reasoning-Effort"] == "low" && headers["X-WQN-Enable-Thinking"] == "false");
    CHECK(timeouts.front() == 100 && timeouts.back() < 90);
    CHECK(std::is_sorted(timeouts.rbegin(), timeouts.rend()));
    CHECK(HasLog("event=ai.http stage=begin req=same-id budget_ms=100 pcm_bytes=10000"));
    CHECK(HasLog("stage=return req=same-id phase=read server_stage=none err=0x0 http_status=200 terminal=1"));
    CHECK(LogCount("stage=first_data ") == 1 && LogCount("stage=first_event ") == 1);
    CHECK(HasLog("callbacks=3 events=3"));
    CHECK(!HasLog(test_token) && !HasLog("Bearer ") && !HasLog("heard") && !HasLog("saved"));
    for (const char* stage : {"terminal_callback_return", "close_begin", "close_return", "read_return", "cleanup_begin", "cleanup_return"}) {
        CHECK(LogCount(std::string("stage=") + stage + " ") == 1);
    }

    Reset(); scenario.packets = {final_frame}; scenario.data_in_headers = true;
    CHECK(Run() == ESP_OK); CHECK(read_count == 0 && delivered.size() == 1);
    Reset(); scenario.packets = {"event: error\ndata: {\"error_code\":\"provider_busy\",\"message\":\"later\"}\n\n", delta_frame};
    CHECK(Run() == ESP_OK); CHECK(response.error_code == "provider_busy" && delivered.size() == 1);
    CHECK(HasLog("terminal=1 error=provider_busy") && HasLog("stage=return "));
    Reset(); CHECK(Run() == ESP_FAIL); CHECK(response.error_code == "model_failed");
    Reset(); scenario.packets = {"event: final\ndata: {}"};
    CHECK(Run() == ESP_FAIL); CHECK(delivered.empty());
    for (int status : {401, 413, 422, 429, 500, 502, 503, 504}) {
        Reset(); scenario.status = status; scenario.data_in_headers = true; scenario.packets = {final_frame};
        CHECK(Run() == ESP_FAIL); CHECK(delivered.empty());
        CHECK(response.error_code == wqn::internal::AiStreamHttpErrorCode(status)); CHECK(read_count == 0);
    }
    Reset(); scenario.status = 401; scenario.fail_headers = true;
    CHECK(Run() == ESP_FAIL); CHECK(response.error_code == "unauthorized");
    Reset(); scenario.status = -1; CHECK(Run() == ESP_FAIL); CHECK(response.error_code == "bad_response");
    Reset(); scenario.fail_init = true; CHECK(Run() == ESP_ERR_NO_MEM);
    Reset(); scenario.fail_open = true; CHECK(Run() == ESP_FAIL && write_count == 0);
    Reset(); scenario.fail_write = true; CHECK(Run() == ESP_FAIL && read_count == 0);
    CHECK(HasLog("stage=return req=same-id phase=upload"));
    Reset(); scenario.fail_read = true; CHECK(Run() == ESP_FAIL);
    CHECK(HasLog("read_result=-1 read_errno=" + std::to_string(ECONNRESET)));
    Reset(); scenario.init_us = 100001; Timeout(); CHECK(write_count == 0);
    Reset(); scenario.open_us = 100001; Timeout(); CHECK(write_count == 0);
    Reset(); scenario.write_max = 1000; scenario.write_us = 21000; Timeout(); CHECK(written == 5000);
    Reset(); scenario.open_us = 30000; scenario.write_us = 10000; scenario.headers_us = 30000;
    scenario.packets = {final_frame}; Timeout(); CHECK(read_count == 0);
    Reset(); scenario.headers_us = 100001; scenario.packets = {final_frame};
    scenario.data_in_headers = true; Timeout();
    Reset(); scenario.headers_us = 100001; scenario.status = 401; Timeout();
    Reset(); scenario.packet_us = 100001; scenario.packets = {final_frame}; Timeout();
    // One SDK read can invoke ON_DATA repeatedly without returning to its caller.
    Reset(); scenario.packet_us = 11000; scenario.drain_in_read = true;
    scenario.packets.assign(200, ": heartbeat\n\n"); scenario.packets.push_back(final_frame);
    Timeout(); CHECK(read_count == 1 && client.packet < 11);
    CHECK(LogCount("stage=deadline ") == 1 && !HasLog("stage=first_event "));
    CHECK(HasLog("progress_gap_ms=-1") && HasLog("events=0"));
    CHECK(log_lines.size() < 24);
    // Framed heartbeats still count as events, never as business progress.
    Reset(); scenario.packet_us = 11000; scenario.drain_in_read = true;
    scenario.packets = {"event: stage\ndata: {\"stage\":\"asr\"}\n\n"};
    scenario.packets.insert(scenario.packets.end(), 200, "event: heartbeat\ndata: {}\n\n");
    CHECK(Run() == ESP_ERR_TIMEOUT);
    // The deadline packet is counted as traffic, but is rejected before decode.
    CHECK(HasLog("data_gap_ms=0 event_gap_ms=11 progress_gap_ms=99"));
    CHECK(log_lines.size() < 24);
    // Thousands of content deltas do not produce thousands of log records.
    Reset(); scenario.packets.assign(1000, "event: text.delta\ndata: {\"delta\":\"PRIVATE_RESPONSE_TEXT\"}\n\n");
    scenario.packets.push_back(final_frame);
    CHECK(Run() == ESP_OK && delivered.size() == 1001);
    CHECK(log_lines.size() < 24 && !HasLog("PRIVATE_RESPONSE_TEXT"));
    CHECK(HasLog("callbacks=1001 events=1001"));
    // Even noisy stage transitions have a fixed logging cap; retain the latest.
    Reset();
    for (int i = 0; i < 40; ++i) {
        scenario.packets.push_back("event: stage\ndata: {\"stage\":\"phase_" + std::to_string(i) + "\"}\n\n");
    }
    scenario.packets.push_back(final_frame); CHECK(Run() == ESP_OK);
    CHECK(LogCount("stage=server_stage ") == 32);
    CHECK(HasLog("stage=return req=same-id phase=read server_stage=phase_39"));
    CHECK(HasLog("stage_logs=32"));
    // Malformed provider payload and unsafe metadata must not escape into logs.
    Reset(); scenario.packets = {"event: text.delta\ndata: PRIVATE_BAD_JSON\n\n", final_frame};
    CHECK(Run() == ESP_OK && HasLog("SSE JSON parse failed: bytes="));
    CHECK(!HasLog("PRIVATE_BAD_JSON"));
    Reset(); scenario.packets = {"event: stage\ndata: {\"stage\":\"PRIVATE STAGE\\nspoof\"}\n\n", final_frame};
    auto unsafe_request = Request(); unsafe_request.request_id = "PRIVATE ID\nspoof";
    CHECK(wqn::UploadAiAudioChatStream(unsafe_request, &response) == ESP_OK);
    CHECK(HasLog("req=redacted") && HasLog("server_stage=redacted"));
    CHECK(!HasLog("PRIVATE ID") && !HasLog("PRIVATE STAGE"));
    CHECK(headers["X-WQN-Request-Id"] == unsafe_request.request_id);
    CHECK(std::string(wqn::voice_diagnostics::SafeKey(nullptr)) == "none");
    CHECK(std::string(wqn::voice_diagnostics::SafeKey(std::string(64, 'x').c_str())) == "redacted");
    CHECK(std::string(wqn::voice_diagnostics::SafeKey(std::string(63, 'x').c_str())) == std::string(63, 'x'));
    // Callback work also spends the same budget; no later business frame leaks.
    Reset(); callback_us = 110000; scenario.packets = {delta_frame + final_frame};
    CHECK(Run() == ESP_ERR_TIMEOUT && delivered.size() == 1);
    // Line limit versus cumulative multi-line limit; each packet is small.
    for (bool multiline : {false, true}) {
        Reset();
        if (multiline) {
            scenario.packets.assign(20, "data: " + std::string(1000, 'x') + "\n");
        } else {
            scenario.packets.assign(20, std::string(1000, 'x'));
        }
        scenario.packets.push_back("\n\n" + final_frame);
        CHECK(Run() == ESP_FAIL); CHECK(response.error_code == "frame_overflow"); CHECK(delivered.empty());
        CHECK(client.packet < scenario.packets.size());
    }
    Reset(); scenario.headers_us = 100001;
    scenario.data_in_headers = true; scenario.packets = {std::string(17000, 'x')}; Timeout();
    auto request = Request(); request.pcm_sample_count = static_cast<size_t>(INT_MAX);
    CHECK(wqn::UploadAiAudioChatStream(request, &response) == ESP_ERR_INVALID_ARG);

    CHECK(wqn::BuildVoiceWebSocketUrl() == "wss://wqn.helema.cn/api/esp32/realtime");
    CHECK(wqn::BuildVoiceWebSocketUrl("http://dev.local:3000/api/esp32", "/voice", "") == "ws://dev.local:3000/voice");
    CHECK(wqn::BuildVoiceWebSocketUrl("https://[::1]:8443/prefix/api", "/ws", "") == "wss://[::1]:8443/ws");
    CHECK(wqn::BuildVoiceWebSocketUrl("https://rest.example/api", "/ws", "wss://voice.example/custom?q=1") == "wss://voice.example/custom?q=1");
    for (const std::string invalid : {"ftp://host/api", "https:///api", "https://u:p@host/api", "https://host/a b", "https://host/#fragment"}) {
        CHECK(wqn::BuildVoiceWebSocketUrl(invalid, "/ws", "").empty());
    }
    CHECK(wqn::BuildVoiceWebSocketUrl("https://host/api", "//other", "").empty());
    CHECK(wqn::BuildVoiceWebSocketUrl("https://host/api", "/ws", "https://wrong/ws").empty());
    std::printf("HTTP SSE/endpoint: %d checks passed (host I/O; not HIL)\n", checks);
}
