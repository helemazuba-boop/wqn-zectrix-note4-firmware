#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "display/display_types.h"

using TickType_t=uint32_t;
constexpr int pdTRUE=1,pdFALSE=0,portMAX_DELAY=-1;
constexpr TickType_t kUiPollDelayTicks=25;
constexpr char kTag[]="force-test";
void HostLog(const char*,const char*,...) {}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
const char* esp_err_to_name(int) { return "host"; }
int64_t clock_ms=10000,g_last_active_us_local=0;
int64_t esp_timer_get_time() { return clock_ms*1000; }
int lock_depth=0;
void xSemaphoreTake(void*,int) { if (lock_depth++!=0) std::abort(); }
void xSemaphoreGive(void*) { if (--lock_depth!=0) std::abort(); }

namespace wqn {
enum class UiScreen { kHome,kAi,kNote };
enum class AiTier { kStd,kAgent,kFlash };
enum class AiSessionStatus { kIdle,kStreaming,kReplyReady,kError,kTranscribing,kListening };
enum class AiFeaturePhase { kIdle,kTranscribing };
struct AiState {
    AiSessionStatus status=AiSessionStatus::kReplyReady;
    AiTier tier=AiTier::kStd;
    std::string pending_text,status_detail;
    int64_t last_render_ms=42,status_since_ms=0;
    bool flash_is_streaming=false;
};
struct AppState {
    UiScreen screen=UiScreen::kAi;
    AiState ai;
    struct { bool stream_active=false; struct { AiFeaturePhase phase=AiFeaturePhase::kIdle; } ui; } agent;
    uint64_t revision=1;
    std::string pixels="same pixels";
};
struct UiFrame { UiScreen screen=UiScreen::kAi; AiState ai; bool prefer_full_refresh=false; std::string pixels; };
@@VIEW@@
bool g_force_full_refresh_next=false;
@@GLOBAL_FORCE@@
UiFrame RenderUiFrame(const AppState& state) { return {state.screen,state.ai,g_force_full_refresh_next,state.pixels}; }
}
namespace device_ui_internal {
@@SCHEDULE@@
@@EVENT@@
@@UPDATE@@
constexpr int64_t kAiStreamingRefreshFloorMs=500;
const char* AppEventKindName(AppEventKind) { return "host"; }
@@SCHEDULE_HELPERS@@
class UiRuntime {
public:
    wqn::AppState state_;
    uint64_t event_sequence_=0;
    uint64_t revision() const { return state_.revision; }
    uint64_t NextRevision();
    UiUpdate FinishEvent(AppEventKind,RefreshSchedule,bool,bool=false);
    UiUpdate DispatchAiStreamingSnapshot(const wqn::AiStreamingStatusView&);
    UiUpdate DispatchDisplayResult(const wqn::display::DisplayResult&);
};
@@RUNTIME@@
}
using device_ui_internal::RefreshSchedule;
using device_ui_internal::StrongerSchedule;
using wqn::display::WaveformRequirement;
using wqn::display::DisplayResult;
using wqn::display::DisplayStatus;
@@TRACKING_TYPES@@

// Producer ports: real snapshot/consume logic, semaphore and status-label I/O ports.
void* g_lock=reinterpret_cast<void*>(1);
wqn::AiState g_state;
bool g_streaming_active=false,g_streaming_force_full_render=false;
std::string g_pending_tool_label;
int64_t g_tool_clear_at_ms=0;
std::string RenderStreamingStatusLocked() { return g_state.pending_text; }
namespace wqn { @@PRODUCER@@ }

std::deque<DisplayResult> results;
void* g_display_result_queue=reinterpret_cast<void*>(2);
int acknowledgements=0,presented_records=0;
int xQueueReceive(void*,DisplayResult* output,int)
{
    if (results.empty()) return pdFALSE;
    *output=results.front(); results.pop_front(); return pdTRUE;
}
void AcknowledgeDisplayResult(uint64_t) { ++acknowledgements; }
void RecordPresentedRefresh(wqn::UiScreen,const char*,const std::string&) { ++presented_records; }
@@TRACKING@@
@@WAVEFORM@@

