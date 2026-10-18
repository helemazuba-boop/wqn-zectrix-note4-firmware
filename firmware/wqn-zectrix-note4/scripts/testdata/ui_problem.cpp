#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>
#include "display_service.h"
#include "font_zectrix.h"
#include "note_list_model.h"
#include "ui/ai_native_text.h"
#include "ui/ai_text_layout.h"
#include "ui/note_reading_geometry.h"
constexpr int ESP_FAIL=-1,ESP_ERR_INVALID_ARG=-2,ESP_ERR_NO_MEM=-3,ESP_ERR_NOT_FOUND=-4;
int64_t esp_timer_get_time() { return 1000000; }
#define ESP_ERROR_CHECK_WITHOUT_ABORT(expr) ((void)(expr))
#define ESP_RETURN_ON_ERROR(expr,...) do { const int r=(expr);if(r!=ESP_OK)return r; } while(0)
void Log(const char*,const char*,...) {}
const char* esp_err_to_name(int) { return "host-result"; }
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
int checks=0;
#define CHECK(c) do { if(!(c)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort();}++checks; } while(0)
struct TextRun { int x,y;const lv_font_t* font;std::string text;bool ink; };
std::vector<TextRun> drawn;
namespace wqn::protocol::problem_study_v1 {
@@PROTOCOL@@
}
namespace wqn {
template<class T>using NotePsramAllocator=std::allocator<T>;
enum class ImageRenderMode : uint8_t { kBlackWhite,kGray16 };
constexpr size_t kNoteImageHeaderBytes=20,kNoteImageGray4PayloadBytes=60000;
@@PACK@@
@@STORE@@
@@MODEL@@
std::string ProblemAppStatusLine(const ProblemAppState&);
std::string ProblemAppNotice(const ProblemAppState&);
int InitProblemApp(ProblemAppState* state) { state->initialized=true;return ESP_OK; }
std::unordered_map<std::string,WqnProblemEntry> records;
int reads=0;
int ReadProblemPackEntry(const ProblemPackIndexEntry& entry,WqnProblemEntry* note) {
    ++reads;const auto found=records.find(entry.problem_id);
    if(found==records.end())return ESP_FAIL;
    *note=found->second;return ESP_OK;
}
struct HomeSummary { bool charging=false,full=false,wifi_connected=true;int battery_percent=76,wifi_rssi=-50; };
struct UiFrame { ProblemAppSnapshot problem_app;HomeSummary home; };
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
int MeasureUtf8TextWidth(const char* text) { return MeasureTextWithFont(PrimaryUiFont(),text); }
void DrawTextWithFont(int x,int y,const lv_font_t* font,const char* text,bool ink) {
    drawn.push_back({x,y,font,text,ink});const char* cursor=text;uint32_t cp;
    while(*cursor&&DecodeUtf8(cursor,&cp)){DrawGlyphFromFont(x,y,font,cp,ink);x+=MeasureGlyphWidthInFont(font,cp);}
}
int bw_blits=0;
void BlitEpdFramebuffer(const uint8_t*,size_t) { ++bw_blits; }
}
namespace {
@@APP_LOCAL@@
}
namespace wqn {
@@APP@@
}
namespace device_ui_internal {
@@SCHEDULE@@
void FillRect(int x,int y,int w,int h,bool ink) { for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)wqn::DrawEpdPixel(xx,yy,ink); }
void DrawHorizontalLine(int x,int y,int w) { FillRect(x,y,w,1,true); }
void DrawVerticalLine(int x,int y,int h) { FillRect(x,y,1,h,true); }
void DrawRect(int x,int y,int w,int h) { DrawHorizontalLine(x,y,w);DrawHorizontalLine(x,y+h-1,w);DrawVerticalLine(x,y,h);DrawVerticalLine(x+w-1,y,h); }
esp_err_t DrawClippedText(int x,int y,int w,const std::string& text,bool ink=true) { AiNativeText(x,y,w,text,16,ink);return ESP_OK; }
std::string CurrentClockLabel() { return "14:32"; }
@@BATTERY@@
@@ICONS@@
int gray_blits=0;
int RefreshNavigationGrayImage(const wqn::UiFrame&,const uint8_t*,size_t) { ++gray_blits;return ESP_OK; }
int RefreshFrame(const wqn::UiFrame&,RefreshSchedule) { return ESP_OK; }
}
@@MD@@
@@PAGE@@
std::string Signature(const wqn::UiFrame& frame) {
    std::string signature; {
@@SIGNATURE@@
    return signature;
}
device_ui_internal::RefreshSchedule Input(wqn::ProblemAppState& problem,wqn::ProblemInput input) {
    using device_ui_internal::RefreshSchedule;
    struct { wqn::ProblemAppState& problem_app; } current{problem};auto* state=&current;
    const auto old= wqn::ProblemAppSignature(problem);
    const bool old_problem_active=problem.active;
    const auto old_problem_mode=problem.mode;
    const auto old_problem_segment=problem.ring_segment;
@@OLD_WINDOW@@
    CHECK(wqn::HandleProblemAppInput(&problem,input)==ESP_OK);
    if(wqn::ProblemAppSignature(problem)!=old) {
@@POLICY@@
    return RefreshSchedule::kNone;
}
bool HasText(const std::string& text) { return std::any_of(drawn.begin(),drawn.end(),[&](const auto& run){return run.text==text;}); }
void Bounds() {
    for(const auto& run:drawn) {
        CHECK(run.x>=0&&run.y>=0);CHECK(run.x+wqn::MeasureTextWithFont(run.font,run.text.c_str())<=400);
        CHECK(run.y+run.font->line_height<=296);CHECK(run.text.find('\n')==std::string::npos);
    }
    CHECK(std::all_of(wqn::pixels.begin()+296*400,wqn::pixels.end(),[](auto pixel){return pixel==0;}));
}
void Paint(const wqn::ProblemAppState& state) {
    wqn::UiFrame frame;frame.problem_app=wqn::BuildProblemAppSnapshot(state);
    CHECK(device_ui_internal::RenderProblemBrowseToEpd(frame,device_ui_internal::RefreshSchedule::kSelection)==ESP_OK);
    Bounds();
}
void Save(const std::string& folder,const std::string& name) {
    if(folder.empty())return;
    std::ofstream out(folder+"/"+name+".pgm",std::ios::binary);out<<"P5\n400 300\n255\n";
    for(auto pixel:wqn::pixels)out.put(pixel?0:255);
}
wqn::ProblemAppState MakeList(int count=12) {
    using namespace wqn;
    ProblemAppState state;state.initialized=true;state.active=true;state.mode=ProblemAppMode::kProblemList;
    ProblemPackSet set;set.set_id="11111111-1111-4111-8111-111111111111";set.name="数学错题本";set.has_pack=true;set.entry_count=count;
    state.pack_index.mounted=true;state.pack_index.has_manifest=true;state.pack_index.sets.push_back(set);state.set_id=set.set_id;state.set_name=set.name;
    for(int i=0;i<count;++i) {
        ProblemPackIndexEntry entry={};std::snprintf(entry.problem_id,sizeof(entry.problem_id),"00000000-0000-4000-8000-%012d",i);std::strcpy(entry.set_id,set.set_id.c_str());
        std::snprintf(entry.title,sizeof(entry.title),"函数、数列与解析几何综合题 %d",i);entry.status=i%3;
        state.pack_index.entries.push_back(entry);
        WqnProblemEntry record;record.problem_id=entry.problem_id;record.title=entry.title;
        record.content_text="求函数的定义域，并证明它的单调性。\nx*y*z + a_b_c";
        WqnProblemPackPart part;part.index=1;part.label="函数性质";part.type="single_choice";part.full_marks=5;part.content_text="选择正确的结论";part.answer_text="B";part.choices={{"A","递增"},{"B","递减"}};record.parts.push_back(part);
        records[record.problem_id]=record;
    }
    return state;
}
void TestList(const std::string& folder) {
    using namespace wqn;using device_ui_internal::RefreshSchedule;
    auto state=MakeList();Paint(state);CHECK(HasText("12 道错题"));CHECK(HasText("1 / 12"));CHECK(HasText("还错着"));Save(folder,"list-head");
    for(size_t i=1;i<12;++i) {
        CHECK(Input(state,ProblemInput::kDown)==RefreshSchedule::kSelection);CHECK(state.list_selected==i);CHECK(state.list_window_start==(i<4?0:i-3));
        Paint(state);CHECK(HasText(std::to_string(i+1)+" / 12"));
    }
    Save(folder,"list-tail");
    for(int n=0;n<5;++n)CHECK(Input(state,ProblemInput::kDown)==RefreshSchedule::kNone);
    for(int n=10;n>=0;--n){CHECK(Input(state,ProblemInput::kUp)==RefreshSchedule::kSelection);Paint(state);}
    for(int n=0;n<5;++n)CHECK(Input(state,ProblemInput::kUp)==RefreshSchedule::kNone);
    state.list_selected=99;state.list_window_start=99;state.pack_index.sets[0].entry_count=2;
    Paint(state);CHECK(HasText("2 / 2")); // Renderer remains bounded while a shorter index arrives.
    auto empty=MakeList(0);Paint(empty);CHECK(HasText("该错题本暂无题目"));Save(folder,"list-empty");
}
void TestText(const std::string& folder) {
    using namespace wqn;using namespace device_ui_internal;
    auto state=MakeList();
    auto& first=records[state.pack_index.entries[0].problem_id];first.content_text.clear();first.parts.clear();
    for(int i=0;i<100;++i)first.content_text+="ROW-"+std::to_string(i)+"\n";
    CHECK(Input(state,ProblemInput::kConfirm)==RefreshSchedule::kCommit);CHECK(state.body_layout);CHECK(state.body_layout->markdown.size()==100);
    auto held=BuildProblemAppSnapshot(state);CHECK(held.body_layout==state.body_layout);
    std::string long_answer;for(int i=0;i<100;++i){if(i)long_answer+='\n';long_answer+="ANS-"+std::to_string(i);}
    WqnProblemPackPart answer_part;answer_part.index=1;answer_part.answer_text=long_answer;first.parts.push_back(answer_part);
    LoadCurrentProblem(&state);CHECK(state.answer_layout->markdown.size()>100);
    Paint(state);Save(folder,"body-head");const auto builds=GetAiTextCacheStats().builds;
    for(bool notice:{false,true}) {
        state.message.clear();state.commit_state=notice?ProblemVerdictCommitState::kFailed:ProblemVerdictCommitState::kIdle;state.verdict_error=notice?"记录空间已满":"";
        auto geometry=ProblemFaceGeometry(state,false);CHECK(geometry.visible==(notice?12u:13u));
        for(uint32_t offset=0;offset<=geometry.max_scroll;++offset) {
            state.body_scroll_lines=offset;Paint(state);CHECK(HasText("ROW-"+std::to_string(offset)));
            CHECK(HasText("ROW-"+std::to_string(offset+geometry.visible-1)));
            for(const auto& run:drawn)if(run.text.rfind("ROW-",0)==0){CHECK(run.y>=geometry.top);CHECK(run.y+20<=296);}
        }
        Save(folder,notice?"body-notice":"body-tail");
        state.body_scroll_lines=0;
        // Preserve the existing ring only when stepping past a face boundary.
        while(state.body_scroll_lines<geometry.max_scroll) {
            const auto before=state.body_scroll_lines;Input(state,ProblemInput::kDown);
            CHECK(state.body_scroll_lines-before<geometry.visible);CHECK(state.body_scroll_lines>before);
        }
    }
    state.answer_unlocked=true;state.ring_segment=AnswerSegment(state);
    for(bool notice:{false,true}) {
        state.message.clear();state.commit_state=notice?ProblemVerdictCommitState::kFailed:ProblemVerdictCommitState::kIdle;
        state.verdict_error=notice?"记录空间已满":"";
        const auto geometry=ProblemFaceGeometry(state,true);
        for(uint32_t offset=0;offset<=geometry.max_scroll;++offset) {
            state.answer_scroll_lines=offset;Paint(state);
            CHECK(HasText(state.answer_layout->markdown[offset].text));
            CHECK(HasText(state.answer_layout->markdown[offset+geometry.visible-1].text));
        }
        CHECK(HasText("ANS-99"));Save(folder,notice?"answer-notice-tail":"answer-tail");
        state.answer_scroll_lines=0;
        while(state.answer_scroll_lines<geometry.max_scroll) {
            const auto before=state.answer_scroll_lines;Input(state,ProblemInput::kDown);
            CHECK(state.answer_scroll_lines-before<geometry.visible);CHECK(state.answer_scroll_lines>before);
        }
    }
    CHECK(GetAiTextCacheStats().builds==builds);
    first.content_text.replace(first.content_text.find("ROW-99"),6,"ALT-99");
    state.message.clear();state.commit_state=ProblemVerdictCommitState::kIdle;state.verdict_error.clear();LoadCurrentProblem(&state);CHECK(state.body_layout!=held.body_layout);CHECK(held.body_layout->markdown.back().text=="ROW-99");
    UiFrame before,after;before.problem_app=held;after.problem_app=BuildProblemAppSnapshot(state);CHECK(Signature(before)!=Signature(after));
    Input(state,ProblemInput::kLongConfirm);CHECK(!state.body_layout);CHECK(!state.answer_layout);CHECK(held.body_layout->markdown.size()==100);
}
void TestVerdict(const std::string& folder) {
    using namespace wqn;using namespace device_ui_internal;
    auto state=MakeList();Input(state,ProblemInput::kConfirm);CHECK(!state.answer_unlocked);
    Paint(state);CHECK(HasText("x*y*z + a_b_c"));Save(folder,"math-body");
    Input(state,ProblemInput::kConfirm);CHECK(state.answer_unlocked);CHECK(SegmentFace(state,state.ring_segment)==ProblemFace::kAnswer);
    Paint(state);CHECK(HasText("答案：B. 递减"));Save(folder,"answer");
    Input(state,ProblemInput::kConfirm);CHECK(state.mode==ProblemAppMode::kVerdict);CHECK(state.verdict_selected==0);
    for(int i=0;i<4;++i){Paint(state);
        const std::array<std::string,4> labels={"A  对了","B  还要想想","C  错了","D  跳过"};
        for(int option=0;option<4;++option) {
            const auto found=std::find_if(drawn.begin(),drawn.end(),[&](const auto& run){return run.text==labels[option];});
            CHECK(found!=drawn.end());CHECK(found->font==AiNativeFont());CHECK(found->ink==(option!=i));
            CHECK(pixels[(kProblemVerdictTop+option*kProblemVerdictPitch+2)*400+kNoteListX+2]==(option==i));
        }
        CHECK(HasText("本题自评"));CHECK(HasText("A  对了"));CHECK(HasText("B  还要想想"));CHECK(HasText("C  错了"));CHECK(HasText("D  跳过"));Save(folder,"verdict-"+std::to_string(i));CHECK(Input(state,ProblemInput::kDown)==RefreshSchedule::kSelection);}
    CHECK(state.verdict_selected==0);Input(state,ProblemInput::kLongConfirm);CHECK(!state.verdict_effect_ready);CHECK(state.mode==ProblemAppMode::kProblemView);
    Input(state,ProblemInput::kConfirm);Input(state,ProblemInput::kDown);Input(state,ProblemInput::kConfirm);
    CHECK(state.commit_state==ProblemVerdictCommitState::kPersisting);CHECK(state.list_selected==0);Paint(state);CHECK(HasText("正在保存..."));Save(folder,"saving");
    DurableProblemObservation effect;CHECK(TakeProblemVerdictEffect(&state,"request-1","2026-10-19T00:00:00Z",91,&effect));CHECK(state.pending_persist_operation_id==91);CHECK(effect.action==protocol::problem_study_v1::ReviewAction::kHesitant);
    const int old_reads=reads;Input(state,ProblemInput::kLongConfirm);Input(state,ProblemInput::kConfirm);CHECK(state.list_selected==0);CHECK(reads==old_reads);CHECK(state.pending_persist_operation_id==91);
    ApplyProblemVerdictCommitResult(&state,ESP_ERR_NO_MEM);CHECK(state.list_selected==0);CHECK(state.commit_state==ProblemVerdictCommitState::kFailed);Paint(state);CHECK(HasText("记录空间已满"));Save(folder,"save-full");
    Input(state,ProblemInput::kDown);Paint(state);CHECK(HasText("记录空间已满"));
    Input(state,ProblemInput::kConfirm);CHECK(state.mode==ProblemAppMode::kVerdict);Paint(state);CHECK(HasText("记录空间已满"));Save(folder,"verdict-retry");
    Input(state,ProblemInput::kConfirm);CHECK(TakeProblemVerdictEffect(&state,"request-2","2026-10-19T00:00:01Z",92,&effect));ApplyProblemVerdictCommitResult(&state,ESP_OK);CHECK(state.list_selected==1);CHECK(!state.answer_unlocked);CHECK(state.mode==ProblemAppMode::kProblemView);
    state.list_selected=11;LoadCurrentProblem(&state);Input(state,ProblemInput::kConfirm);Input(state,ProblemInput::kConfirm);Input(state,ProblemInput::kConfirm);ApplyProblemVerdictCommitResult(&state,ESP_OK);CHECK(state.mode==ProblemAppMode::kProblemList);CHECK(state.list_selected==11);Paint(state);CHECK(HasText("本轮已完成"));Save(folder,"round-complete");
}
void TestImagesAndNotices(const std::string& folder) {
    using namespace wqn;using namespace device_ui_internal;
    auto state=MakeList();auto& first=records[state.pack_index.entries[0].problem_id];
    first.image_ids={"bw-0","bw-1"};first.gray4_image_ids={"gray-0","gray-1"};
    first.solution_image_ids={"solution"};
    Input(state,ProblemInput::kConfirm);Input(state,ProblemInput::kUp);
    CHECK(state.image_index==1);CHECK(state.image_expected_id=="gray-1");
    std::string id,problem_id;bool solution=false,gray=false;uint8_t index=0;
    CHECK(TakeProblemImageRequest(&state,&problem_id,&solution,&index,&id,&gray));CHECK(gray);CHECK(!solution);CHECK(index==1);
    auto gray_payload=std::make_shared<std::vector<uint8_t>>(kNoteImageHeaderBytes+kNoteImageGray4PayloadBytes);(*gray_payload)[5]=2;
    ApplyProblemImageResult(&state,ESP_OK,id,gray_payload);
    UiFrame frame;frame.problem_app=BuildProblemAppSnapshot(state);
    CHECK(RenderProblemBrowseToEpd(frame,RefreshSchedule::kCommit)==ESP_OK);CHECK(gray_blits==1);
    Input(state,ProblemInput::kUp);CHECK(state.image_index==0);
    CHECK(TakeProblemImageRequest(&state,&problem_id,&solution,&index,&id,&gray));CHECK(gray);
    ApplyProblemImageResult(&state,ESP_ERR_NOT_FOUND,id,nullptr);CHECK(state.image_request);CHECK(!state.image_expected_gray4);
    CHECK(TakeProblemImageRequest(&state,&problem_id,&solution,&index,&id,&gray));CHECK(!gray);CHECK(id=="bw-0");
    auto bw_payload=std::make_shared<std::vector<uint8_t>>(kNoteImageHeaderBytes+kEpdFramebufferSize);
    ApplyProblemImageResult(&state,ESP_OK,id,bw_payload);frame.problem_app=BuildProblemAppSnapshot(state);
    CHECK(RenderProblemBrowseToEpd(frame,RefreshSchedule::kCommit)==ESP_OK);CHECK(bw_blits==1);
    Input(state,ProblemInput::kDown);CHECK(TakeProblemImageRequest(&state,&problem_id,&solution,&index,&id,&gray));
    ApplyProblemImageResult(&state,ESP_FAIL,id,nullptr);Paint(state);CHECK(HasText("图片加载失败"));Save(folder,"image-error");
    // Confirm on an attachment retains answer-unlock semantics in this batch.
    Input(state,ProblemInput::kConfirm);CHECK(state.answer_unlocked);CHECK(SegmentFace(state,state.ring_segment)==ProblemFace::kAnswer);
    Input(state,ProblemInput::kLongConfirm);CHECK(state.mode==ProblemAppMode::kProblemList);CHECK(state.list_selected==0);
    auto old=ProblemAppSignature(state);state.cloud_sync_failed=true;state.message="请重新配对";
    CHECK(ProblemAppSignature(state)!=old);Paint(state);CHECK(HasText("请重新配对"));Save(folder,"pairing-notice");
    UiFrame before,after;before.problem_app=BuildProblemAppSnapshot(state);state.message="网络超时，请稍后重试";after.problem_app=BuildProblemAppSnapshot(state);CHECK(Signature(before)!=Signature(after));
    state.message.clear();state.outbox.suspended_count=2;Paint(state);CHECK(HasText("同步挂起 2 条"));
    state.cloud_sync_failed=false;state.outbox.suspended_count=0;
    old=ProblemAppSignature(state);state.message="已到顶部";CHECK(ProblemAppSignature(state)==old);
}
int main(int argc,char**argv) {
    const std::string folder=argc>1?argv[1]:"";
    CHECK(wqn::RunProblemPageStateSelfTest());
    TestList(folder);TestText(folder);TestVerdict(folder);TestImagesAndNotices(folder);
    std::printf("Problem production UI: %d assertions passed\n",checks);
}
