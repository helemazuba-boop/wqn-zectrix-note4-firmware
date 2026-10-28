#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "agent_voice_pipe.h"
#include "stdpro_ws_transport.h"
#include "voice_diagnostics.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

using namespace wqn::stdpro_ws;
namespace wqn { bool HostVoiceResultPublished(); }
constexpr char kTag[] = "host";
@@RECORD@@
TurnTerminalRecord g_terminal_turn_record;
#if HAS_TERMINAL_QUERY
portMUX_TYPE g_terminal_turn_mux = portMUX_INITIALIZER_UNLOCKED;
#endif
StaticSemaphore_t terminal_sem;
SemaphoreHandle_t g_turn_complete_sem = &terminal_sem;
int64_t clock_ms = 0;
int disconnects = 0, aborts = 0, critical_depth = 0, wait_returns = 0;
size_t checks = 0, failures = 0;
FinalHandoffResult handoff = FinalHandoffResult::kFinalSent;
std::function<void()> final_hook;
std::function<void()> query_enter_hook;
wqn::WqnAiSseCallback callback = nullptr;
void* callback_context = nullptr;
struct Event { int64_t at; std::function<void()> apply; };
std::vector<Event> events;

void Check(bool ok, const char* message)
{
    ++checks;
    if (!ok) {
        if (failures < 14) std::fprintf(stderr, "FAIL %s at %lld ms\n", message, static_cast<long long>(clock_ms));
        ++failures;
    }
}

void HostLog(const char*, const char* format, ...)
{
    if (std::strstr(format, "stage=wait_return") != nullptr) ++wait_returns;
}
void LogWsPhase(const char*, const char*, uint32_t, esp_err_t) {}
int64_t esp_timer_get_time() { return clock_ms * 1000; }
void HostEnter(portMUX_TYPE* mux)
{
    if (query_enter_hook) { auto hook = std::move(query_enter_hook); query_enter_hook = {}; hook(); }
    mux->lock();
    if (critical_depth++ != 0) std::abort();
}
void HostExit(portMUX_TYPE* mux) { if (--critical_depth != 0) std::abort(); mux->unlock(); }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* p) { *p = {}; return p; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* p) { *p = {true, true}; return p; }
int xSemaphoreGive(SemaphoreHandle_t p) { p->available = true; return pdTRUE; }
int xSemaphoreTake(SemaphoreHandle_t p, TickType_t ticks)
{
    if (!p->available && p->mutex) std::abort();
    if (!p->available && ticks > 0) {
        const int64_t end = clock_ms + static_cast<int64_t>(ticks) * 10;
        while (!events.empty() && events.front().at <= end && !p->available) {
            Event event = std::move(events.front());
            events.erase(events.begin());
            clock_ms = std::max(clock_ms, event.at);
            event.apply();
        }
        if (!p->available) clock_ms = end;
    }
    if (!p->available) return pdFALSE;
    p->available = false;
    return pdTRUE;
}

@@PUBLISH@@

namespace wqn {
void ArmAiVoicePreroll() {}
namespace stdpro_ws {
esp_err_t EnsureConnected(const std::string&, uint32_t, const char*) { return ESP_OK; }
esp_err_t StartTurn(const std::string&, const std::string&, const std::string&, bool, const char*, uint32_t* gen)
{
    if (gen != nullptr) *gen = 7;
    return ESP_OK;
}
void AbortTurn(const std::string&, uint32_t) { ++aborts; }
void Disconnect() { ++disconnects; }
void SetSseCallback(WqnAiSseCallback cb, void* ctx) { callback = cb; callback_context = ctx; }
FinalHandoffResult SendFinalAndWait(const std::string&, int, uint32_t)
{
    if (final_hook) { auto hook = std::move(final_hook); final_hook = {}; hook(); }
    return handoff;
}
@@QUERY@@
@@WAIT@@
} // namespace stdpro_ws
} // namespace wqn

void Schedule(int64_t at, std::function<void()> apply)
{
    events.push_back({at, std::move(apply)});
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
}
void Publish(TurnReleaseState release, uint32_t gen = 7, const char* id = "req-current")
{
    PublishTurnTerminalRecord(gen, id, release, TurnStreamObservation::kTransportLost);
}
void Asr(wqn::WqnAiSseEvent::Kind kind, std::string text = "recognized", uint64_t id = 1)
{
    wqn::WqnAiSseEvent event;
    event.kind = kind;
    event.text = std::move(text);
    event.event_id = id;
    event.request_id = "req-current";
    event.error_code = "asr_failed";
    callback(event, callback_context);
}
void Reset()
{
    events.clear();
    clock_ms = 0;
    disconnects = aborts = wait_returns = 0;
    g_terminal_turn_record = {};
    terminal_sem = {};
    handoff = FinalHandoffResult::kFinalSent;
    final_hook = {};
    query_enter_hook = {};
    wqn::AgentVoiceRegisterDeltaCallback(nullptr, nullptr);
    uint32_t gen = 0;
    Check(wqn::AgentVoiceTurnStart("synthetic-token", "req-current", &gen) == ESP_OK && gen == 7,
          "production pipe starts a turn");
}