struct Request { wqn::UiFrame frame; std::string signature; uint64_t revision; RefreshSchedule schedule; WaveformRequirement waveform; };
bool accept_request=true;
std::vector<Request> requests;
std::string FrameSignature(const wqn::UiFrame& frame) { return frame.pixels; }
wqn::display::DisplaySubmission RequestEpdUiRefresh(const wqn::UiFrame& frame,const std::string& signature,
    uint64_t revision,RefreshSchedule schedule,WaveformRequirement waveform)
{
    requests.push_back({frame,signature,revision,schedule,waveform});
    return {accept_request,revision,waveform,0};
}
device_ui_internal::UiRuntime ui_runtime;
DisplayTrackingState display_tracking;
RefreshSchedule pending_refresh_schedule=RefreshSchedule::kNone;
std::string last_clock_label="12:00";
TickType_t poll_delay=0;
size_t checks=0,failures=0;
void Check(bool pass,const char* message)
{
    ++checks;
    if (!pass) { if (failures<20) std::fprintf(stderr,"FAIL %s\n",message); ++failures; }
}
bool Force(const device_ui_internal::UiUpdate& update)
{
#if HAS_UPDATE_FORCE
    return update.force_full_refresh;
#else
    (void)update; return false;
#endif
}
void Tick()
{
    auto& state=ui_runtime.state_;
    RefreshSchedule refresh_schedule=pending_refresh_schedule;
    pending_refresh_schedule=RefreshSchedule::kNone;
    DrainDisplayResults(&ui_runtime,&display_tracking,&refresh_schedule);
    wqn::AiStreamingStatusView streaming_view;
    const bool streaming_changed=wqn::CopyAiStreamingStatus(&streaming_view);
@@OWNER@@
    if (refresh_schedule!=RefreshSchedule::kNone) {
@@RENDER@@
    }
}
void Reset(wqn::AiTier tier=wqn::AiTier::kStd)
{
    ui_runtime.state_={}; ui_runtime.event_sequence_=0; ui_runtime.state_.ai.tier=tier;
    g_state={}; g_streaming_active=g_streaming_force_full_render=false;
    g_pending_tool_label.clear(); g_tool_clear_at_ms=0;
    display_tracking={}; display_tracking.presented_signature=ui_runtime.state_.pixels;
    pending_refresh_schedule=RefreshSchedule::kNone;
    results.clear(); requests.clear(); accept_request=true;
    wqn::g_force_full_refresh_next=false; acknowledgements=presented_records=0;
    poll_delay=0;
}
void ForceEvent() { g_streaming_force_full_render=true; }
void CheckFullRequest(const char* message)
{
    Check(!requests.empty() && requests.back().waveform==WaveformRequirement::kFull,message);
    Check(!requests.empty() && requests.back().revision==ui_runtime.revision(),"full request carries current reducer revision");
}
int main()
{
    Reset();
    wqn::AiStreamingStatusView view;
    ForceEvent();
    Check(wqn::CopyAiStreamingStatus(&view) && view.force_full_render && !g_streaming_force_full_render,
          "real producer transfers its one-shot force exactly once");
    const auto revision=ui_runtime.revision();
    const auto update=ui_runtime.DispatchAiStreamingSnapshot(view);
    Check(Force(update) && update.refresh==RefreshSchedule::kAi,"typed reducer update retains full policy separately from schedule");
    Check(update.revision_advanced && !update.state_changed && update.revision==revision+1,
          "pure waveform repair advances revision without inventing pixel-state changes");
    Check(wqn::CopyAiStreamingStatus(&view) && !view.force_full_render,"next snapshot does not replay consumed producer flag");
    Check(!Force(ui_runtime.DispatchAiStreamingSnapshot(view)),"ordinary next update does not inherit a force bit");

    Reset(); ForceEvent(); Tick();
    CheckFullRequest("identical presented pixels still produce a Full submission");
    Check(!requests.empty() && requests.back().frame.prefer_full_refresh,"AI force also reaches the final frame preference");
    Check(requests.size()==1 && !display_tracking.force_next_submission,"accepted tracked intent consumes local force once");
    Check(display_tracking.submitted_full_refresh,"real submission ledger records Full waveform");
    Tick(); Check(requests.size()==1,"quiet following tick does not duplicate accepted full intent");
    ForceEvent(); Tick();
    Check(requests.size()==2 && requests[1].revision>requests[0].revision,"a new forced event receives a fresh revision despite identical in-flight pixels");

    Reset(); accept_request=false; ForceEvent(); Tick();
    CheckFullRequest("rejected submission still requested the full waveform");
    Check(display_tracking.force_next_submission && pending_refresh_schedule!=RefreshSchedule::kNone,
          "rejection retains both waveform obligation and retry schedule");
    Check(!g_streaming_force_full_render,"retry does not depend on rearming producer one-shot");
    accept_request=true; Tick();
    Check(requests.size()==2,"unchanged next snapshot does not erase pending retry");
    CheckFullRequest("deferred full survives ordinary next update");
    Check(!display_tracking.force_next_submission,"successful tracked retry clears pending force");

    // Each stream's static throttle is first entered at 100ms, then allowed to
    // retry at 1000ms. The exact production sampling branches run unchanged.
    for (auto tier : {wqn::AiTier::kStd,wqn::AiTier::kAgent,wqn::AiTier::kFlash}) {
        Reset(tier); clock_ms=100;
        if (tier==wqn::AiTier::kStd) g_state.status=ui_runtime.state_.ai.status=wqn::AiSessionStatus::kStreaming;
        if (tier==wqn::AiTier::kAgent) ui_runtime.state_.agent.stream_active=true;
        if (tier==wqn::AiTier::kFlash) ui_runtime.state_.ai.flash_is_streaming=true;
        ForceEvent(); Tick();
        Check(requests.empty() && display_tracking.force_next_submission && pending_refresh_schedule!=RefreshSchedule::kNone,
              "stream throttle retains full intent without premature request");
        clock_ms=1000; Tick();
        Check(requests.size()==1,"throttled identical frame retries without another producer force");
        CheckFullRequest("stream throttle cannot weaken Full waveform");
        Check(!display_tracking.force_next_submission,"tracked throttle retry retires force");
    }
    clock_ms=10000;
    Reset(); accept_request=false; ForceEvent(); Tick();
    ui_runtime.state_.screen=wqn::UiScreen::kHome; ui_runtime.state_.pixels="latest home pixels";
    accept_request=true; Tick();
    CheckFullRequest("pending repair follows newest pixels when user navigates during deferral");
    Check(!requests.empty() && requests.back().frame.screen==wqn::UiScreen::kHome,"repair never resurrects stale AI pixels");

    Reset(); ui_runtime.state_.screen=wqn::UiScreen::kHome; ForceEvent(); Tick();
    Check(requests.empty() && !display_tracking.force_next_submission,"hidden AI force does not invent a refresh on another page");
    Reset(); g_state.last_render_ms=43; ui_runtime.state_.pixels="new ordinary pixels"; Tick();
    Check(requests.size()==1 && requests.back().waveform==WaveformRequirement::kAuto &&
          !requests.back().frame.prefer_full_refresh,"ordinary changed AI pixels preserve Auto waveform policy");
    ++g_state.last_render_ms; Tick();
    Check(requests.size()==1,"ordinary identical in-flight pixels remain deduplicated");

    Reset(); pending_refresh_schedule=RefreshSchedule::kImmediate; display_tracking.force_next_submission=true; Tick();
    CheckFullRequest("unrelated ordinary update cannot clear existing display-fault force");
    Reset(); pending_refresh_schedule=RefreshSchedule::kImmediate; wqn::RequestForceFullRefresh(); Tick();
    CheckFullRequest("legacy global force still bypasses identical-pixel deduplication");
    Check(!wqn::g_force_full_refresh_next,"legacy flag consumed at final render once");
    Reset(); pending_refresh_schedule=RefreshSchedule::kCommit; Tick();
    CheckFullRequest("commit refresh retains its existing forced Full policy");
    Reset(); pending_refresh_schedule=RefreshSchedule::kNavigation; ForceEvent(); Tick();
    CheckFullRequest("schedule coalescing cannot weaken explicit full-waveform repair");
    Check(!requests.empty() && requests.back().schedule==RefreshSchedule::kCoalesced,
          "full waveform repair does not repurpose the drawing schedule");

    Reset(); ForceEvent(); Tick();
    const auto accepted=display_tracking.submitted_revision;
    if (accepted!=wqn::display::kInvalidDisplayRevision) {
        results.push_back({accepted,DisplayStatus::kPresented,accepted,0,ESP_OK});
        Tick();
        Check(acknowledgements==1 && presented_records==1 && display_tracking.presented_revision==accepted,
              "physical Presented receipt advances the real ledger and is acknowledged");
        Check(requests.size()==1 && !display_tracking.force_next_submission,
              "Presented receipt does not manufacture another full refresh");
    } else { Check(false,"forced intent is accepted before presentation"); Check(false,"presentation control has a valid record"); }
    Reset(); ForceEvent(); Tick();
    const auto failed=display_tracking.submitted_revision;
    if (failed!=wqn::display::kInvalidDisplayRevision) {
        results.push_back({failed,DisplayStatus::kFailed,0,0,ESP_FAIL});
        Tick(); CheckFullRequest("Failed result retains existing full-waveform recovery");
        Check(requests.size()==2 && acknowledgements==1,"failure recovery replaces intent after matching result ACK");
    } else { Check(false,"failed intent has a record"); Check(false,"failure path is reachable"); Check(false,"failure ACK is reachable"); }
    Check(lock_depth==0,"snapshot producer releases its mutex");
    std::printf("AI force-full dispatch: %zu checks, %zu failures (real snapshot/reducer/final dispatch; render/RTOS/panel ports; NOT HIL)\n",checks,failures);
    return failures==0 ? 0 : 1;
}
