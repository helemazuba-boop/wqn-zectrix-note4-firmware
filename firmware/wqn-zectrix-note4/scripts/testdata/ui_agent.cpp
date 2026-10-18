#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include "display_service.h"
#include "opencode_session.h"
#include "button_event.h"
#include "ui/ai_text_layout.h"
#include "ui/ai_scroll_geometry.h"
#include "ui/ai_native_text.h"
#include "ui/ui_layout.h"
#include "ui/assets/font_wqn_ui_16_1.h"

int checks = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); std::abort(); } ++checks; } while (0)
int64_t esp_timer_get_time() { return 1000000; }
struct TextRun { int x,y; const lv_font_t* font; std::string text; bool ink; };
std::vector<TextRun> drawn;

namespace wqn {
std::array<uint8_t,400*300> pixels;
extern "C" const lv_font_t SourceHanSansSC_Regular_slim;
extern "C" bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t*,lv_font_glyph_dsc_t*,uint32_t,uint32_t) { return false; }
extern "C" const uint8_t* lv_font_get_bitmap_fmt_txt(const lv_font_t*,uint32_t) { return nullptr; }
void DrawEpdPixel(int x,int y,bool ink) { if(x>=0&&x<400&&y>=0&&y<300) pixels[y*400+x]=ink; }
void ClearEpdFramebuffer(bool white) { pixels.fill(!white); drawn.clear(); }
const lv_font_t* PrimaryUiFont() { return &SourceHanSansSC_Regular_slim; }
@@FONT_LOOKUP@@
@@FONT_DRAW@@
@@FONT_MEASURE@@
void DrawTextWithFont(int x,int y,const lv_font_t* font,const char* text,bool ink) {
    drawn.push_back({x,y,font,text,ink});
    const char* cursor=text; uint32_t cp;
    while(*cursor&&DecodeUtf8(cursor,&cp)) {
        DrawGlyphFromFont(x,y,font,cp,ink); x+=MeasureGlyphWidthInFont(font,cp);
    }
}
int MeasureUtf8TextWidth(const char* text) { return MeasureTextWithFont(PrimaryUiFont(),text); }
const char* AiFeaturePhaseLabel(AiFeaturePhase) { return "空闲"; }
struct HomeSummary {};
struct StatusBarEditState { bool active=false; int selected=0; };
enum class UiScreen { kAi,kTodo };
enum class AiTier { kAgent };
struct UiState {
    UiScreen screen=UiScreen::kAi;
    struct { AiTier tier=AiTier::kAgent; } ai;
    AgentSessionState agent;
    struct { uint8_t focused=0; } agent_option;
    struct { int64_t last_agent_confirm_tap_ms=0; } gestures;
};
int sends=0, discards=0, approvals=0, denials=0, answers=0, interrupts=0, creates=0, locks=0, observes=0, moves=0;
int answer_index=-1;
AgentSessionState backend;
int ConfirmOpenCodePrompt(int64_t) { ++sends;return ESP_OK; }
void CancelOpenCodePrompt() { ++discards; }
int ReplyPendingOpenCodePermission(bool allow) { allow?++approvals:++denials;return ESP_OK; }
void InterruptOpenCodeRun() { ++interrupts; }
int ReplyPendingOpenCodeQuestion(int index) { ++answers;answer_index=index;return ESP_OK; }
int CreateNewOpenCodeSession(OpenCodeRejectReason*) { ++creates;return ESP_OK; }
int LockSelectedOpenCodeSession(OpenCodeRejectReason*) { ++locks;return ESP_OK; }
int ObserveOpenCodeSession(OpenCodeRejectReason*) { ++observes;return ESP_OK; }
int MoveOpenCodeSessionSelection(int) { ++moves;return ESP_OK; }
void* g_lock=reinterpret_cast<void*>(1);
constexpr int portMAX_DELAY=0;
void xSemaphoreTake(void*,int) {}
void xSemaphoreGive(void*) {}
AgentSessionState& g_state=backend;
int changes=0;
void MarkChangedLocked() { ++changes; }
@@SCROLL_OWNER@@
struct UiFrame {
    UiScreen screen=UiScreen::kAi;
    struct { AiTier tier=AiTier::kAgent; } ai;
    AgentSessionState agent;
    struct { int focused=0; } agent_option;
};
}

