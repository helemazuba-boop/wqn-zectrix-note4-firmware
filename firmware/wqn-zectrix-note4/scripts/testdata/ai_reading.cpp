#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "display_service.h"
#include "ai_history.h"
#include "ai_interaction.h"
#include "opencode_session.h"
#include "ui/ai_native_text.h"
#include "ui/ai_scroll_geometry.h"
#include "ui/ai_text_layout.h"
#include "ui/ui_layout.h"
#include "ui/assets/font_wqn_inline_12_1.h"

int checks=0;
#define CHECK(c) do { if(!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort(); } ++checks; } while(0)
int64_t esp_timer_get_time() { return 2000000; }
struct TextRun { int x,y; const lv_font_t* font; std::string text; };
std::vector<TextRun> drawn;

namespace wqn {
std::array<uint8_t,400*300> pixels;
extern "C" const lv_font_t SourceHanSansSC_Regular_slim;
extern "C" bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t*,lv_font_glyph_dsc_t*,uint32_t,uint32_t) { return false; }
extern "C" const uint8_t* lv_font_get_bitmap_fmt_txt(const lv_font_t*,uint32_t) { return nullptr; }
void DrawEpdPixel(int x,int y,bool ink) { if(x>=0&&x<400&&y>=0&&y<300)pixels[y*400+x]=ink; }
void ClearEpdFramebuffer(bool white) { pixels.fill(!white);drawn.clear(); }
const lv_font_t* PrimaryUiFont() { return &SourceHanSansSC_Regular_slim; }
@@FONT_LOOKUP@@
@@FONT_DRAW@@
@@FONT_MEASURE@@
void DrawTextWithFont(int x,int y,const lv_font_t* font,const char* text,bool ink) {
    drawn.push_back({x,y,font,text});const char* cursor=text;uint32_t cp;
    while(*cursor&&DecodeUtf8(cursor,&cp)) { DrawGlyphFromFont(x,y,font,cp,ink);x+=MeasureGlyphWidthInFont(font,cp); }
}
int MeasureUtf8TextWidth(const char* text) {
    int width=0;uint32_t cp;const char* cursor=text;
    while(*cursor&&DecodeUtf8(cursor,&cp))width+=cp<128?6:MeasureGlyphWidthInFont(PrimaryUiFont(),cp);
    return width;
}
esp_err_t DrawUtf8Text(int x,int y,const char* text,bool ink) { DrawTextWithFont(x,y,PrimaryUiFont(),text,ink);return ESP_OK; }
@@AI_MODEL@@
enum class UiScreen { kAi,kHome };
struct UiState { UiScreen screen=UiScreen::kAi;AiSessionState ai;AgentSessionState agent; };
AgentSessionState g_state;
std::shared_ptr<const AiHistorySnapshot> follow_snapshots[3];
std::shared_ptr<const AiHistorySnapshot> GetAiHistorySnapshot(AiHistoryChannel channel) { return follow_snapshots[static_cast<int>(channel)]; }
void* g_lock=reinterpret_cast<void*>(1);
constexpr int portMAX_DELAY=0;
void xSemaphoreTake(void*,int) {}
void xSemaphoreGive(void*) {}
int follow_marks=0;
void MarkChangedLocked() { ++follow_marks; }
AiSessionState std_follow_state;
namespace std_owner {
AiSessionState& g_state=std_follow_state;
void MarkChanged() { ++follow_marks; }
@@STD_FOLLOW_OWNER@@
}
bool ApplyAiViewportFollow(uint32_t epoch,int32_t target,int32_t low,int32_t high,bool finished) { return std_owner::ApplyAiViewportFollow(epoch,target,low,high,finished); }
@@FOLLOW_OWNER@@
}

