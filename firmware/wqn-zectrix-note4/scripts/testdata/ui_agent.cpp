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
#include "opencode_model.h"
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
const char* AiFeaturePhaseLabel(AiFeaturePhase) { return "空闲"; }
struct HomeSummary {};
struct StatusBarEditState { bool active=false; int selected=0; };
enum class UiScreen { kAi,kTodo };
enum class AiTier { kAgent };
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
    std::printf("Agent production UI: %d assertions passed\n",checks);
}