namespace device_ui_internal {
void FillRect(int x,int y,int width,int height,bool ink) {
    for(int yy=y;yy<y+height;++yy)for(int xx=x;xx<x+width;++xx)wqn::DrawEpdPixel(xx,yy,ink);
}
void DrawHorizontalLine(int x,int y,int width) { FillRect(x,y,width,1,true); }
void DrawRect(int x,int y,int width,int height) {
    FillRect(x,y,width,1,true);FillRect(x,y+height-1,width,1,true);
    FillRect(x,y,1,height,true);FillRect(x+width-1,y,1,height,true);
}
void DrawSelectableIcon(int x,int y,const WqnBitmapAsset& asset,bool focused,int) {
    if(focused)FillRect(x,y,asset.width,asset.height,true);
    for(int yy=0;yy<asset.height;++yy)for(int xx=0;xx<asset.width;++xx)
        if(asset.data[yy*asset.stride+xx/8]&(0x80>>(xx%8)))wqn::DrawEpdPixel(x+xx,y+yy,!focused);
}
void DrawWifiStatusIcon(int,int,const wqn::HomeSummary&) {}
void DrawDashedRect(int x,int y,int w,int h) { DrawRect(x,y,w,h); }
enum class AgentOption { kSend,kReinput,kApprove,kDeny,kCount };
enum class AgentOptionMode { kNone,kPermission,kQuestion,kConfirmSend };
@@BAR_CONSTANTS@@
@@DECISIONS@@
enum class RefreshSchedule { kNone,kAi };
void SyncAgentSnapshot(wqn::UiState*) {}
#define ESP_LOGI(...) do {} while(0)
#define ESP_LOGW(...) do {} while(0)
@@INPUT@@
@@PICKER@@
@@STATUS@@
std::string AgentSignature(const wqn::UiFrame& frame) {
    std::string signature;
@@SIGNATURE@@
    return signature;
}
}

