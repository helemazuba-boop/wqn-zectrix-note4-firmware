#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using TickType_t = uint32_t;
constexpr int portMAX_DELAY = -1;
namespace wqn {
struct UiFrame {
    bool prefer_full_refresh = false;
    int content = 0;
    std::array<uint8_t, 4240> frame_padding{};
    std::vector<std::string> lines;
};
namespace display {
using DisplayRevision = uint64_t;
constexpr DisplayRevision kInvalidDisplayRevision = 0;
constexpr uint32_t kDisplayResultQueueDepth = 16;
enum class WaveformRequirement { kPartial, kFull };
struct DisplayIntent {
    DisplayRevision revision = 0;
    WaveformRequirement waveform = WaveformRequirement::kPartial;
    uint32_t deadline_tick = 0;
    uint32_t reason_mask = 0;
};
struct DisplaySubmission {
    bool accepted = false;
    DisplayRevision revision = 0;
    WaveformRequirement waveform = WaveformRequirement::kPartial;
    uint32_t deadline_tick = 0;
};
struct DisplayResult { DisplayRevision old_revision, replacement; };
}
namespace runtime {
enum class SleepBlocker { kDisplay };
bool allow_lease = true;
struct SleepLease {
    bool acquired = false;
    explicit operator bool() const { return acquired; }
    static SleepLease TryAcquire(SleepBlocker, const char*, const char*, int) {
        return {allow_lease};
    }
};
}
int epd_activity = 0;
void NoteEpdActivity() { ++epd_activity; }
}
using wqn::display::DisplayIntent;
using wqn::display::DisplayRevision;
using wqn::display::WaveformRequirement;
enum class RefreshSchedule { kNone, kSelection, kImmediate };
struct SecondarySlot {
    bool pending = false;
    wqn::UiFrame frame;
    std::string signature;
    DisplayIntent intent;
    RefreshSchedule schedule = RefreshSchedule::kNone;
    TickType_t due_tick = 0;
};
void* g_refresh_mutex = reinterpret_cast<void*>(1);
void* g_refresh_task = reinterpret_cast<void*>(2);
void* g_display_result_queue = reinterpret_cast<void*>(3);
wqn::UiFrame g_pending_frames[2];
std::string g_pending_signatures[2];
DisplayIntent g_pending_intents[2];
SecondarySlot g_secondary;
std::atomic<int> g_consumer_index{0};
uint32_t g_outstanding_display_intents = 0;
bool g_refresh_busy = false, g_refresh_pending = false;
TickType_t g_refresh_due_tick = 0;
RefreshSchedule g_refresh_schedule = RefreshSchedule::kNone;
wqn::runtime::SleepLease g_display_sleep_lease;
int lock_depth = 0, notified = 0, yielded = 0;
bool duplicate = false, allow_publish = true, allow_track = true;
std::vector<wqn::display::DisplayResult> published;
constexpr char kTag[] = "test";

// Keep arguments type-checked/used, but do not infer ROM printf stack use.
template <class... Args> void TestLog(const char*, const char*, Args...) {}
#define ESP_LOGW(...) TestLog(__VA_ARGS__)
#define ESP_LOGE(...) TestLog(__VA_ARGS__)
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
TickType_t xTaskGetTickCount() { return 100; }
TickType_t RefreshDelay(RefreshSchedule) { return 20; }
const char* RefreshScheduleName(RefreshSchedule) { return "test"; }
void xSemaphoreTake(void*, int) { assert(lock_depth == 0); ++lock_depth; }
void xSemaphoreGive(void*) { assert(lock_depth == 1); --lock_depth; }
bool HasOutstandingRevisionLocked(DisplayRevision) { assert(lock_depth == 1); return duplicate; }
bool TrackOutstandingRevisionLocked(DisplayRevision) {
    assert(lock_depth == 1);
    if (allow_track) ++g_outstanding_display_intents;
    return allow_track;
}
void xTaskNotifyGive(void*) { assert(lock_depth == 0); ++notified; }
void taskYIELD() { ++yielded; }
WaveformRequirement StrongerWaveform(WaveformRequirement left, WaveformRequirement right) {
    return left == WaveformRequirement::kFull || right == WaveformRequirement::kFull
        ? WaveformRequirement::kFull : WaveformRequirement::kPartial;
}
TickType_t EarlierDeadline(TickType_t left, TickType_t right) { return left < right ? left : right; }
DisplayIntent NewDisplayIntent(DisplayRevision revision, RefreshSchedule,
                               TickType_t deadline, WaveformRequirement waveform) {
    return {revision, waveform, deadline, 2};
}
RefreshSchedule EffectiveSchedule(RefreshSchedule schedule, const DisplayIntent& intent) {
    return intent.waveform == WaveformRequirement::kFull ? RefreshSchedule::kImmediate : schedule;
}
wqn::display::DisplayResult SupersededResult(DisplayRevision old, DisplayRevision replacement) {
    return {old, replacement};
}
bool PublishDisplayResult(wqn::display::DisplayResult result, int) {
    assert(lock_depth == 1);
    if (allow_publish) published.push_back(result);
    return allow_publish;
}

@@PRODUCTION@@

