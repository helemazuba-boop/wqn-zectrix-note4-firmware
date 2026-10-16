#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "font_fmt.h"
#include "font_zectrix.h"
#include "ui/home_model.h"
#include "ui/navigation_model.h"
#include "ai_feature.h"
#include "ui/assets/font_wqn_digits_48_1.h"

#define CONFIG_WQN_AI_ENABLE 1
#define CONFIG_WQN_AGENT_ENABLE 1
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, ESP_ERR_NOT_FOUND = 2;
#define ESP_RETURN_ON_ERROR(expr, ...) do { const auto error = (expr); if (error != ESP_OK) return error; } while (0)
int assertions = 0;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#value); std::abort(); } ++assertions; } while (0)
int64_t now_ms = 1000;
int64_t esp_timer_get_time() { return now_ms * 1000; }

namespace wqn {
constexpr int kEpdWidth = 400, kEpdHeight = 300;
std::array<uint8_t, 400*300> pixels;
void ClearEpdFramebuffer(bool white) { pixels.fill(!white); }
void DrawEpdPixel(int x, int y, bool black) {
    if (x >= 0 && x < 400 && y >= 0 && y < 300) pixels[y*400+x] = black;
}
extern "C" const lv_font_t SourceHanSansSC_Regular_slim;
extern "C" bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t*, lv_font_glyph_dsc_t*, uint32_t, uint32_t) { return false; }
extern "C" const uint8_t* lv_font_get_bitmap_fmt_txt(const lv_font_t*, uint32_t) { return nullptr; }
@@FONT_LOOKUP@@
@@FONT_DRAW@@
@@FONT_SIZE@@
@@FONT_MEASURE@@
@@ASCII@@
const lv_font_t* PrimaryUiFont() { return &SourceHanSansSC_Regular_slim; }
int DrawUtf8Text(int x, int y, const char* text, bool ink) {
    const char* cursor = text; uint32_t cp;
    while (*cursor && DecodeUtf8(cursor,&cp)) {
        if (cp >= 32 && cp <= 126) {
            for (int xx=0;xx<5;++xx) for(int yy=0;yy<7;++yy)
                if (kFont5x7[cp-32][xx] & (1<<yy)) DrawEpdPixel(x+xx,y+8+yy,ink);
            x += 6;
        } else { DrawGlyphFromFont(x,y,PrimaryUiFont(),cp,ink); x += MeasureGlyphWidthInFont(PrimaryUiFont(),cp); }
    }
    return ESP_OK;
}
void DrawTextWithFont(int x,int y,const lv_font_t* font,const char* text,bool black) {
    const char* cursor=text; uint32_t cp;
    while(*cursor && DecodeUtf8(cursor,&cp)) {
        DrawGlyphFromFont(x,y,font,cp,black); x+=MeasureGlyphWidthInFont(font,cp);
    }
}
enum class OpenCodeSessionOutcome { kUnknown, kRunning, kSucceeded, kInterrupted, kFailed };
struct OpenCodeSessionInfo { std::string id, title; int64_t updated_at=0; OpenCodeSessionOutcome outcome=OpenCodeSessionOutcome::kUnknown; };
using AgentSessionOption = OpenCodeSessionInfo;
struct OpenCodeResult {};
int ListOpenCodeSessions(const std::string&,std::vector<OpenCodeSessionInfo>*,OpenCodeResult*);
enum class OpenCodeRejectReason { kNone };
struct AgentSessionState {
    std::vector<AgentSessionOption> sessions;
    struct Home { AgentSessionOption session; int64_t checked_at_ms=0; bool loaded=false, failed=false; } home;
    AiFeatureUiState ui;
    std::string current_session_id, current_session_title, pending_permission_id, pending_question_id;
    bool stream_active=false, run_live=false;
};
@@SUMMARY@@
enum class AiTier { kFlash, kStd, kAgent };
struct UiState {
    UiScreen screen=UiScreen::kHome;
    HomeSummary home;
    AgentSessionState agent;
    struct { bool paired=true, wifi_connected=true; } status;
    struct { AiTier tier=AiTier::kStd; } ai;
    NavigationState navigation;
    size_t selected_home_task=0;
    std::string home_viewed_session_id;
    int64_t home_viewed_result_version=0;
};
struct UiFrame { HomeSummary home; size_t selected_home_task=0; };
enum class TimeTile { kPomodoro, kCountdown };
struct TimeAppState { TimeTile tile=TimeTile::kCountdown; };
bool NavigateUiToScreen(UiState* s, UiScreen target) { s->screen=target; return true; }
int open_result=ESP_OK, open_calls=0;
int home_queries=0;
int RequestOpenCodeHomeStatus() { ++home_queries; return ESP_OK; }
std::string opened_id, opened_title;
int OpenCodeHomeSession(const std::string& id, const std::string& title) {
    ++open_calls; opened_id=id; opened_title=title; return open_result;
}
}