void Save(const std::string& folder,const std::string& name) {
    if(folder.empty())return;
    std::ofstream out(folder+"/"+name+".pgm",std::ios::binary);
    out<<"P5\n400 300\n255\n";
    for(auto pixel:wqn::pixels)out.put(pixel?0:255);
}
void CheckBounds() {
    for(const auto& run:drawn) {
        CHECK(run.x>=0&&run.y>=0);
        CHECK(run.x+wqn::MeasureTextWithFont(run.font,run.text.c_str())<=400);
        CHECK(run.y+run.font->line_height<=300);
        CHECK(run.text.find('\n')==std::string::npos);
    }
}
int main(int argc,char**argv) {
    using namespace device_ui_internal;
    const std::string folder=argc>1?argv[1]:"";
    wqn::AgentSessionState agent;
    for(int i=0;i<23;++i) agent.sessions.push_back({"ses_"+std::to_string(i),
        "整理本周数学错题与函数学习记录 "+std::to_string(i),1792305600000LL,
        static_cast<wqn::OpenCodeSessionOutcome>(i%5)});
    for(size_t i=0;i<agent.sessions.size();++i) {
        agent.selected_session=i;
        agent.session_window_start=wqn::ClampAgentPickerWindow(agent.session_window_start,i,agent.sessions.size());
        wqn::ClearEpdFramebuffer();DrawAgentSessionPicker(agent);CheckBounds();
        CHECK(drawn.size()==15);
        CHECK(drawn[2].text==std::to_string(i+1)+"/23");
        CHECK(agent.session_window_start<=i&&i<agent.session_window_start+4);
        if(i==0)Save(folder,"picker-first");
        if(i==22)Save(folder,"picker-last");
    }
    CHECK(wqn::ClampAgentPickerWindow(8,10,23)==8);
    CHECK(wqn::ClampAgentPickerWindow(8,7,23)==7);
    CHECK(wqn::ClampAgentPickerWindow(20,0,23)==0);
    CHECK(wqn::ClampAgentPickerWindow(0,22,23)==19);
    CHECK(wqn::ClampAgentPickerWindow(20,99,2)==0);
    CHECK(wqn::ClampAgentPickerWindow(20,99,0)==0);
    auto missing=agent.sessions.front();missing.updated_at=0;
    CHECK(AgentSessionMetadata(missing)=="状态未知");
    missing.outcome=wqn::OpenCodeSessionOutcome::kSucceeded;
    CHECK(AgentSessionMetadata(missing)=="已完成");
    agent.current_session_title=std::string(160,'W')+"\n不能挤占状态";
    agent.ui.status_label="等待授权，当前任务需要你的允许";
    wqn::ClearEpdFramebuffer();DrawAgentStatusBar(agent,{},{});CheckBounds();
    CHECK(drawn.size()==2);
    CHECK(drawn[1].x+wqn::MeasureTextWithFont(drawn[1].font,drawn[1].text.c_str())+8<=drawn[0].x);
    Save(folder,"status-long");
    wqn::UiFrame frame;frame.agent=agent;
    auto before=AgentSignature(frame);
    frame.agent.sessions[0].outcome=wqn::OpenCodeSessionOutcome::kInterrupted;
    CHECK(AgentSignature(frame)!=before);
    before=AgentSignature(frame);frame.agent.sessions[0].updated_at++;
    CHECK(AgentSignature(frame)!=before);
    before=AgentSignature(frame);frame.agent.session_window_start++;
    CHECK(AgentSignature(frame)!=before);
    // Native pending title and body have disjoint vertical bounds.
    wqn::ClearEpdFramebuffer();DrawAgentPendingBubble("这是等待确认的输入，保持现有气泡布局。", "确认发送");CheckBounds();
    CHECK(drawn.size()>=2);CHECK(drawn[0].y+drawn[0].font->line_height<=drawn[1].y);
    Save(folder,"pending");
    agent.pending_question_id="question";agent.ui.phase=wqn::AiFeaturePhase::kAwaitingQuestion;
    for(int i=0;i<8;++i)agent.pending_question_options.push_back({"value"+std::to_string(i),"这是一个需要较长文字说明的回答选项"+std::to_string(i)});
    CHECK(AgentOptionModeFor(agent)==AgentOptionMode::kQuestion);
    for(int i=0;i<9;++i) {
        wqn::ClearEpdFramebuffer();DrawAgentOptionBar(AgentOptionMode::kQuestion,i,agent);CheckBounds();
        CHECK(drawn.back().text==std::to_string(i+1)+"/9");
        for(const auto& run:drawn)CHECK(run.x+wqn::MeasureTextWithFont(run.font,run.text.c_str())<=356);
        if(i==6)Save(folder,"question-options");
    }
    wqn::UiState state;state.agent=agent;state.agent.session_locked=true;
    auto event=[&](wqn::ButtonId button,wqn::ButtonEventType type,int64_t now=2000) {
        RefreshSchedule schedule=RefreshSchedule::kNone;
        auto result=TryApplyAgentAiButtonEvent({button,type},now,&state,&schedule);
        CHECK(result==AgentInputResult::kHandled);return schedule;
    };
    for(auto mode:{AgentOptionMode::kConfirmSend,AgentOptionMode::kPermission,AgentOptionMode::kQuestion}) {
        state.agent.ui.requires_confirmation=mode==AgentOptionMode::kConfirmSend;
        state.agent.ui.phase=mode==AgentOptionMode::kPermission?wqn::AiFeaturePhase::kAwaitingPermission:
            mode==AgentOptionMode::kQuestion?wqn::AiFeaturePhase::kAwaitingQuestion:wqn::AiFeaturePhase::kAwaitingConfirmation;
        state.agent.pending_permission_id="permission";
        for(auto type:{wqn::ButtonEventType::kPress,wqn::ButtonEventType::kHoldPress,wqn::ButtonEventType::kLongPress,wqn::ButtonEventType::kLongRelease,wqn::ButtonEventType::kRelease})
            CHECK(event(wqn::ButtonId::kConfirm,type)==RefreshSchedule::kNone);
        state.agent_option.focused=0;
        event(wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress);
        state.agent_option.focused=1;
        event(wqn::ButtonId::kConfirm,wqn::ButtonEventType::kDoublePress);
    }
    CHECK(wqn::sends==1&&wqn::discards==1&&wqn::approvals==1&&wqn::denials==1);
    CHECK(wqn::answers==2&&wqn::answer_index==1&&wqn::interrupts==0);
    state.agent_option.focused=8;event(wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress);
    CHECK(wqn::interrupts==1);
    state.agent.pending_question_options.clear();state.agent_option.focused=0;
    CHECK(AgentQuestionPrompt(state.agent).find("请选择自己回答后重新输入")==0);
    CHECK(AgentQuestionPrompt(state.agent).find("长按")==std::string::npos);
    event(wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress);CHECK(wqn::interrupts==2);
    state.agent.ui.phase=wqn::AiFeaturePhase::kIdle;state.agent.ui.requires_confirmation=false;state.agent.session_locked=false;
    event(wqn::ButtonId::kConfirm,wqn::ButtonEventType::kHoldPress);CHECK(wqn::creates==0);
    event(wqn::ButtonId::kConfirm,wqn::ButtonEventType::kLongRelease);CHECK(wqn::creates==1);
    event(wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress,3000);CHECK(wqn::locks==1);
    event(wqn::ButtonId::kConfirm,wqn::ButtonEventType::kShortPress,3700);CHECK(wqn::observes==1);
    event(wqn::ButtonId::kConfirm,wqn::ButtonEventType::kDoublePress);CHECK(wqn::observes==2);
    event(wqn::ButtonId::kDownPower,wqn::ButtonEventType::kShortPress);CHECK(wqn::moves==1);
    // Production backend owns relative targets and does not publish at edges.
    wqn::backend.ui.scroll_offset_lines=0;const int step=AiPageStepRows(273,18);CHECK(step==13);
    CHECK(wqn::ScrollOpenCodeResponse(1,-10,40,step));CHECK(wqn::backend.ui.scroll_offset_lines==13);
    CHECK(wqn::ScrollOpenCodeResponse(1,-10,40,step));CHECK(wqn::backend.ui.scroll_offset_lines==26);
    CHECK(wqn::ScrollOpenCodeResponse(1,-10,40,step));CHECK(wqn::ScrollOpenCodeResponse(1,-10,40,step));
    const auto changed=wqn::changes;CHECK(!wqn::ScrollOpenCodeResponse(1,-10,40,step));CHECK(wqn::changes==changed);
    for(int i=0;i<5;++i)wqn::ScrollOpenCodeResponse(-1,-10,40,step);
    CHECK(wqn::backend.ui.scroll_offset_lines==-10);CHECK(!wqn::ScrollOpenCodeResponse(-1,-10,40,step));
    std::printf("Agent production UI: %d assertions passed\n",checks);
}
