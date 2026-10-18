#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "display_service.h"
#include "font_zectrix.h"
#include "note_list_model.h"
#include "ui/ai_native_text.h"
#include "ui/ui_layout.h"
#include "ui/ai_text_layout.h"
#include "ui/note_reading_geometry.h"

constexpr int ESP_FAIL=-1;
#define ESP_RETURN_ON_ERROR(expr,...) do { const int result=(expr);if(result!=ESP_OK)return result; } while(0)
void Log(const char*,const char*,...) {}
const char* esp_err_to_name(int) { return "host-result"; }
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
int checks=0;
#define CHECK(c) do { if(!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort(); } ++checks; } while(0)
struct TextRun { int x,y;const lv_font_t* font;std::string text;bool ink; };
std::vector<TextRun> drawn;
namespace wqn::protocol::note_study_v1 {
@@PROTOCOL@@
}
namespace wqn {
template<class T>using NotePsramAllocator=std::allocator<T>;
template<class T>using NoteStorePsramAllocator=std::allocator<T>;
constexpr size_t kNoteObservationOutboxCapacity=1000;
constexpr size_t kNoteImageHeaderBytes=20,kNoteImageGray4PayloadBytes=60000;
enum class ImageRenderMode : uint8_t { kBlackWhite,kGray16 };
@@PACK@@
@@STORE@@
@@MODEL@@
std::string NoteAppStatusLine(const NoteAppState&);
std::string NoteAppNotice(const NoteAppState&);
@@NOTICE@@
WqnNoteEntry stored_note;
int body_reads=0;
int ReadNotePackEntry(const NotePackIndexEntry&,WqnNoteEntry* destination) { ++body_reads;*destination=stored_note;return ESP_OK; }
struct HomeSummary { bool charging=false,full=false,wifi_connected=true;int battery_percent=76,wifi_rssi=-50; };
enum class UiScreen { kNote,kHome };
struct UiFrame { UiScreen screen=UiScreen::kNote;NoteAppSnapshot note_app;HomeSummary home; };
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
    while(*cursor&&DecodeUtf8(cursor,&cp)) { DrawGlyphFromFont(x,y,font,cp,ink);x+=MeasureGlyphWidthInFont(font,cp); }
}
}
@@FIND@@
@@WINDOW@@
@@LOAD@@
@@BOUNDS@@
namespace wqn {
@@SNAPSHOT@@
}
namespace device_ui_internal {
void FillRect(int,int,int,int,bool);
void DrawHorizontalLine(int,int,int);
void DrawVerticalLine(int,int,int);
void DrawRect(int,int,int,int);
esp_err_t DrawClippedText(int,int,int,const std::string&,bool ink=true);
}
@@MD_RENDER@@
namespace device_ui_internal {
void FillRect(int x,int y,int w,int h,bool ink) { for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)wqn::DrawEpdPixel(xx,yy,ink); }
void DrawHorizontalLine(int x,int y,int w) { FillRect(x,y,w,1,true); }
void DrawVerticalLine(int x,int y,int h) { FillRect(x,y,1,h,true); }
void DrawRect(int x,int y,int w,int h) { DrawHorizontalLine(x,y,w);DrawHorizontalLine(x,y+h-1,w);DrawVerticalLine(x,y,h);DrawVerticalLine(x+w-1,y,h); }
esp_err_t DrawClippedText(int x,int y,int w,const std::string& text,bool ink) { AiNativeText(x,y,w,text,16,ink);return ESP_OK; }
std::string CurrentClockLabel() { return "14:32"; }
@@BATTERY@@
@@ICONS@@
@@LISTS@@
@@BODY@@
std::string NoteFrameSignature(const wqn::UiFrame& frame) {
    std::string signature;
@@SIGNATURE@@
    return signature;
}
}
void PaintBody(const wqn::NoteAppSnapshot& note) {
    wqn::UiFrame frame;frame.note_app=note;
    device_ui_internal::DrawNoteStatusBar(frame);device_ui_internal::RenderNoteBody(note);
}
void Save(const std::string& folder,const std::string& name) {
    if(folder.empty())return;
    std::ofstream out(folder+"/"+name+".pgm",std::ios::binary);
    out<<"P5\n400 300\n255\n";for(auto pixel:wqn::pixels)out.put(pixel?0:255);
}
bool HasText(const std::string& text) { return std::any_of(drawn.begin(),drawn.end(),[&](const auto& run){return run.text==text;}); }
void Bounds() { for(const auto& run:drawn){CHECK(run.x>=0&&run.y>=0);CHECK(run.x+wqn::MeasureTextWithFont(run.font,run.text.c_str())<=400);CHECK(run.y+run.font->line_height<=300);CHECK(run.text.find('\n')==std::string::npos);} }
int main(int argc,char**argv) {
    using namespace device_ui_internal;using namespace wqn;
    const std::string folder=argc>1?argv[1]:"";
    NoteAppState state;
    for(int i=0;i<23;++i){NotePackNotebook book;book.title="数学笔记与函数学习记录 "+std::to_string(i);book.notebook_id="book-"+std::to_string(i);book.entry_count=12;book.has_pack=true;state.pack_index.notebooks.push_back(book);}
    state.problem_sets.push_back({"problem-set","数学错题本",26});state.word_decks.push_back({"word-deck","高考核心词汇",128});
    for(size_t selected=0;selected<25;++selected){
        state.notebook_selected=selected;UpdateListViewport(selected,25,&state.notebook_window_start);
        auto snapshot=BuildNoteAppSnapshot(state);
        ClearEpdFramebuffer();RenderNotebookList(snapshot);Bounds();
        CHECK(drawn.size()==15);CHECK(state.notebook_window_start<=selected&&selected<state.notebook_window_start+4);
        CHECK(HasText(std::to_string(selected+1)+" / 25"));
        if(selected==0 || selected==24) {
            UiFrame root_frame;root_frame.note_app=snapshot;DrawNoteStatusBar(root_frame);Bounds();
            Save(folder,selected==0?"notebooks":"library-tail");
        }
    }
    CHECK(HasText("128 个单词"));CHECK(HasText("26 道错题"));
    CHECK(ClampNoteListWindow(8,10,25)==8);CHECK(ClampNoteListWindow(8,7,25)==7);CHECK(ClampNoteListWindow(20,24,2)==0);
    CHECK(LastViewedLabel("")=="未读");CHECK(LastViewedLabel("2026-10-17T18:30:00.000Z")=="看过 2026-10-18");
    state.message="已到底部";CHECK(NoteAppNotice(state).empty());const auto quiet=NoteAppSignature(state);state.message.clear();CHECK(NoteAppSignature(state)==quiet);
    state.body_fetch_request=true;CHECK(NoteAppNotice(state)=="正在同步内容...");state.body_fetch_request=false;state.body_fetch_error=true;CHECK(!NoteAppNotice(state).empty());
    state.message="请重新配对";CHECK(NoteAppNotice(state)=="请重新配对");
    state.body_fetch_error=false;state.message="打开失败，请重试";CHECK(NoteAppSignature(state)!=quiet);
    state.message.clear();state.session.commit_state=NoteObservationCommitState::kFailed;CHECK(!NoteAppNotice(state).empty());state.session.commit_state=NoteObservationCommitState::kIdle;
    state.mode=NoteAppMode::kNoteList;state.session.persisted.remote.notebook_id="book-0";
    for(int i=0;i<12;++i){StoredNoteSessionItem item;std::snprintf(item.item_id,sizeof(item.item_id),"note-%02d",i);std::strcpy(item.notebook_id,"book-0");state.session.persisted.remote.items.push_back(item);
        NotePackIndexEntry entry={};std::strcpy(entry.note_id,item.item_id);std::strcpy(entry.notebook_id,"book-0");std::snprintf(entry.title,sizeof(entry.title),"函数的定义域、单调性与导数学习记录 %d",i);state.pack_index.entries.push_back(entry);state.pack_index.note_order.push_back(i);}
    state.session.persisted.remote.has_more=true;
    UiFrame frame;frame.note_app=BuildNoteAppSnapshot(state);
    ClearEpdFramebuffer();DrawNoteStatusBar(frame);RenderNoteList(frame.note_app);Bounds();CHECK(HasText("已加载 12 条笔记"));CHECK(HasText("1 / 12+"));CHECK(HasText("未读"));Save(folder,"titles-loading");
    auto previous=NoteFrameSignature(frame);frame.note_app.titles_have_more=false;CHECK(NoteFrameSignature(frame)!=previous);previous=NoteFrameSignature(frame);frame.note_app.notice="同步失败，确认重试";CHECK(NoteFrameSignature(frame)!=previous);
    ClearEpdFramebuffer();DrawNoteStatusBar(frame);RenderNoteList(frame.note_app);Bounds();CHECK(HasText("同步失败，确认重试"));Save(folder,"sync-error");
    previous=NoteFrameSignature(frame);frame.note_app.notebooks[0].kind=NoteLibraryRowKind::kWordDeck;CHECK(NoteFrameSignature(frame)!=previous);
    previous=NoteFrameSignature(frame);frame.note_app.titles[0].content_available=false;CHECK(NoteFrameSignature(frame)!=previous);
    ClearEpdFramebuffer();RenderNoteList(frame.note_app);CHECK(HasText("内容未同步"));
    frame.note_app.mode=NoteAppMode::kNoteView;frame.note_app.note_title=std::string(200,'W')+"\n标题不能覆盖时间和状态图标";
    ClearEpdFramebuffer();DrawNoteStatusBar(frame);Bounds();CHECK(drawn.back().x+wqn::MeasureTextWithFont(drawn.back().font,drawn.back().text.c_str())+8<=drawn[2].x);Save(folder,"long-header");
    frame.note_app=NoteAppSnapshot{};ClearEpdFramebuffer();DrawNoteStatusBar(frame);RenderNoteList(frame.note_app);Bounds();CHECK(HasText("该笔记本暂无笔记"));Save(folder,"empty");
    // Loading produces the exact immutable result consumed by the snapshot;
    // rendering every position must not trigger any new layout build.
    state.note_list_selected=0;state.message.clear();state.mode=NoteAppMode::kNoteView;
    stored_note.note_id="note-00";stored_note.title="函数笔记";
    for(int i=1;i<=42;++i){if(i>1)stored_note.content+='\n';stored_note.content+="NOTE-"+std::to_string(i);}
    const auto builds=GetAiTextCacheStats().builds;
    LoadCurrentNoteBody(&state);CHECK(state.body_layout!=nullptr);CHECK(state.body_layout->markdown.size()==42);
    CHECK(GetAiTextCacheStats().builds==builds+1);
    auto held=BuildNoteAppSnapshot(state);CHECK(held.body_layout==state.body_layout);
    for(int images:{0,3})for(bool notice:{false,true}) {
        state.current_note.image_ids.assign(images,"image");state.message=notice?"阅读记录未保存":"";
        auto shot=BuildNoteAppSnapshot(state);
        auto geometry=ComputeNoteReadingGeometry(42,images!=0,notice);
        CHECK(NoteBodyMaxScroll(state)==geometry.max_scroll);CHECK(geometry.visible==static_cast<uint32_t>(13-images/3-static_cast<int>(notice)));
        for(uint32_t offset=0;offset<=geometry.max_scroll;++offset) {
            shot.note_scroll_offset_lines=offset;ClearEpdFramebuffer();PaintBody(shot);Bounds();
            CHECK(HasText("NOTE-"+std::to_string(offset+1)));
            for(const auto& run:drawn)if(run.text.rfind("NOTE-",0)==0){CHECK(run.y>=geometry.top);CHECK(run.y+20<=296);}
        }
        CHECK(HasText("NOTE-42"));
        if(images==3&&!notice)Save(folder,"body-tail");
    }
    CHECK(GetAiTextCacheStats().builds==builds+1);
    state.current_note.image_ids.clear();state.message.clear();
    ClearEpdFramebuffer();PaintBody(BuildNoteAppSnapshot(state));Save(folder,"body-head");
    state.body_layout.reset();state.current_note={};CHECK(held.body_layout->markdown.back().text=="NOTE-42");
    LoadCurrentNoteBody(&state);CHECK(state.body_layout==held.body_layout);
    UiFrame before_frame;before_frame.note_app=BuildNoteAppSnapshot(state);
    stored_note.content.replace(stored_note.content.rfind("NOTE-42"),7,"TAIL-42");LoadCurrentNoteBody(&state);
    UiFrame after_frame;after_frame.note_app=BuildNoteAppSnapshot(state);
    CHECK(NoteFrameSignature(before_frame)!=NoteFrameSignature(after_frame));CHECK(before_frame.note_app.body_layout->markdown.back().text=="NOTE-42");
    const std::string mixed="# 原生混排\n\n- 中文与 English words\n> 引用和 `inline code`\n```cpp\nstd::string title = \"note\";\n```\n| 条件 | 结论 |\n|---|---|\n| x > 0 | 递增 |\n| x < 0 | 递减 |";
    stored_note.content=mixed;LoadCurrentNoteBody(&state);
    auto mixed_shot=BuildNoteAppSnapshot(state);auto mg=ComputeNoteReadingGeometry(state.body_layout->markdown.size(),false,false);
    for(uint32_t offset=0;offset<=mg.max_scroll;++offset){mixed_shot.note_scroll_offset_lines=offset;ClearEpdFramebuffer();PaintBody(mixed_shot);Bounds();}
    Save(folder,"mixed-markdown");
    stored_note.content.clear();for(int i=0;i<4097;++i)stored_note.content+="x\n";LoadCurrentNoteBody(&state);
    CHECK(state.body_layout->truncated);CHECK(state.body_layout->markdown.size()==4096);
    CHECK(state.body_layout->markdown.back().text=="内容过长，后续未显示");
    std::printf("Note production UI: %d assertions passed\n",checks);
}