namespace device_ui_internal {
enum class RefreshSchedule { kNone, kSelection, kCommit, kClock };
struct UiRect { int x,y,width,height; const char* name; };
@@LAYOUT@@
@@RECT@@
constexpr UiRect kPomodoroConfigRect={0,0,1,1,"p"}, kCountdownConfigRect={0,0,1,1,"c"};
std::string CurrentClockLabel() { return "14:32"; }
std::string CurrentDateLabel() { return "10月17日 星期六"; }
void FillRect(int x,int y,int w,int h,bool ink) { for(int yy=y;yy<y+h;++yy) for(int xx=x;xx<x+w;++xx) wqn::DrawEpdPixel(xx,yy,ink); }
void DrawHorizontalLine(int x,int y,int w) { FillRect(x,y,w,1,true); }
void ClearRect(const UiRect& r) { FillRect(r.x,r.y,r.width,r.height,false); }
int RefreshRegion(const UiRect&,RefreshSchedule) { return ESP_OK; }
int RefreshFrame(const wqn::UiFrame&,RefreshSchedule) { return ESP_OK; }
@@BATTERY@@
@@STATUS_ICONS@@
void DrawDigit48Run(int x,int y,const std::string& text) {
    bool first=true;
    for(char c:text) { const auto* a=WqnDigit48(c); if(!a) continue;
        if(!first) ++x;
        for(int yy=0;yy<a->height;++yy) for(int xx=0;xx<a->width;++xx)
            if(WqnBitmapPixel(*a,xx,yy)) wqn::DrawEpdPixel(x+xx,y+yy,true);
        x += a->width; first=false;
    }
}
@@PROJECT@@
struct UiUpdate { RefreshSchedule refresh; bool changed; };
enum class AppEventKind { kAgentListPoll };
struct UiRuntime {
    wqn::UiState state_;
    int64_t home_activity_at_ms_=1000, home_poll_next_ms_=0;
    UiUpdate FinishEvent(AppEventKind,RefreshSchedule refresh,bool changed) { return {refresh,changed}; }
    UiUpdate DispatchHomeStatusPoll(int64_t now_ms);
    void Activity(const wqn::ButtonEvent& event,int64_t event_time_ms) {
@@ACTIVITY@@
    }
};
@@RUNTIME_POLL@@
void SyncAgentSnapshot(wqn::UiState*) {}
int transitions=0;
RefreshSchedule FinishScreenNavigation(wqn::UiState*,wqn::UiScreen) { ++transitions; return RefreshSchedule::kCommit; }
RefreshSchedule HomeShortcut(const wqn::ButtonEvent& event,int64_t event_time_ms,wqn::UiState* state) {
@@HOME_INPUT@@
    return RefreshSchedule::kNone;
}
}
@@PAGE@@