int passed = 0;
void Check(bool condition) { assert(condition); ++passed; }
void Reset() {
    g_refresh_mutex = reinterpret_cast<void*>(1);
    g_refresh_task = reinterpret_cast<void*>(2);
    g_display_result_queue = reinterpret_cast<void*>(3);
    g_refresh_busy = g_refresh_pending = duplicate = false;
    allow_publish = allow_track = wqn::runtime::allow_lease = true;
    g_display_sleep_lease = {};
    g_outstanding_display_intents = 0;
    g_refresh_schedule = RefreshSchedule::kNone;
    g_refresh_due_tick = 0;
    g_consumer_index.store(0);
    g_pending_frames[0] = {}; g_pending_frames[1] = {};
    g_pending_frames[0].content = 101; g_pending_frames[1].content = 102;
    g_pending_signatures[0] = "old-primary"; g_pending_signatures[1] = "unused";
    g_pending_intents[0] = {7, WaveformRequirement::kFull, 90, 1};
    g_secondary = {};
    g_secondary.frame.content = 103;
    lock_depth = notified = yielded = wqn::epd_activity = 0;
    published.clear();
}
wqn::display::DisplaySubmission Submit(bool prefer_full = false) {
    wqn::UiFrame frame;
    frame.prefer_full_refresh = prefer_full;
    frame.content = 222;
    frame.lines = {"immutable current pixels", "latest card"};
    frame.frame_padding.back() = 0x42;
    const auto result = RequestEpdUiRefresh(frame, "new-signature", 8,
                                           RefreshSchedule::kSelection,
                                           WaveformRequirement::kPartial);
    Check(frame.prefer_full_refresh == prefer_full && frame.content == 222);
    Check(frame.lines.size() == 2 && frame.frame_padding.back() == 0x42);
    Check(lock_depth == 0);
    return result;
}
void Untouched() {
    Check(g_pending_frames[0].content == 101 && g_pending_frames[1].content == 102);
    Check(g_secondary.frame.content == 103 && g_consumer_index.load() == 0);
    Check(notified == 0 && yielded == 0 && wqn::epd_activity == 0);
}
int main() {
    Reset(); g_refresh_task = nullptr;
    Check(!Submit().accepted); Untouched();
    Reset(); duplicate = true;
    Check(!Submit().accepted); Untouched();
    Reset(); g_outstanding_display_intents = 16;
    Check(!Submit().accepted); Untouched();
    Reset(); wqn::runtime::allow_lease = false;
    Check(!Submit().accepted); Untouched();

    Reset(); auto result = Submit();
    Check(result.accepted && result.revision == 8 && result.deadline_tick == 120);
    Check(result.waveform == WaveformRequirement::kPartial);
    Check(g_pending_frames[0].content == 101 && g_pending_frames[1].content == 222);
    Check(g_pending_frames[1].lines.size() == 2 && g_pending_frames[1].frame_padding.back() == 0x42);
    Check(!g_pending_frames[1].prefer_full_refresh && g_pending_signatures[1] == "new-signature");
    Check(g_consumer_index.load() == 1 && g_refresh_pending);
    Check(notified == 1 && yielded == 1 && wqn::epd_activity == 1);
    Check(published.empty());

    Reset(); result = Submit(true);
    Check(result.accepted && g_pending_frames[1].prefer_full_refresh);
    Reset(); g_refresh_busy = true; result = Submit(true);
    Check(result.accepted && g_secondary.frame.prefer_full_refresh);

    Reset(); g_refresh_pending = true;
    result = Submit();
    Check(result.accepted && result.waveform == WaveformRequirement::kFull && result.deadline_tick == 90);
    Check(g_pending_frames[1].prefer_full_refresh && g_pending_intents[1].reason_mask == 3);
    Check(g_refresh_schedule == RefreshSchedule::kImmediate && g_refresh_due_tick == 90);
    Check(published.size() == 1 && published[0].old_revision == 7 && published[0].replacement == 8);
    Check(g_pending_frames[0].content == 101);

    Reset(); g_refresh_pending = true; allow_publish = false;
    Check(!Submit().accepted); Untouched();
    Check(g_refresh_pending && g_pending_signatures[0] == "old-primary");

    Reset(); g_refresh_busy = true;
    result = Submit();
    Check(result.accepted && g_secondary.pending && g_secondary.frame.content == 222);
    Check(g_secondary.frame.lines.size() == 2 && !g_secondary.frame.prefer_full_refresh);
    Check(g_pending_frames[0].content == 101 && g_consumer_index.load() == 0);
    Check(notified == 0 && yielded == 0 && wqn::epd_activity == 1);

    Reset(); g_refresh_busy = g_secondary.pending = true;
    g_secondary.intent = {6, WaveformRequirement::kFull, 80, 4};
    result = Submit();
    Check(result.accepted && result.waveform == WaveformRequirement::kFull && result.deadline_tick == 80);
    Check(g_secondary.frame.prefer_full_refresh && g_secondary.intent.reason_mask == 6);
    Check(g_secondary.signature == "new-signature" && g_secondary.due_tick == 80);
    Check(published.size() == 1 && published[0].old_revision == 6 && published[0].replacement == 8);

    Reset(); g_refresh_busy = g_secondary.pending = true; allow_publish = false;
    g_secondary.signature = "old-secondary";
    g_secondary.intent = {6, WaveformRequirement::kFull, 80, 4};
    Check(!Submit().accepted); Untouched();
    Check(g_secondary.pending && g_secondary.signature == "old-secondary");
    Check(g_secondary.intent.revision == 6 && g_secondary.intent.deadline_tick == 80);

    Reset(); allow_track = false;
    Check(!Submit().accepted && !g_refresh_pending && g_consumer_index.load() == 0);
    Check(lock_depth == 0 && notified == 0 && wqn::epd_activity == 0);
    Reset(); g_refresh_busy = true; allow_track = false;
    Check(!Submit().accepted && !g_secondary.pending);
    Check(lock_depth == 0 && notified == 0 && wqn::epd_activity == 0);

    std::printf("display submit production seams: %d PASS / 0 FAIL\n", passed);
}