namespace device_ui_internal {
void FillRect(int x,int y,int w,int h,bool ink) { for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)wqn::DrawEpdPixel(xx,yy,ink); }
void DrawHorizontalLine(int x,int y,int w) { FillRect(x,y,w,1,true); }
void DrawVerticalLine(int x,int y,int h) { FillRect(x,y,1,h,true); }
void DrawRect(int x,int y,int w,int h) { DrawHorizontalLine(x,y,w);DrawHorizontalLine(x,y+h-1,w);DrawVerticalLine(x,y,h);DrawVerticalLine(x+w-1,y,h); }
void DrawRoundedRect(int x,int y,int w,int h,int) { DrawRect(x,y,w,h); }
void DrawHistoryClear(int y0,int y1) { FillRect(0,y0,400,y1-y0,false);drawn.clear(); }
void DrawWqnBitmapAsset(int x,int y,const WqnBitmapAsset& a,bool ink) {
    for(int yy=0;yy<a.height;++yy)for(int xx=0;xx<a.width;++xx)
        if(a.data[yy*a.stride+xx/8]&(0x80>>(xx%8)))wqn::DrawEpdPixel(x+xx,y+yy,ink);
}
esp_err_t DrawClippedText(int x,int y,int w,const std::string& text,bool ink=true) { AiNativeText(x,y,w,text,16,ink);return ESP_OK; }
esp_err_t DrawMarkdownLine(const MdLine&,int,int,int,int,const lv_font_t* font=nullptr);
@@HISTORY_MODEL@@
}
@@MD_RENDER@@
namespace device_ui_internal {
#define DRC(...) do { (void)wqn::DrawUtf8Text(__VA_ARGS__); } while(0)
@@PAGE@@
}

namespace device_ui_internal {
enum class AppEventKind { kAiViewportFollow };
enum class RefreshSchedule { kNone,kAi };
struct UiUpdate { bool changed; };
int AiBottomReserve(const wqn::AgentSessionState& agent) { return agent.ui.requires_confirmation || !agent.pending_permission_id.empty() || !agent.pending_question_id.empty() ? 22 : 0; }
struct UiRuntime { wqn::UiState state_;UiUpdate DispatchAiViewportFollow();UiUpdate FinishEvent(AppEventKind,RefreshSchedule,bool changed) {return {changed};} };
#define ESP_LOGI(...) do {} while(0)
@@FOLLOW_RUNTIME@@
}

