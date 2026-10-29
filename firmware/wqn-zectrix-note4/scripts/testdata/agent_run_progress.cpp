#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "cJSON.h"
#include "opencode_client.h"
#include "opencode_model.h"
#include "agent_run_progress.h"
#include "esp_http_client.h"
#include "esp_log.h"

constexpr int64_t first_ms = wqn::AgentRunProgress::kFirstEventUs / 1000;
constexpr int64_t progress_ms = wqn::AgentRunProgress::kProgressUs / 1000;
constexpr char request_id[] = "0011223344556677";
int64_t clock_ms = 0;
int inits = 0, stream_opens = 0, run_posts = 0, reply_posts = 0, interrupts = 0;
int closes = 0, cleanups = 0, live_clients = 0, peak_clients = 0, failed_replies = 0;
size_t checks = 0, failures = 0;
std::string submitted_body, reply_url;
std::vector<wqn::OpenCodeEvent> delivered;
wqn::OpenCodeOutboundQueue outbound;
std::atomic<bool> cancel_requested{false}, cancel_delivered{false};
std::atomic<bool> switch_requested{false}, switch_delivered{false};
struct Packet { int64_t at; std::string bytes; size_t offset = 0; };
struct Action { int64_t at; std::function<void()> run; };
std::vector<Packet> packets;
std::vector<Action> actions;
struct Scenario {
    int reads = 0, max_reads = 7202, status = 200, reply_failures = 0;
    bool fail_init = false, fail_headers = false, fail_timeout_config = false;
    std::function<void()> reply_hook;
} scenario;
struct HostClient {
    std::string url, response;
    int method = 0, timeout_ms = 1000, status = 200;
    bool stream = false, read_response = false;
};

void Check(bool pass, const char* message)
{
    ++checks;
    if (!pass) {
        if (failures < 14) std::fprintf(stderr, "FAIL %s at %lldms\n", message, static_cast<long long>(clock_ms));
        ++failures;
    }
}
void HostLog(const char*, const char*, ...) {}
int64_t esp_timer_get_time() { return clock_ms * 1000; }