namespace worker_seam {
using namespace wqn;
enum class WorkerCommand { kNone, kHomeStatus, kRunPrompt };
AgentSessionState g_state;
WorkerCommand g_command=WorkerCommand::kNone;
int g_lock=0;
constexpr int portMAX_DELAY=0;
int acquired=0, released=0, armed=0, published=0;
bool slot_free_on_publish=false;
struct Demand { void Reset() { ++released; } } g_connectivity_demand;
void xSemaphoreTake(int,int) {}
void xSemaphoreGive(int) {}
int transport=ESP_OK;
std::vector<OpenCodeSessionInfo> response;
int LoadToken(std::string*) { return transport; }
int AcquireNetwork() { return transport; }
int InitOpenCodeSession() { return ESP_OK; }
int AcquireAgentLeaseLocked() { ++acquired; return ESP_OK; }
bool ArmWorkerLocked(WorkerCommand cmd) { ++armed; g_command=cmd; return true; }
void ReleaseWorkOwnershipLocked() { ++released; }
void RefreshAgentRunLeaseLocked() {}
void MarkChangedLocked() { ++published; slot_free_on_publish=g_command==WorkerCommand::kNone; }
@@ATTACHED_ROW@@
int LockSessionAndUnlock(const AgentSessionOption& selected,OpenCodeRejectReason*) {
    RememberAttachedSessionLocked(selected);
    opened_id=selected.id; opened_title=selected.title; return ESP_OK;
}
@@QUERY@@
@@QUERY_REQUEST@@
@@OPEN@@
}
namespace wqn {
int ListOpenCodeSessions(const std::string&,std::vector<OpenCodeSessionInfo>* out,OpenCodeResult*) {
    *out=worker_seam::response; return worker_seam::transport;
}
}