void Save(const std::string& folder,const std::string& name) {
    if(folder.empty())return;
    std::ofstream out(folder+"/"+name+".pgm",std::ios::binary);
    out<<"P5\n400 300\n255\n";for(auto pixel:wqn::pixels)out.put(pixel?0:255);
}
bool HasText(const std::string& text) { return std::any_of(drawn.begin(),drawn.end(),[&](const auto& run){return run.text==text;}); }
void Bounds(int bottom=300) {
    for(const auto& run:drawn) {
        CHECK(run.x>=0&&run.x+wqn::MeasureTextWithFont(run.font,run.text.c_str())<=400);
        CHECK(run.y>=28&&run.y+run.font->line_height<=bottom);
    }
}
int main(int argc,char**argv) {
    using namespace device_ui_internal;
    const std::string folder=argc>1?argv[1]:"";
    for(int total=0;total<1300;total+=7)for(int anchor=0;anchor<=total;anchor+=31)for(int pad:{0,22}) {
        auto top=ComputeAiScrollGeometry(total,anchor,272-pad,18,INT32_MAX);
        auto tail=ComputeAiScrollGeometry(total,anchor,272-pad,18,INT32_MIN);
        CHECK(top.window_top==0);CHECK(tail.window_top==std::max(0,total-(272-pad)));
        CHECK(top.min_scroll<=top.max_scroll);
    }
    auto snapshot=std::make_shared<wqn::AiHistorySnapshot>();snapshot->revision=1;
    wqn::ChatMessageSnapshot user;user.id=1;user.kind=wqn::ChatMessageKind::kUser;user.text="Q";
    wqn::ChatMessageSnapshot reply;reply.id=2;reply.kind=wqn::ChatMessageKind::kAssistant;
    for(int i=1;i<=14;++i){if(i>1)reply.text+='\n';reply.text+="ROW_"+std::to_string(i);}
    snapshot->messages={user,reply};wqn::AiSessionState ai;ai.tier=wqn::AiTier::kAgent;
    int32_t minimum=0,maximum=0;GetAiScrollBounds(snapshot,true,0,&minimum,&maximum);
    CHECK(minimum==-1&&maximum==0);
    RenderAiHistoryViewport(ai,snapshot,minimum,0);CHECK(HasText("ROW_14"));Bounds();Save(folder,"tail-reachable");
    auto wide_tail=std::make_shared<wqn::AiHistorySnapshot>(*snapshot);wide_tail->revision++;
    std::string wide;
    while(wqn::MeasureTextWithFont(AiNativeFont(),(wide+"W").c_str())<=378)wide+='W';
    auto& wide_body=wide_tail->messages.back().text;
    wide_body.resize(wide_body.rfind('\n')+1);wide_body+=wide;
    GetAiScrollBounds(wide_tail,true,0,&minimum,&maximum);RenderAiHistoryViewport(ai,wide_tail,minimum,0);
    CHECK(HasText(wide));
    auto target=*std::find_if(drawn.begin(),drawn.end(),[&](const auto& run){return run.text==wide;});
    CHECK(target.y>=280);Save(folder,"tail-right-edge");
    const auto actual=wqn::pixels;
    wqn::ClearEpdFramebuffer();wqn::DrawTextWithFont(target.x,target.y,target.font,wide.c_str());
    int compared=0;
    for(int y=target.y;y<target.y+16;++y)for(int x=360;x<394;++x)if(wqn::pixels[y*400+x]){CHECK(actual[y*400+x]);++compared;}
    CHECK(compared>0);
    for(int pad:{0,22}) {
        GetAiScrollBounds(snapshot,true,pad,&minimum,&maximum);
        RenderAiHistoryViewport(ai,snapshot,minimum,pad);CHECK(HasText("ROW_14"));
        // Indicator counts live outside the interaction band's text viewport.
        for(const auto& run:drawn)if(run.text.rfind("ROW_",0)==0)CHECK(run.y+18<=300-pad);
    }
    const auto builds=GetAiTextCacheStats().builds;
    for(int i=0;i<40;++i){GetAiScrollBounds(snapshot,true,0,&minimum,&maximum);RenderAiHistoryViewport(ai,snapshot,minimum,0);}
    CHECK(GetAiTextCacheStats().builds==builds);
    auto before=GetAiHistoryLayout(snapshot,true);
    auto grown=std::make_shared<wqn::AiHistorySnapshot>(*snapshot);grown->revision++;
    grown->messages.back().text+="\nSTREAM_DELTA";
    auto after=GetAiHistoryLayout(grown,true);
    CHECK(before->text[0]==after->text[0]);CHECK(before->text[1]!=after->text[1]);
    auto prompt=std::make_shared<wqn::AiHistorySnapshot>();prompt->revision=2;
    user.id=20;user.text.clear();for(int i=1;i<=24;++i){if(i>1)user.text+='\n';user.text+="长提问第 "+std::to_string(i)+" 行";}
    prompt->messages={user};auto long_layout=GetAiHistoryLayout(prompt,true);
    CHECK(long_layout->text[0]->plain.size()==24);
    GetAiScrollBounds(prompt,true,0,&minimum,&maximum);RenderAiHistoryViewport(ai,prompt,minimum,0);
    CHECK(HasText("长提问第 24 行"));Bounds();Save(folder,"long-prompt-tail");
    RenderAiHistoryViewport(ai,prompt,maximum,0);CHECK(HasText("长提问第 1 行"));Bounds();Save(folder,"long-prompt-head");
    // Page in both directions at every block inset. Adjacent pages retain a
    // complete text row, and every row is visited before reaching either edge.
    auto pages=std::make_shared<wqn::AiHistorySnapshot>();pages->revision=100;
    reply.id=100;reply.text.clear();for(int i=0;i<100;++i)reply.text+="PAGE-"+std::to_string(i)+"\n";
    pages->messages={reply};
    for(int pad:{0,22}) {
        GetAiScrollBounds(pages,true,pad,&minimum,&maximum);
        const int step=AiPageStepRows(300-kAiViewportY-pad,kAiLineH);
        std::vector<std::string> previous,visited;
        for(int offset=maximum;;offset=std::max(minimum,offset-step)) {
            RenderAiHistoryViewport(ai,pages,offset,pad);
            std::vector<std::string> current;
            for(const auto& run:drawn)if(run.text.rfind("PAGE-",0)==0)current.push_back(run.text);
            if(!previous.empty())CHECK(std::any_of(current.begin(),current.end(),[&](const auto& row){
                return std::find(previous.begin(),previous.end(),row)!=previous.end();}));
            visited.insert(visited.end(),current.begin(),current.end());previous=std::move(current);
            if(offset==minimum)break;
        }
        for(int i=0;i<100;++i)CHECK(std::find(visited.begin(),visited.end(),"PAGE-"+std::to_string(i))!=visited.end());
    }
    // A STD collapse never changes Agent pixels or any Agent scroll bound.
    auto details=std::make_shared<wqn::AiHistorySnapshot>();details->revision=9000;
    auto thought=reply;thought.id=9000;thought.kind=wqn::ChatMessageKind::kThinking;
    thought.text="THINK-1\nTHINK-2\nTHINK-3\nTHINK-4";
    auto tool=reply;tool.id=9001;tool.kind=wqn::ChatMessageKind::kToolResult;
    tool.text.clear();tool.tool_name="inspect";tool.tool_args_json="file";tool.tool_result_json="done";
    details->messages={user,thought,tool};
    ai.expand_content=true;
    CHECK(wqn::AiHistoryContentExpanded(ai));
    GetAiScrollBounds(details,wqn::AiHistoryContentExpanded(ai),0,&minimum,&maximum);
    const auto expanded_layout=GetAiHistoryLayout(details,wqn::AiHistoryContentExpanded(ai));
    RenderAiHistoryViewport(ai,details,minimum,0);const auto agent_pixels=wqn::pixels;
    ai.expand_content=false;
    CHECK(wqn::AiHistoryContentExpanded(ai));
    CHECK(GetAiHistoryLayout(details,wqn::AiHistoryContentExpanded(ai))==expanded_layout);
    RenderAiHistoryViewport(ai,details,minimum,0);CHECK(wqn::pixels==agent_pixels);
    ai.tier=wqn::AiTier::kStd;
    CHECK(!wqn::AiHistoryContentExpanded(ai));
    const auto collapsed_layout=GetAiHistoryLayout(details,wqn::AiHistoryContentExpanded(ai));
    CHECK(collapsed_layout->total_content_h<expanded_layout->total_content_h);
    RenderAiHistoryViewport(ai,details,0,0);CHECK(wqn::pixels!=agent_pixels);
    ai.expand_content=true;CHECK(wqn::AiHistoryContentExpanded(ai));ai.tier=wqn::AiTier::kAgent;
    // Replay the production follow dispatcher and epoch-checked owner. The
    // same event sequence must land on the same final answer at any UI cadence.
    auto tick=[&](UiRuntime& runtime,const std::shared_ptr<wqn::AiHistorySnapshot>& history) {
        wqn::follow_snapshots[static_cast<int>(wqn::AiHistoryChannel::kAgent)]=history;
        runtime.DispatchAiViewportFollow();runtime.state_.agent=wqn::g_state;
    };
    auto thinking=std::make_shared<wqn::AiHistorySnapshot>();thinking->revision=10000;
    thought.id=10000;thought.text.clear();for(int i=0;i<30;++i)thought.text+="THINK-"+std::to_string(i)+"\n";
    thinking->messages={user,thought};
    auto preface=std::make_shared<wqn::AiHistorySnapshot>(*thinking);preface->revision++;
    reply.id=10001;reply.text="I will inspect it first.";preface->messages.push_back(reply);
    auto working=std::make_shared<wqn::AiHistorySnapshot>(*preface);working->revision++;
    tool.id=10002;tool.kind=wqn::ChatMessageKind::kToolStart;working->messages.push_back(tool);
    auto answered=std::make_shared<wqn::AiHistorySnapshot>(*working);answered->revision++;
    reply.id=10003;reply.text="Final answer";for(int i=0;i<25;++i)reply.text+="\nANSWER-"+std::to_string(i);
    answered->messages.push_back(reply);
    int32_t final_top=0;CHECK(GetAiNewestAnswerTopOffsetLines(answered,true,0,&final_top));
    for(bool sample_preface:{false,true}) {
        UiRuntime runtime;runtime.state_.ai.tier=wqn::AiTier::kAgent;
        wqn::g_state={};wqn::g_state.ui.phase=wqn::AiFeaturePhase::kRunning;
        wqn::follow_snapshots[2]=thinking;wqn::ArmAgentFollowLocked();runtime.state_.agent=wqn::g_state;
        tick(runtime,thinking);CHECK(runtime.state_.agent.follow_active);
        if(sample_preface){tick(runtime,preface);CHECK(runtime.state_.agent.follow_active);}
        tick(runtime,working);CHECK(runtime.state_.agent.follow_active);
        tick(runtime,answered);CHECK(runtime.state_.agent.ui.scroll_offset_lines==final_top);
        CHECK(runtime.state_.agent.follow_active);
        // More body text stays parked at its first line, then a later round
        // is followed without undoing a manual reading decision.
        wqn::g_state.ui.phase=wqn::AiFeaturePhase::kComplete;runtime.state_.agent=wqn::g_state;
        tick(runtime,answered);CHECK(!runtime.state_.agent.follow_active);
    }
    UiRuntime resumed;resumed.state_.ai.tier=wqn::AiTier::kAgent;
    wqn::g_state={};wqn::g_state.ui.phase=wqn::AiFeaturePhase::kRunning;
    wqn::follow_snapshots[2]=answered;wqn::ArmAgentFollowLocked();resumed.state_.agent=wqn::g_state;
    tick(resumed,answered);CHECK(resumed.state_.agent.follow_active);
    CHECK(resumed.state_.agent.ui.scroll_offset_lines!=final_top); // old answer is not the resumed body
    const auto stale_epoch=wqn::g_state.follow_epoch;
    wqn::ArmAgentFollowLocked();
    CHECK(!wqn::ApplyOpenCodeViewportFollow(stale_epoch,0,-100,100,true));
    const auto epoch=wqn::g_state.follow_epoch;
    wqn::SetOpenCodeFollowState(false,true);const auto manual_offset=wqn::g_state.ui.scroll_offset_lines;
    CHECK(!wqn::ApplyOpenCodeViewportFollow(epoch,0,-100,100,false));
    resumed.state_.agent=wqn::g_state;tick(resumed,working);
    CHECK(wqn::g_state.ui.scroll_offset_lines==manual_offset&&!wqn::g_state.follow_active&&wqn::g_state.user_moved);
    wqn::ArmAgentFollowLocked();wqn::g_state.ui.phase=wqn::AiFeaturePhase::kAwaitingQuestion;
    CHECK(!wqn::ApplyOpenCodeViewportFollow(wqn::g_state.follow_epoch,0,-100,100,false));
    resumed.state_.agent=wqn::g_state;CHECK(!resumed.DispatchAiViewportFollow().changed);

    // STD consumes the same sequence and geometry, with its own owner/epoch.
    for(bool sample_preface:{false,true}) {
        UiRuntime runtime;runtime.state_.ai.tier=wqn::AiTier::kStd;
        wqn::std_follow_state={};wqn::std_follow_state.status=wqn::AiSessionStatus::kStreaming;
        wqn::follow_snapshots[0]=thinking;wqn::std_owner::ArmAiFollowLocked();runtime.state_.ai=wqn::std_follow_state;
        auto std_tick=[&](const std::shared_ptr<wqn::AiHistorySnapshot>& history) {
            wqn::follow_snapshots[0]=history;runtime.DispatchAiViewportFollow();runtime.state_.ai=wqn::std_follow_state;
        };
        std_tick(thinking);CHECK(runtime.state_.ai.follow_active);
        if(sample_preface){std_tick(preface);CHECK(runtime.state_.ai.follow_active);}
        std_tick(working);std_tick(answered);
        CHECK(runtime.state_.ai.follow_active&&runtime.state_.ai.scroll_offset_lines==final_top);
        wqn::std_follow_state.status=wqn::AiSessionStatus::kReplyReady;runtime.state_.ai=wqn::std_follow_state;
        std_tick(answered);CHECK(!runtime.state_.ai.follow_active&&runtime.state_.ai.scroll_offset_lines==final_top);
        wqn::std_owner::ArmAiFollowLocked();const auto old_epoch=wqn::std_follow_state.follow_epoch;
        wqn::std_owner::SetAiFollowState(false,true);
        CHECK(!wqn::ApplyAiViewportFollow(old_epoch,0,-100,100,false));
        wqn::std_owner::ArmAiFollowLocked();wqn::std_follow_state.status=wqn::AiSessionStatus::kAwaitingDecision;
        CHECK(!wqn::ApplyAiViewportFollow(wqn::std_follow_state.follow_epoch,0,-100,100,false));
        wqn::std_follow_state.status=wqn::AiSessionStatus::kStreaming;wqn::std_follow_state.auto_follow=false;
        CHECK(!wqn::ApplyAiViewportFollow(wqn::std_follow_state.follow_epoch,0,-100,100,false));
    }

    const auto* font=AiNativeFont();
    auto english=WrapAiNativeText("alpha beta gamma",wqn::MeasureTextWithFont(font,"alpha beta"),font,10);
    CHECK(english.size()==2&&english[0]=="alpha beta"&&english[1]=="gamma");
    auto punct=WrapAiNativeText("中文标点，不能丢失。",64,font,10);
    std::string joined;for(const auto& line:punct)joined+=line;CHECK(joined=="中文标点，不能丢失。");
    auto crlf=WrapAiNativeText("first\r\nsecond",300,font,10);CHECK(crlf.size()==2&&crlf[0]=="first");
    auto decorated=LayoutMarkdown("~~WWW~~",378,0,font);
    CHECK(decorated[0].decorations[0].w==wqn::MeasureTextWithFont(font,"WWW"));
    CHECK(decorated[0].decorations[0].w!=wqn::MeasureUtf8TextWidth("WWW"));
    CHECK(LayoutMarkdown("~~WWW~~",378)[0].decorations[0].w==wqn::MeasureUtf8TextWidth("WWW"));
    CHECK(LayoutMarkdown("x*y + z_1",378,kMdNoSingleEmphasis)[0].text=="x*y + z_1");
    const std::string markdown="# 混排标题\n\n- 第一项 English words remain readable\n- 第二项\n\n> 引用与 `inline code`\n\n```cpp\nstd::string message = \"hello\";\n```\n\n| a long wide heading | b | c | d |\n|---|---|---|---|\n| very long English cell content | 中文 | 123 | end |";
    auto mixed=std::make_shared<wqn::AiHistorySnapshot>();mixed->revision=3;reply.id=21;reply.text=markdown;mixed->messages={reply};
    GetAiScrollBounds(mixed,true,0,&minimum,&maximum);
    for(int offset=minimum;offset<=maximum;++offset){RenderAiHistoryViewport(ai,mixed,offset,0);Bounds();}
    for(const auto& row:LayoutMarkdown(markdown,378,0,font)) {
        if(row.kind==MdLineKind::kTableRow)CHECK(row.table_width<=378);
        else CHECK(row.indent_px+wqn::MeasureTextWithFont(font,row.text.c_str())<=378);
    }
    RenderAiHistoryViewport(ai,mixed,minimum,0);Save(folder,"mixed-markdown-tail");
    std::string huge;for(int i=0;i<4100;++i)huge+="budget row\n";
    // Identical ids/text/fonts with different math rules must not share rows.
    const std::string formula="x*y*z + a_b_c";
    auto normal_math=GetAiTextLayout(98,formula,AiTextKind::kMarkdown,378,font);
    auto literal_math=GetAiTextLayout(98,formula,AiTextKind::kMarkdown,378,font,kAiTextMaxRows,kMdNoSingleEmphasis);
    CHECK(normal_math!=literal_math);
    CHECK(literal_math->markdown[0].text==formula);
    CHECK(normal_math->markdown[0].text!=formula);
    CHECK(GetAiTextLayout(98,formula,AiTextKind::kMarkdown,378,font,kAiTextMaxRows,kMdNoSingleEmphasis)==literal_math);
    CHECK(GetAiTextLayout(98,formula,AiTextKind::kMarkdown,378,font)==normal_math);
    auto limited=GetAiTextLayout(99,huge,AiTextKind::kMarkdown,378,font);
    CHECK(limited->truncated&&limited->markdown.size()<=4096);
    CHECK(limited->markdown.back().text.find("后续未显示")!=std::string::npos);
    bool cut=false;std::string exact;
    for(int i=0;i<4096;++i)exact+="row\n";
    auto full_budget=LayoutMarkdown(exact,378,0,font,&cut);
    CHECK(full_budget.size()==4096&&!cut);
    auto retained=GetAiTextLayout(100,"retained",AiTextKind::kPlain,378,font);
    std::atomic<bool> safe{true};std::vector<std::thread> workers;
    for(int thread=0;thread<6;++thread)workers.emplace_back([&,thread]{for(int i=0;i<180;++i){
        const std::string word="worker-"+std::to_string(thread)+"-"+std::to_string(i);
        auto result=GetAiTextLayout(thread,word,AiTextKind::kPlain,378,font);
        if(result->plain.size()!=1||result->plain.front()!=word)safe=false;
        if(i%18==0) {
            auto live=std::make_shared<wqn::AiHistorySnapshot>(*grown);
            live->revision+=thread*180+i+1;live->messages.back().text=word;
            auto view=GetAiHistoryLayout(live,true);
            if(view->text[1]->markdown.front().text!=word)safe=false;
        }
    }});
    for(auto& worker:workers)worker.join();
    CHECK(safe.load());CHECK(retained->plain.front()=="retained");
    auto stats=GetAiTextCacheStats();CHECK(stats.entries<=kAiTextCacheMaxEntries&&stats.bytes<=kAiTextCacheMaxBytes);
    std::printf("AI production reading: %d assertions passed; cache %zu entries / %zu bytes\n",checks,stats.entries,stats.bytes);
}