HostClient* esp_http_client_init(const esp_http_client_config_t* config)
{
    ++inits;
    if (scenario.fail_init) return nullptr;
    auto* client = new HostClient;
    client->url = config->url;
    client->method = config->method;
    client->timeout_ms = config->timeout_ms;
    client->stream = client->url.find("/run?") != std::string::npos || client->url.find("/events?") != std::string::npos;
    ++live_clients;
    peak_clients = std::max(peak_clients, live_clients);
    return client;
}
int esp_http_client_set_header(HostClient*, const char*, const char*) { return ESP_OK; }
int esp_http_client_set_timeout_ms(HostClient* client, int ms)
{
    if (scenario.fail_timeout_config) return ESP_FAIL;
    client->timeout_ms = ms;
    return ESP_OK;
}
int esp_http_client_open(HostClient* client, int)
{
    if (client->stream) ++stream_opens;
    return ESP_OK;
}
int esp_http_client_write(HostClient* client, const char* bytes, int size)
{
    if (client->stream) { ++run_posts; submitted_body.assign(bytes, size); }
    else if (client->url.find("/interrupt") != std::string::npos) ++interrupts;
    else { ++reply_posts; reply_url = client->url; }
    return size;
}
int64_t esp_http_client_fetch_headers(HostClient* client)
{
    if (client->stream) {
        client->status = scenario.status;
        if (scenario.fail_headers) return -1;
        if (client->status != 200) client->response = "{\"success\":false,\"error\":{\"code\":\"run_in_progress\",\"message\":\"already running\"}}";
    } else {
        if (scenario.reply_hook && client->url.find("/interrupt") == std::string::npos) {
            auto hook = std::move(scenario.reply_hook); scenario.reply_hook = {}; hook();
        }
        if (client->url.find("/interrupt") != std::string::npos) {
            client->response = "{\"success\":true,\"data\":{\"interrupted\":false}}";
        } else if (scenario.reply_failures > 0) {
            --scenario.reply_failures;
            client->status = 503;
            client->response = "{\"success\":false,\"error\":{\"code\":\"upstream_error\",\"message\":\"scripted reply failure\"}}";
        } else client->response = "{\"success\":true,\"data\":{}}";
    }
    return 0;
}
int esp_http_client_get_status_code(HostClient* client) { return client->status; }
int esp_http_client_read(HostClient* client, char* data, int size)
{
    if (!client->stream || client->status != 200) {
        if (client->read_response) return 0;
        client->read_response = true;
        const int count = static_cast<int>(std::min(client->response.size(), static_cast<size_t>(size)));
        std::memcpy(data, client->response.data(), count);
        return count;
    }
    if (++scenario.reads > scenario.max_reads) return 0;
    const int64_t end = clock_ms + client->timeout_ms;
    while (!actions.empty() && actions.front().at <= end &&
           (packets.empty() || actions.front().at <= packets.front().at)) {
        auto action = std::move(actions.front()); actions.erase(actions.begin());
        clock_ms = std::max(clock_ms, action.at);
        action.run();
    }
    if (!packets.empty() && packets.front().at <= end) {
        auto& packet = packets.front();
        clock_ms = std::max(clock_ms, packet.at);
        const int count = static_cast<int>(std::min(static_cast<size_t>(size), packet.bytes.size() - packet.offset));
        std::memcpy(data, packet.bytes.data() + packet.offset, count);
        packet.offset += count;
        if (packet.offset == packet.bytes.size()) packets.erase(packets.begin());
        return count;
    }
    clock_ms = end;
    return -ESP_ERR_HTTP_EAGAIN;
}
int esp_http_client_close(HostClient* client) { if (client->stream) ++closes; return ESP_OK; }
int esp_http_client_cleanup(HostClient* client)
{
    if (client->stream) ++cleanups;
    --live_clients;
    delete client;
    return ESP_OK;
}

void Event(int64_t at, const std::string& name, const std::string& json = "{}")
{
    packets.push_back({at, "event: " + name + "\ndata: " + json + "\n\n"});
}
void ActionAt(int64_t at, std::function<void()> run) { actions.push_back({at, std::move(run)}); }
void Callback(const wqn::OpenCodeEvent& event, void*) { delivered.push_back(event); }
void ReplyFailed(const wqn::OpenCodeOutboundReply&, esp_err_t, void*) { ++failed_replies; }
void Reset()
{
    Check(live_clients == 0, "prior stream released all HTTP clients");
    clock_ms = 0;
    inits = stream_opens = run_posts = reply_posts = interrupts = closes = cleanups = peak_clients = failed_replies = 0;
    scenario = {};
    submitted_body.clear(); reply_url.clear(); delivered.clear(); packets.clear(); actions.clear();
    cancel_requested = cancel_delivered = switch_requested = switch_delivered = false;
    wqn::OpenCodeOutboundReply ignored;
    while (outbound.Pop(&ignored)) {}
}
esp_err_t Run(wqn::OpenCodeResult* result, bool observe = false)
{
    const std::string token = "synthetic-device-token", session = "ses_parent";
    if (observe) return wqn::WatchOpenCodeSession(token, session, 2, &outbound, ReplyFailed, nullptr,
        &cancel_requested, &cancel_delivered, &switch_requested, &switch_delivered, Callback, nullptr, result);
    return wqn::RunOpenCodePrompt(token, session, 2, "synthetic prompt", request_id, &outbound, ReplyFailed, nullptr,
        &cancel_requested, &cancel_delivered, &switch_requested, &switch_delivered, Callback, nullptr, result);
}
void OneRunOnly()
{
    Check(stream_opens == 1 && run_posts == 1 && interrupts == 0, "timeout does not replay or interrupt submitted run");
    Check(closes == 1 && cleanups == 1 && live_clients == 0, "owner closes stream and frees client once");
    cJSON* root = cJSON_Parse(submitted_body.c_str());
    const char* id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "request_id"));
    Check(id != nullptr && std::string(id) == request_id, "logical request identity reaches wire unchanged");
    cJSON_Delete(root);
}
void Question(int64_t at, const char* id = "q1", const char* owner = "ses_child")
{
    Event(at, "agent.question", std::string("{\"session_id\":\"") + owner + "\",\"question_id\":\"" + id +
          "\",\"title\":\"Choose\",\"options\":[{\"value\":\"yes\",\"label\":\"Yes\"}]}");
}
void Permission(int64_t at)
{
    Event(at, "agent.permission", "{\"session_id\":\"ses_child\",\"permission_id\":\"p1\",\"type\":\"tool\",\"title\":\"Allow\",\"preview\":\"synthetic\"}");
}
void Reply(bool question, const char* id, const char* owner = "ses_child")
{
    wqn::OpenCodeOutboundReply reply;
    reply.is_question = question;
    reply.question_id = question ? id : "";
    reply.permission_id = question ? "" : id;
    reply.session_id = owner;
    reply.answer = "yes";
    reply.approve = true;
    outbound.Push(std::move(reply));
}