int main()
{
    using Kind = wqn::WqnAiSseEvent::Kind;
    std::string text;
    for (uint32_t timeout : {15000U, 120000U}) {
        Reset();
        Schedule(100, [] { Publish(TurnReleaseState::kTransportLost); });
        const auto result = wqn::AgentVoiceTurnFinalize(1000, timeout, &text);
        Check(result == ESP_ERR_INVALID_RESPONSE && text.empty(), "terminal without ASR returns failure without batch fallback");
        Check(clock_ms <= 200 && wait_returns == 0 && disconnects == 0, "transport loss ends finalize within one polling slice");
        std::printf("terminal at 100ms, budget=%u, return=%lldms, result=%d\n", timeout,
                    static_cast<long long>(clock_ms), result);
    }
    for (auto release : {TurnReleaseState::kReleased, TurnReleaseState::kAborted,
                         TurnReleaseState::kPreFinalFailed, TurnReleaseState::kHandoffAmbiguous,
                         TurnReleaseState::kLocallyAbandoned}) {
        Reset();
        Publish(release);
        Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_ERR_INVALID_RESPONSE,
              "each matching terminal without ASR fails conservatively");
        Check(clock_ms == 0 && disconnects == 0, "prepublished terminal is read without waiting or teardown");
    }
    // Callback result wins over a transport terminal published in the same slice.
    for (bool terminal_first : {false, true}) {
        Reset();
        Schedule(100, [=] {
            if (terminal_first) Publish(TurnReleaseState::kReleased);
            Asr(Kind::kAsrComplete);
            if (!terminal_first) Publish(TurnReleaseState::kReleased);
        });
        Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_OK && text == "recognized",
              "ASR complete and terminal in one owner slice preserve transcript");
        Check(clock_ms == 100 && wait_returns == 0, "complete does not wait for release");
    }
    for (auto kind : {Kind::kAsrFailed, Kind::kError}) {
        Reset();
        Schedule(100, [=] { Asr(kind); });
        Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_ERR_INVALID_RESPONSE,
              "real trampoline error exits promptly");
        Check(clock_ms == 100 && wait_returns == 0, "ASR failure retains prior fast path");
    }
    for (auto kind : {Kind::kAsrComplete, Kind::kAsrFailed}) {
        Reset();
        bool observer_before_publication = false;
        wqn::AgentVoiceRegisterDeltaCallback([](const wqn::WqnAiSseEvent&, void* context) {
            *static_cast<bool*>(context) = !wqn::HostVoiceResultPublished();
        }, &observer_before_publication);
        Schedule(100, [=] { Asr(kind); });
        const auto result = wqn::AgentVoiceTurnFinalize(1000, 15000, &text);
        Check(observer_before_publication, "observer finishes before worker sees ASR terminal flags");
        Check(result == (kind == Kind::kAsrComplete ? ESP_OK : ESP_ERR_INVALID_RESPONSE),
              "observer ordering preserves complete/error result");
    }
    Reset();
    Schedule(100, [] { Asr(Kind::kAsrComplete); });
    Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_OK && text == "recognized",
          "normal ASR complete returns while release is still absent");
    Check(clock_ms == 100 && wait_returns == 0, "R-24 transport handoff lifetime is not hidden by waiting here");
    for (bool wrong_generation : {false, true}) {
        Reset();
        Schedule(100, [=] { Publish(TurnReleaseState::kTransportLost, wrong_generation ? 6 : 7,
                                   wrong_generation ? "req-current" : "req-old"); });
        Schedule(300, [] { Asr(Kind::kAsrComplete); });
        Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_OK && text == "recognized",
              "stale terminal does not fail current ASR");
        Check(clock_ms == 300 && disconnects == 0, "identity mismatch remains observation only");
    }
    Reset();
    Schedule(100, [] { wqn::AgentVoiceRequestCancel(); Publish(TurnReleaseState::kTransportLost); });
    Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_OK && text.empty(), "soft cancel takes priority");
    Check(clock_ms == 100 && wqn::AgentVoiceCancelRequested(), "cancel state remains available to caller");
    Reset();
    handoff = FinalHandoffResult::kDefinitelyNotSent;
    Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_ERR_INVALID_STATE && clock_ms == 0,
          "definitely-unsent FINAL still permits batch fallback");
    Reset();
    handoff = FinalHandoffResult::kAmbiguous;
    Schedule(100, [] { Publish(TurnReleaseState::kHandoffAmbiguous); });
    Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_ERR_INVALID_RESPONSE && clock_ms <= 200,
          "ambiguous FINAL with terminal never permits batch replay");
    Reset();
    handoff = FinalHandoffResult::kAmbiguous;
    Schedule(100, [] { Asr(Kind::kAsrComplete); });
    Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_OK, "ambiguous but live handoff can resolve as ASR");
    Reset();
    Schedule(100, [] { Asr(Kind::kAsrComplete, ""); });
    Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_ERR_INVALID_SIZE, "empty complete stays no-speech");