using namespace wqn;
using namespace device_ui_internal;
void Save(const std::string& path) {
    std::ofstream file(path,std::ios::binary); file<<"P4\n400 300\n";
    for(int y=0;y<300;++y) for(int x=0;x<400;x+=8) {
        uint8_t b=0; for(int bit=0;bit<8;++bit) b|=pixels[y*400+x+bit]<<(7-bit);
        file.put(static_cast<char>(b));
    }
}
int main(int argc,char** argv) {
    CHECK(ShouldPollHomeAgent(0,0,0,true,false,true,true,false));
    CHECK(!ShouldPollHomeAgent(14999,0,15000,true,false,true,true,true));
    CHECK(ShouldPollHomeAgent(15000,0,15000,true,false,true,true,true));
    CHECK(!ShouldPollHomeAgent(30000,0,30000,true,false,true,true,true));
    CHECK(!ShouldPollHomeAgent(15000,0,15000,true,false,true,true,false));
    for(int gate=0;gate<4;++gate) CHECK(!ShouldPollHomeAgent(0,0,0,gate!=0,gate==1,gate!=2,gate!=3,true));
    int polls=0; int64_t due=0;
    for(int64_t t=0;t<6*3600000;t+=500) if(ShouldPollHomeAgent(t,0,due,true,false,true,true,true)) { ++polls; due=t+kHomeAgentPollMs; }
    CHECK(polls==2); // Six idle hours do not become six hours of network polls.
    CHECK(ShouldPollHomeAgent(3600000,3600000,0,true,false,true,true,true));
    CHECK(!ShouldPollHomeAgent(99,100,0,true,false,true,true,true));
    UiRuntime runtime;
    runtime.state_.agent.home.loaded=true;
    runtime.state_.agent.home.checked_at_ms=1000;
    runtime.state_.agent.home.session={"live","title",1,OpenCodeSessionOutcome::kRunning};
    for(int64_t t=1000;t<6*3600000;t+=500) runtime.DispatchHomeStatusPoll(t);
    CHECK(home_queries==2 && runtime.home_activity_at_ms_==1000);
    ButtonEvent activity; activity.type=ButtonEventType::kPress; activity.repeat=true;
    runtime.Activity(activity,3600000); CHECK(runtime.home_activity_at_ms_==1000);
    activity.repeat=false; runtime.Activity(activity,3600000);
    runtime.DispatchHomeStatusPoll(3600000); CHECK(home_queries==3);
    CHECK(runtime.home_activity_at_ms_==3600000);
    runtime.Activity(activity,1000); CHECK(runtime.home_activity_at_ms_==3600000);

    UiState s;
    s.agent.home.loaded=true; s.agent.home.checked_at_ms=1000;
    s.agent.home.session={"session-A","整理物理错题",100,OpenCodeSessionOutcome::kRunning};
    for(const auto outcome:{OpenCodeSessionOutcome::kRunning,OpenCodeSessionOutcome::kSucceeded,
                           OpenCodeSessionOutcome::kFailed,OpenCodeSessionOutcome::kInterrupted,OpenCodeSessionOutcome::kUnknown}) {
        s.agent.home.session.outcome=outcome; UpdateHomeAiSummary(&s,1000);
        CHECK((s.home.ai.status==HomeAiStatus::kComplete)==(outcome==OpenCodeSessionOutcome::kSucceeded));
    }
    s.agent.home.session.outcome=OpenCodeSessionOutcome::kSucceeded;
    UpdateHomeAiSummary(&s,46001); CHECK(s.home.ai.status==HomeAiStatus::kUnknown);
    s.status.wifi_connected=false; UpdateHomeAiSummary(&s,1000); CHECK(s.home.ai.status==HomeAiStatus::kUnknown);
    s.status.wifi_connected=true; s.agent.home.failed=true;
    UpdateHomeAiSummary(&s,1000); CHECK(s.home.ai.status==HomeAiStatus::kUnknown);
    s.agent.home.failed=false; s.home_viewed_session_id="session-A"; s.home_viewed_result_version=100;
    UpdateHomeAiSummary(&s,1000); CHECK(s.home.ai.badge=="已查看");
    s.agent.home.session.updated_at=101; UpdateHomeAiSummary(&s,1000); CHECK(s.home.ai.badge=="已完成");
    s.agent.current_session_id="new-current"; s.agent.current_session_title="new task";
    UpdateHomeAiSummary(&s,1000);
    CHECK(s.home.ai.status==HomeAiStatus::kUnknown && s.home.ai.session_id=="new-current");
    s.agent.stream_active=true; s.agent.current_session_id="session-B"; s.agent.pending_permission_id="ask";
    UpdateHomeAiSummary(&s,1000); CHECK(s.home.ai.status==HomeAiStatus::kPermission && s.home.ai.session_id=="session-B");
    s.status.paired=false; UpdateHomeAiSummary(&s,1000); CHECK(s.home.ai.target==UiScreen::kSettings);
    CHECK(MakeHomeAiCard(HomeAiStatus::kComplete,"id","title",1,true,false,false).target==UiScreen::kSettings);

    s={}; s.home.ai=MakeHomeAiCard(HomeAiStatus::kComplete,"card-A","卡片 A",100,true,true,false);
    s.agent.home.session.id="newer-worker-B";
    ButtonEvent event; event.button=ButtonId::kConfirm; event.type=ButtonEventType::kShortPress;
    CHECK(HomeShortcut(event,2000,&s)==RefreshSchedule::kCommit);
    CHECK(opened_id=="card-A" && s.screen==UiScreen::kAi && transitions==1);
    CHECK(s.home_viewed_session_id=="card-A" && s.home_viewed_result_version==100);
    CHECK((s.navigation.blocked_mask & (1U<<static_cast<unsigned>(ButtonId::kConfirm)))!=0);
    event.type=ButtonEventType::kRelease;
    CHECK(ReduceNavigationInput(&s.navigation,s.screen,event,2001,true)==NavigationAction::kConsumed);
    event.type=ButtonEventType::kPress;
    CHECK(ReduceNavigationInput(&s.navigation,s.screen,event,2100,true)==NavigationAction::kConsumed);
    event.type=ButtonEventType::kHoldPress;
    CHECK(ReduceNavigationInput(&s.navigation,s.screen,event,2300,true)==NavigationAction::kConsumed);
    s.screen=UiScreen::kHome; s.home_viewed_session_id.clear(); open_result=ESP_ERR_INVALID_STATE;
    event.type=ButtonEventType::kShortPress;
    CHECK(HomeShortcut(event,3000,&s)==RefreshSchedule::kSelection && s.screen==UiScreen::kHome);
    CHECK(s.home_viewed_session_id.empty());
    s.selected_home_task=1; s.home.study.target=UiScreen::kWord;
    CHECK(HomeShortcut(event,4000,&s)==RefreshSchedule::kCommit && s.screen==UiScreen::kWord);
    s.screen=UiScreen::kHome; event.button=ButtonId::kDownPower; event.type=ButtonEventType::kDoublePress;
    CHECK(HomeShortcut(event,4001,&s)==RefreshSchedule::kSelection && s.selected_home_task==0);

    using namespace worker_seam;
    g_command=WorkerCommand::kRunPrompt; const int leases=acquired;
    CHECK(worker_seam::RequestOpenCodeHomeStatus()==ESP_ERR_INVALID_STATE && acquired==leases);
    g_command=WorkerCommand::kNone; g_state.stream_active=true;
    CHECK(worker_seam::RequestOpenCodeHomeStatus()==ESP_ERR_INVALID_STATE && acquired==leases);
    g_state.stream_active=false; g_state.current_session_id="A"; g_state.ui.prompt_text="unsent";
    g_state.sessions={{"picker-X","unchanged",1,OpenCodeSessionOutcome::kUnknown}};
    CHECK(worker_seam::RequestOpenCodeHomeStatus()==ESP_OK && g_command==WorkerCommand::kHomeStatus);
    response={{"B","other",2,OpenCodeSessionOutcome::kSucceeded},{"A","task",3,OpenCodeSessionOutcome::kRunning}};
    LoadHomeStatus(); CHECK(g_state.home.session.id=="A" && slot_free_on_publish);
    CHECK(g_state.current_session_id=="A" && g_state.ui.prompt_text=="unsent" && !g_state.stream_active);
    CHECK(g_state.sessions.size()==1 && g_state.sessions[0].id=="picker-X");
    CHECK(released==1);
    transport=ESP_ERR_INVALID_STATE; LoadHomeStatus();
    CHECK(g_state.home.failed && g_state.home.session.id=="A" && released==2);
    CHECK(worker_seam::OpenCodeHomeSession("clicked-C","clicked title",nullptr)==ESP_OK && opened_id=="clicked-C");
    CHECK(g_state.sessions.size()==2 && g_state.sessions[1].id=="clicked-C");
    RememberAttachedSessionLocked({"clicked-C","latest",2,OpenCodeSessionOutcome::kRunning});
    CHECK(g_state.sessions.size()==2 && g_state.sessions[1].outcome==OpenCodeSessionOutcome::kRunning);
    CHECK(worker_seam::OpenCodeHomeSession("","",nullptr)==ESP_ERR_NOT_FOUND);

    UiFrame frame; frame.home.study.title="函数与导数"; frame.home.study.resumable=true;
    frame.home.wifi_connected=true; frame.home.wifi_rssi=-50; frame.home.battery_percent=75;
    frame.home.study.progress="8 / 20"; frame.home.study.progress_percent=40;
    frame.home.review_label="待复习 12"; frame.home.todo_label="待办 3";
    for(const auto status:{HomeAiStatus::kIdle,HomeAiStatus::kRunning,HomeAiStatus::kPermission,HomeAiStatus::kComplete,HomeAiStatus::kUnknown}) {
        frame.home.ai=MakeHomeAiCard(status,"A","整理物理错题",1,true,true,false);
        for(size_t focus=0;focus<2;++focus) {
            frame.selected_home_task=focus;
            CHECK(RenderHomeToEpd(frame,RefreshSchedule::kSelection)==ESP_OK);
            CHECK(pixels[124*400+7]==(focus==0));
            CHECK(pixels[232*400+7]==(focus==1));
            if(argc>1) Save(std::string(argv[1])+"/home-"+std::to_string(static_cast<int>(status))+"-"+std::to_string(focus)+".pbm");
        }
    }
    frame.home.ai.title=std::string(2000,'W')+"中文长标题";
    CHECK(RenderHomeToEpd(frame,RefreshSchedule::kSelection)==ESP_OK);
    CHECK(pixels[151*400+399]==0); // Sized text remains inside the content bounds.
    const auto before=pixels;
    wqn::DrawTextWithFontSized(1,1,1,nullptr,"x",24,false,true);
    CHECK(before==pixels);
    ClearEpdFramebuffer(true);
    DrawTextWithFontSized(10,10,100,PrimaryUiFont(),"学",16,false,true);
    const auto native=pixels;
    ClearEpdFramebuffer(true);
    DrawTextWithFontSized(10,10,100,PrimaryUiFont(),"学",12,false,true);
    bool strokes_preserved=true;
    for(int y=0;y<16;++y) for(int x=0;x<16;++x)
        if(native[(10+y)*400+10+x]) strokes_preserved &= pixels[(10+y*3/4)*400+10+x*3/4]!=0;
    CHECK(strokes_preserved);
    std::printf("PASS: Home status, shortcuts, bounded query, idle policy and production pixels (%d assertions; host seams, NOT HIL)\n",assertions);
}