size_t allocation_attempts = 0, fail_at = 0;
bool injected = false;
void* Allocate(size_t size)
{
    if (++allocation_attempts == fail_at && fail_at != 0) { injected = true; return nullptr; }
    return std::malloc(size);
}

@@OWNER_PROBE@@

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (const char* payload: {R"({"delta":"visible\u0000hidden"})",R"({"delta\u0000hidden":"visible"})"}) {
        wqn::OpenCodeEvent event;
        Check(wqn::ParseOpenCodeAgentFrame("agent.text.delta",payload,&event)==ESP_ERR_INVALID_RESPONSE,
              "actual Agent typed parser rejects NUL value/key aliases");
    }
    wqn::OpenCodeEvent literal;
    Check(wqn::ParseOpenCodeAgentFrame("agent.text.delta",R"({"delta":"literal\\u0000"})",&literal)==ESP_OK &&
          literal.text==R"(literal\u0000)","Agent preserves a literal backslash-u0000 delta");
    wqn::OpenCodeResult result;
    Reset();
    const auto silent = Run(&result);
    Check(silent == ESP_ERR_TIMEOUT && result.error_code == "run_first_event_timeout", "silent POST has initial event deadline");
    Check(clock_ms >= first_ms && clock_ms <= first_ms + 1000, "first event deadline does not wait for scripted EOF");
    std::printf("silent submitted run: result=%d code=%s elapsed_ms=%lld\n", silent, result.error_code.c_str(), static_cast<long long>(clock_ms));
    OneRunOnly();
    Reset(); clock_ms = 1LL << 40;
    const int64_t started = clock_ms;
    Check(Run(&result) == ESP_ERR_TIMEOUT && clock_ms - started == first_ms,
          "long monotonic uptime does not narrow the deadline clock");
    OneRunOnly();
    for (const std::string& frame : {std::string(": keepalive\n\n"),
                                   std::string("event: future.event\ndata: {}\n\n"),
                                   std::string("event: agent.question\ndata: {}\n\n"),
                                   std::string("event: agent.status\ndata: {bad}\n\n")}) {
        Reset();
        for (int64_t at = 1000; at < first_ms * 3; at += 1000) packets.push_back({at, frame});
        Check(Run(&result) == ESP_ERR_TIMEOUT && result.error_code == "run_first_event_timeout",
              "comments, unknown events and invalid asks do not count as progress");
        Check(clock_ms <= first_ms + 1000, "nonbusiness input cannot renew initial deadline");
        OneRunOnly();
    }
    Reset();
    for (int64_t at = 1000; at < first_ms * 3; at += 1000) packets.push_back({at, "e"});
    Check(Run(&result) == ESP_ERR_TIMEOUT && result.error_code == "run_first_event_timeout",
          "partial frame trickle does not count as business progress");
    Check(clock_ms <= first_ms + 1000, "partial input has bounded initial wait");
    OneRunOnly();
    Reset();
    for (int64_t at = 1000; at < 700000; at += 10000) Event(at, "agent.accepted");
    Check(Run(&result) == ESP_ERR_TIMEOUT && result.error_code == "run_progress_timeout", "repeated receipts do not renew progress");
    Check(clock_ms >= 1000 + progress_ms && clock_ms <= 2000 + progress_ms, "receipt-only stream has bounded quiet progress");
    OneRunOnly();
    Reset();
    Event(1000, "agent.accepted");
    Event(250000, "agent.text.delta", "{\"delta\":\"working\"}");
    Event(500000, "agent.reasoning.delta", "{\"delta\":\"still working\"}");
    Event(750000, "agent.status", "{\"status\":\"idle\"}");
    Check(Run(&result) == ESP_OK && clock_ms == 750000, "validated business progress renews idle budget");
    OneRunOnly();
    Reset();
    Event(1000, "agent.accepted");
    Event(10000, "agent.tool", "{\"tool\":\"test\",\"call_id\":\"c1\",\"status\":\"running\",\"preview\":\"started\"}");
    Check(Run(&result) == ESP_ERR_TIMEOUT && result.error_code == "run_progress_timeout", "stalled run after real work detaches with unknown outcome");
    Check(clock_ms >= 10000 + progress_ms && clock_ms <= 11000 + progress_ms, "progress timeout uses last business update");
    OneRunOnly();
    Reset();
    Question(1000);
    ActionAt(1800000, [] { Reply(true, "q1"); });
    Event(1802000, "agent.status", "{\"status\":\"idle\"}");
    Check(Run(&result) == ESP_OK && clock_ms == 1802000, "waiting for user can exceed progress deadline");
    Check(reply_posts == 1 && reply_url.find("/ses_child/question") != std::string::npos && peak_clients == 2,
          "accepted subagent reply is session-bound and uses existing short reply lane");
    OneRunOnly();
    Reset();
    Permission(1000); Question(2000);
    ActionAt(1000000, [] { Reply(false, "p1"); });
    ActionAt(2000000, [] { Reply(true, "q1"); });
    Event(2002000, "agent.status", "{\"status\":\"idle\"}");
    Check(Run(&result) == ESP_OK && clock_ms == 2002000, "permission reply does not expire a deferred question");
    Check(reply_posts == 2, "both pending ask identities are answered");
    OneRunOnly();
    Reset();
    Permission(1000); Question(2000); Question(3000, "q_unshown");
    ActionAt(1000000, [] { Reply(false, "p1"); });
    ActionAt(2000000, [] { Reply(true, "q1"); });
    Check(Run(&result) == ESP_ERR_TIMEOUT && result.error_code == "run_progress_timeout",
          "unshown third ask cannot replace the answerable deferred question");
    Check(clock_ms >= 2000000 + progress_ms && clock_ms <= 2001000 + progress_ms,
          "answering visible asks resumes deadline even after extra ask violation");
    OneRunOnly();
    Reset();
    Question(1000); Permission(2000);
    ActionAt(1000000, [] { Reply(false, "p1"); });
    Check(Run(&result) == ESP_ERR_TIMEOUT && result.error_code == "run_progress_timeout",
          "permission supersedes an obsolete question exactly like UI policy");
    Check(clock_ms >= 1000000 + progress_ms && clock_ms <= 1001000 + progress_ms,
          "superseded question cannot suspend run forever after permission reply");
    OneRunOnly();
    Reset();
    Question(1000);
    scenario.reply_failures = 1;
    ActionAt(500000, [] { Reply(true, "q1"); });
    ActionAt(1000000, [] { Reply(true, "q1"); });
    Check(Run(&result) == ESP_ERR_TIMEOUT && result.error_code == "run_progress_timeout", "successful reply resumes bounded progress wait");
    Check(failed_replies == 1 && reply_posts == 2 && clock_ms >= 1000000 + progress_ms && clock_ms <= 1001000 + progress_ms,
          "failed reply preserves waiting; successful retry starts a fresh progress budget");
    OneRunOnly();
    Reset();
    Question(1000);
    ActionAt(500000, [] { Reply(true, "q1", "ses_wrong"); });
    ActionAt(1000000, [] { switch_requested = true; });
    Check(Run(&result) == ESP_OK && switch_delivered && clock_ms == 1000000,
          "mismatched reply cannot disarm waiting for the actual ask");
    Check(interrupts == 0, "detaching waiting stream does not stop cloud execution");
    Reset();
    Question(1000);
    ActionAt(500000, [] { Reply(true, "q_old_step"); });
    ActionAt(1000000, [] { switch_requested = true; });
    Check(Run(&result) == ESP_OK && switch_delivered && clock_ms == 1000000,
          "stale question-step reply cannot clear current question identity");
    Reset();
    Event(7200000, "agent.status", "{\"status\":\"idle\"}");
    Check(Run(&result, true) == ESP_OK && clock_ms == 7200000 && run_posts == 0,
          "intentional GET observation has no new run progress deadline");
    Reset();
    ActionAt(first_ms, [] { cancel_requested = true; switch_requested = true; });
    Check(Run(&result) == ESP_OK && cancel_delivered && interrupts == 1, "explicit cancel wins at deadline and ahead of switch");
    Check(run_posts == 1 && cleanups == 1 && live_clients == 0, "cancel cleans current stream without replay");
    Reset();
    ActionAt(first_ms, [] { switch_requested = true; });
    Check(Run(&result) == ESP_OK && switch_delivered && interrupts == 0 && clock_ms == first_ms,
          "explicit view switch wins over deadline without interrupt POST");
    Reset();
    ActionAt(1000, [] { Reply(true, "stale"); });
    scenario.reply_hook = [] { clock_ms = first_ms; cancel_requested = true; };
    Check(Run(&result) == ESP_OK && cancel_delivered && interrupts == 1,
          "cancel arriving during reply POST wins over the following deadline check");
    Check(run_posts == 1 && cleanups == 1 && live_clients == 0, "reply-time cancel still cleans stream without replay");
    Reset();
    scenario.status = 409;
    Check(Run(&result) != ESP_OK && result.http_status == 409 && result.error_code == "run_in_progress", "JSON 409 remains available to owner downgrade");
    OneRunOnly();
    Reset();
    scenario.fail_timeout_config = true;
    Check(Run(&result) != ESP_OK && scenario.reads == 0, "failed short-read configuration does not enter unbounded read");
    OneRunOnly();
    // Fault every cJSON builder allocation; failed construction must not reach
    // esp_http_client_init even if the missing field was only request_id.
    cJSON_Hooks hooks{Allocate, std::free};
    cJSON_InitHooks(&hooks);
    Reset(); scenario.fail_init = true; allocation_attempts = 0;
    Check(Run(&result) == ESP_ERR_NO_MEM && inits == 1, "builder allocation profile reaches isolated client-init failure");
    const size_t sites = allocation_attempts;
    for (size_t point = 1; point <= sites; ++point) {
        Reset(); scenario.fail_init = true; allocation_attempts = 0; fail_at = point; injected = false;
        Check(Run(&result) == ESP_ERR_NO_MEM && injected, "builder allocation failure is reported");
        fail_at = 0;
        Check(inits == 0, "partial run body never reaches network initialization");
    }
    cJSON_InitHooks(nullptr);
    owner_probe::Test();
    std::printf("Agent stream/client: %zu checks, %zu failures; %zu builder allocation sites (virtual HTTP/time; NOT HIL)\n", checks, failures, sites);
    return failures == 0 ? 0 : 1;
}