#if HAS_TERMINAL_QUERY
    Reset();
    Schedule(0, [] { Asr(Kind::kAsrComplete); });
    Check(wqn::AgentVoiceTurnFinalize(1000, 9, &text) == ESP_OK, "sub-tick timeout waits at least one RTOS tick");
    Reset();
    query_enter_hook = [] { Asr(Kind::kAsrComplete); Publish(TurnReleaseState::kReleased); };
    Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_OK && text == "recognized",
          "ASR published between flag check and terminal snapshot wins the race");
    Reset();
    query_enter_hook = [] { wqn::AgentVoiceRequestCancel(); Publish(TurnReleaseState::kTransportLost); };
    Check(wqn::AgentVoiceTurnFinalize(1000, 15000, &text) == ESP_OK && text.empty(),
          "cancel published during terminal observation wins the race");
#endif
    Reset();
    Check(wqn::AgentVoiceTurnFinalize(1000, 250, &text) == ESP_ERR_TIMEOUT, "live silent transport still reaches deadline");
    Check(clock_ms >= 250 && clock_ms <= 3270 && disconnects == 1, "timeout retains bounded release recovery");
#if HAS_TERMINAL_QUERY
    Reset();
    Publish(TurnReleaseState::kTransportLost);
    TurnWaitResult observed;
    Check(TryGetTurnTerminal(7, "req-current", &observed), "sticky terminal query matches");
    Check(observed.wait_status == TurnWaitStatus::kTerminalObserved && observed.turn_gen == 7 &&
          observed.release_state == TurnReleaseState::kTransportLost, "query copies a coherent terminal");
    Check(TryGetTurnTerminal(7, "req-current", &observed) && terminal_sem.available,
          "query neither consumes completion signal nor record");
    Check(!TryGetTurnTerminal(6, "req-current", &observed) && !TryGetTurnTerminal(7, "req-old", &observed) &&
          !TryGetTurnTerminal(7, std::string("req-current\0suffix", 18), &observed) &&
          !TryGetTurnTerminal(0, "req-current", &observed) && !TryGetTurnTerminal(7, "", &observed) &&
          !TryGetTurnTerminal(7, "req-current", nullptr), "query requires full identity and output");
    Check(observed.turn_gen == 7 && observed.release_state == TurnReleaseState::kTransportLost,
          "unsuccessful query preserves caller output");
    terminal_sem.available = false;
    observed = WaitForTurnRelease(7, "req-current", 20);
    Check(observed.wait_status == TurnWaitStatus::kTerminalObserved && clock_ms == 0 && disconnects == 0,
          "release wait sees ledger even after a signal was consumed");
    // Concurrent writer/reader use the actual publisher/query bodies. Host
    // critical-section ports use a mutex; this is not ESP32 interrupt timing.
    PublishTurnTerminalRecord(7, "req-current", TurnReleaseState::kReleased,
                              TurnStreamObservation::kTurnDoneObserved);
    std::atomic<bool> running{true}, coherent{true};
    std::atomic<size_t> reads{0};
    std::thread writer([&] {
        while (reads.load() == 0) std::this_thread::yield();
        for (int i = 0; i < 10000; ++i) {
            PublishTurnTerminalRecord(7, "req-current", TurnReleaseState::kReleased,
                                      TurnStreamObservation::kTurnDoneObserved);
            PublishTurnTerminalRecord(8, "req-new", TurnReleaseState::kAborted,
                                      TurnStreamObservation::kAborted);
        }
        running.store(false);
    });
    while (running.load() || reads.load() < 1000) {
        TurnWaitResult snapshot;
        if (TryGetTurnTerminal(7, "req-current", &snapshot) &&
            (snapshot.release_state != TurnReleaseState::kReleased ||
             snapshot.stream_observation != TurnStreamObservation::kTurnDoneObserved)) coherent.store(false);
        if (TryGetTurnTerminal(8, "req-new", &snapshot) &&
            (snapshot.release_state != TurnReleaseState::kAborted ||
             snapshot.stream_observation != TurnStreamObservation::kAborted)) coherent.store(false);
        reads.fetch_add(1);
    }
    writer.join();
    Check(coherent.load() && critical_depth == 0, "20,000 concurrent publications retain coherent identity and terminal fields");
#endif
    std::printf("%zu checks, %zu failures (real voice pipe + terminal ledger; virtual 100 Hz RTOS; NOT HIL)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
