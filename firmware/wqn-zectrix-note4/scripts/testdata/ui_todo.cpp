#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include "display_service.h"
#include "font_zectrix.h"
#include "ui/ai_native_text.h"
#include "ui/ui_layout.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include "todo_list_model.h"
constexpr int ESP_FAIL=-1,ESP_ERR_INVALID_STATE=-2,ESP_ERR_INVALID_ARG=-3;
int checks=0;
#define CHECK(c) do { if(!(c)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort();}++checks; } while(0)
namespace wqn {
@@ITEM@@
@@MODEL@@
enum class UiScreen { kTodo,kHome,kTime };
enum class TimeTile { kCountdown,kPomodoro,kClock };
enum class UiInput { kUp,kDown,kConfirm,kLongConfirm };
struct UiState { UiScreen screen=UiScreen::kTodo;TodoUiState todo;
    struct { bool visible=false; } navigation;
    struct { TimeTile tile=TimeTile::kClock;bool config_mode=false; } time_app;
};
struct HomeSummary { bool charging=false,full=false,wifi_connected=true;int battery_percent=76,wifi_rssi=-50; };
struct UiFrame { UiScreen screen=UiScreen::kTodo;TodoUiState todo;HomeSummary home;
    struct { bool visible=false;size_t selected=0; } navigation;
};
void ClampUiSelection(UiState* state) {
@@CLAMP@@
}
}
namespace wqn {
std::vector<WqnTodoItem> network_rows;
int timeline_result=ESP_OK,complete_result=ESP_OK,timeline_calls=0;
esp_err_t FetchTodoTimeline(const std::string&,const WqnTodoTimelineRequest& request,WqnTodoListPage* page){
    ++timeline_calls;if(timeline_result!=ESP_OK)return timeline_result;
    const size_t maximum=network_rows.size()>24?network_rows.size()-24:0;
    size_t start=request.cursor.empty()?0:static_cast<size_t>(std::stoul(request.cursor.substr(7)));
    start=std::min(start,maximum);*page={};page->total=network_rows.size();page->selected_index=0;
    page->todos.assign(network_rows.begin()+start,network_rows.begin()+std::min(start+24,network_rows.size()));
    page->has_earlier=start>0;page->has_later=start+page->todos.size()<network_rows.size();
    if(page->has_earlier)page->previous_cursor="offset:"+std::to_string(start>24?start-24:0);
    if(page->has_later)page->next_cursor="offset:"+std::to_string(std::min(start+24,maximum));
    return ESP_OK;
}
esp_err_t CompleteTodo(const std::string&,const char* id,WqnTodoItem* task){
    if(complete_result!=ESP_OK)return complete_result;
    const auto found=std::find_if(network_rows.begin(),network_rows.end(),[&](const auto& row){return row.id==id;});
    if(found==network_rows.end())return ESP_FAIL;
    *task=*found;task->status="completed";network_rows.erase(found);return ESP_OK;
}
}
namespace device_ui_internal {
@@SCHEDULE@@
@@CLOCK_POLICY@@
int primary_time_updates=0;
void UpdateHomePrimaryTimeLine(wqn::UiState*){++primary_time_updates;}
struct UiUpdate { RefreshSchedule refresh;bool changed; };
enum class AppEventKind { kClockMinute };
struct UiRuntime {
    wqn::UiState state_;
    UiUpdate FinishEvent(AppEventKind,RefreshSchedule schedule,bool changed){return {schedule,changed};}
    UiUpdate DispatchClockMinute(bool);
};
@@MINUTE@@
@@RESULT@@
bool busy=false,admit=true,token_valid=true;
int requests=0,summaries=0;
TodoCloudRequest last_request;
bool QueueTodoCloudRequest(const TodoCloudRequest& request) {
    if(busy||!admit)return false;
    last_request=request;busy=true;++requests;return true;
}
bool IsTodoCloudBusy(){return busy;}
bool LoadValidTokenForTodo(std::string* token){*token=token_valid?"host-token":"";return token_valid;}
void BuildHomeSummary(wqn::UiState*){++summaries;}
@@CLOUD@@
TodoCloudResult g_todo_result_slot;
uint32_t g_todo_result_generation=0;
int publications=0;
void SendTodoCloudResult(){++publications;}
@@EXECUTE@@
}
struct TextRun { int x,y;const lv_font_t* font;std::string text;bool ink; };
std::vector<TextRun> drawn;
namespace wqn {
std::array<uint8_t,400*300> pixels;
bool framebuffer_ready=true;
int full_clears=0,pixel_writes=0;
bool IsEpdFramebufferSynchronized(){return framebuffer_ready;}
extern "C" const lv_font_t SourceHanSansSC_Regular_slim;
extern "C" bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t*,lv_font_glyph_dsc_t*,uint32_t,uint32_t) { return false; }
extern "C" const uint8_t* lv_font_get_bitmap_fmt_txt(const lv_font_t*,uint32_t) { return nullptr; }
void DrawEpdPixel(int x,int y,bool ink) { if(x>=0&&x<400&&y>=0&&y<300){pixels[y*400+x]=ink;++pixel_writes;} }
void ClearEpdFramebuffer(bool white) { ++full_clears;pixels.fill(!white);drawn.clear(); }
const lv_font_t* PrimaryUiFont() { return &SourceHanSansSC_Regular_slim; }
@@FONT_LOOKUP@@
@@FONT_DRAW@@
@@FONT_MEASURE@@
void DrawTextWithFont(int x,int y,const lv_font_t* font,const char* text,bool ink) {
    drawn.push_back({x,y,font,text,ink});const char* cursor=text;uint32_t cp;
    while(*cursor&&DecodeUtf8(cursor,&cp)) { DrawGlyphFromFont(x,y,font,cp,ink);x+=MeasureGlyphWidthInFont(font,cp); }
}
}
namespace device_ui_internal {
std::time_t clock_now=0;
constexpr std::time_t kMinReasonableUnixTime=1700000000;
std::string clock_label="20:00",date_label="10月19日 星期一";
std::time_t CurrentUnixTime(){return clock_now;}
std::string CurrentClockLabel(){return clock_label;}
std::string CurrentDateLabel(){return date_label;}
void FillRect(int x,int y,int w,int h,bool ink){for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)wqn::DrawEpdPixel(xx,yy,ink);}
void DrawHorizontalLine(int x,int y,int w){FillRect(x,y,w,1,true);}
void ClearRect(const UiRect& r){FillRect(r.x,r.y,r.width,r.height,false);}
wqn::UiScreen g_last_rendered_screen=wqn::UiScreen::kTodo;
int presented=0;RefreshSchedule presented_schedule=RefreshSchedule::kNone;
int regional=0,render_result=ESP_OK;
esp_err_t RefreshFrame(const wqn::UiFrame& frame,RefreshSchedule schedule){
    ++presented;presented_schedule=schedule;
    if(frame.navigation.visible)FillRect(kNavigationX,kNavigationY,kNavigationWidth,kNavigationHeight,true);
    if(render_result==ESP_OK){g_last_rendered_screen=frame.screen;wqn::framebuffer_ready=true;}
    return render_result;
}
esp_err_t RefreshRegion(const UiRect& r,RefreshSchedule schedule){
    CHECK(r.x==0&&r.y==0&&r.width==400&&r.height==kStatusBarHeight);
    ++regional;presented_schedule=schedule;return render_result;
}
@@BATTERY@@
@@ICONS@@
std::string TodoSignature(const wqn::UiFrame& frame){std::string signature;
@@SIGNATURE@@
return signature;}
}
@@PAGE@@
using namespace wqn;
using namespace device_ui_internal;
WqnTodoListPage Page(int count,int base=0){
    WqnTodoListPage page;page.total=count;page.selected_index=0;
    for(int i=0;i<count;++i){WqnTodoItem item;item.id="task-"+std::to_string(base+i);item.title="待办 "+std::to_string(i);item.status="pending";item.subject_name="数学";page.todos.push_back(item);}
    return page;
}
void TestWindows(){
    for(int count:{0,1,3,4,5,12,24}){
        UiState state;ApplyTodoList(&state,Page(count),false);
        for(int i=1;i<count;++i){
            CHECK(HandleTodoInput(&state,UiInput::kDown)==RefreshSchedule::kSelection);
            CHECK(state.todo.selected==static_cast<size_t>(i));
            CHECK(state.todo.window_start==static_cast<size_t>(i<4?0:i-3));
        }
        for(int i=0;i<5;++i)CHECK(HandleTodoInput(&state,UiInput::kDown)==RefreshSchedule::kNone);
        size_t window=state.todo.window_start;
        for(int i=count-2;i>=0;--i){
            CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kSelection);
            CHECK(state.todo.selected==static_cast<size_t>(i));
            if(static_cast<size_t>(i)<window)window=i;
            CHECK(state.todo.window_start==window);
        }
        for(int i=0;i<5;++i)CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kNone);
    }
    UiState state;ApplyTodoList(&state,Page(12),false);state.todo.selected=7;ClampUiSelection(&state);
    auto page=Page(12);page.selected_index=0;std::swap(page.todos[6],page.todos[7]);
    ApplyTodoList(&state,page,false);CHECK(state.todo.selected==6);CHECK(state.todo.todos[6].id=="task-7");
    ApplyTodoList(&state,Page(2),false);CHECK(state.todo.selected<2);CHECK(state.todo.window_start==0);
    ApplyTodoList(&state,Page(0),false);CHECK(state.todo.selected==0);CHECK(state.todo.window_start==0);
}
void TestPaging(){
    UiState state;auto page=Page(24);page.total=70;page.has_earlier=page.has_later=true;page.previous_cursor="previous";page.next_cursor="next";
    ApplyTodoList(&state,page,false);busy=false;
    const int before=requests;
    CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kSelection);
    CHECK(std::string(last_request.cursor)=="previous");CHECK(state.todo.sync_status==TodoSyncStatus::kLoading);
    CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kNone);CHECK(requests==before+1);
    TodoCloudResult result;result.result=ESP_OK;result.cursor_request=true;result.page=page;result.page.selected_index=23;
    bool changed=false;CHECK(ApplyTodoCloudResult(&state,result,&changed));CHECK(changed);CHECK(state.todo.selected==23);CHECK(state.todo.window_start==20);busy=false;
    CHECK(HandleTodoInput(&state,UiInput::kDown)==RefreshSchedule::kSelection);CHECK(std::string(last_request.cursor)=="next");
    result.page=Page(24,24);result.page.selected_index=0;CHECK(ApplyTodoCloudResult(&state,result,&changed));busy=false;
    CHECK(state.todo.selected==0);CHECK(state.todo.window_start==0);
    // A failed page request keeps the old page and permits an explicit retry.
    ApplyTodoList(&state,page,true);result.result=ESP_FAIL;CHECK(ApplyTodoCloudResult(&state,result,&changed));
    CHECK(state.todo.todos.size()==24);CHECK(state.todo.previous_cursor=="previous");busy=false;
    CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kSelection);busy=false;
    // After completing the final loaded item, either remote direction remains reachable.
    state.todo.todos.clear();state.todo.selected=state.todo.window_start=0;
    CHECK(HandleTodoInput(&state,UiInput::kDown)==RefreshSchedule::kSelection);CHECK(std::string(last_request.cursor)=="next");busy=false;
    CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kSelection);CHECK(std::string(last_request.cursor)=="previous");busy=false;
    admit=false;CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kSelection);CHECK(state.todo.sync_status==TodoSyncStatus::kSyncFailed);admit=true;
    state.screen=UiScreen::kHome;CHECK(HandleTodoInput(&state,UiInput::kDown)==RefreshSchedule::kNone);
}
void TestCompletion(){
    busy=false;token_valid=true;admit=true;
    UiState state;auto page=Page(8);page.total=60;ApplyTodoList(&state,page,false);
    state.todo.selected=4;ClampUiSelection(&state);
    CHECK(HandleTodoInput(&state,UiInput::kConfirm)==RefreshSchedule::kSelection);
    CHECK(state.todo.completion_task_id=="task-4");CHECK(state.todo.sync_status==TodoSyncStatus::kCompleting);
    const int before=requests;
    for(auto key:{UiInput::kUp,UiInput::kDown,UiInput::kConfirm})CHECK(HandleTodoInput(&state,key)==RefreshSchedule::kNone);
    CHECK(state.todo.selected==4);CHECK(requests==before);
    CHECK(!RefreshTodosFromCloud(&state));CHECK(state.todo.sync_status==TodoSyncStatus::kCompleting);
    TodoCloudResult result;result.op=TodoCloudOp::kComplete;result.result=ESP_FAIL;std::snprintf(result.todo_id,sizeof(result.todo_id),"task-4");
    bool changed=false;CHECK(ApplyTodoCloudResult(&state,result,&changed));busy=false;
    CHECK(state.todo.todos.size()==8);CHECK(state.todo.total_pending==60);CHECK(state.todo.sync_status==TodoSyncStatus::kCompleteFailed);
    CHECK(HandleTodoInput(&state,UiInput::kConfirm)==RefreshSchedule::kSelection);CHECK(std::string(last_request.todo_id)=="task-4");
    result.result=ESP_OK;result.todo.id="task-4";CHECK(ApplyTodoCloudResult(&state,result,&changed));busy=false;
    CHECK(state.todo.todos.size()==7);CHECK(state.todo.total_pending==59);CHECK(state.todo.todos[4].id=="task-5");CHECK(state.todo.selected==4);
    CHECK(state.todo.completion_task_id=="task-4");CHECK(state.todo.sync_status==TodoSyncStatus::kCompleted);
    CHECK(!ApplyTodoCloudResult(&state,result,&changed));CHECK(state.todo.total_pending==59);
    CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kSelection);CHECK(state.todo.completion_task_id.empty());
    // Sync busy must not manufacture an accepted completion or replace its status.
    state.todo.sync_status=TodoSyncStatus::kLoading;busy=true;
    CHECK(HandleTodoInput(&state,UiInput::kConfirm)==RefreshSchedule::kNone);CHECK(state.todo.sync_status==TodoSyncStatus::kLoading);CHECK(state.todo.completion_task_id.empty());busy=false;
    admit=false;CHECK(HandleTodoInput(&state,UiInput::kConfirm)==RefreshSchedule::kSelection);
    CHECK(state.todo.sync_status==TodoSyncStatus::kCompleteFailed);CHECK(state.todo.completion_task_id=="task-3");admit=true;
    CHECK(HandleTodoInput(&state,UiInput::kDown)==RefreshSchedule::kSelection);CHECK(state.todo.sync_status==TodoSyncStatus::kReady);CHECK(state.todo.completion_task_id.empty());
    CHECK(HandleTodoInput(&state,UiInput::kConfirm)==RefreshSchedule::kSelection);
    std::snprintf(result.todo_id,sizeof(result.todo_id),"task-5");result.todo.id="other-task";
    CHECK(ApplyTodoCloudResult(&state,result,&changed));busy=false;CHECK(state.todo.todos.size()==7);CHECK(state.todo.sync_status==TodoSyncStatus::kCompleteFailed);
    // Unrelated or duplicate terminal results cannot affect a newer operation.
    CHECK(!ApplyTodoCloudResult(&state,result,&changed));
    CHECK(HandleTodoInput(&state,UiInput::kConfirm)==RefreshSchedule::kSelection);
    std::snprintf(result.todo_id,sizeof(result.todo_id),"task-3");CHECK(!ApplyTodoCloudResult(&state,result,&changed));CHECK(state.todo.sync_status==TodoSyncStatus::kCompleting);
    std::snprintf(result.todo_id,sizeof(result.todo_id),"task-5");result.todo.id="task-5";result.result=ESP_FAIL;result.auth_required=true;
    CHECK(ApplyTodoCloudResult(&state,result,&changed));CHECK(state.todo.sync_status==TodoSyncStatus::kAuthRequired);busy=false;
    // A transient refresh failure preserves known global totals and every cached row.
    result={};result.op=TodoCloudOp::kRefresh;result.result=ESP_FAIL;
    CHECK(ApplyTodoCloudResult(&state,result,&changed));CHECK(state.todo.total_pending==59);CHECK(state.todo.todos.size()==7);
    token_valid=false;CHECK(RefreshTodosFromCloud(&state));CHECK(state.todo.total_pending==59);token_valid=true;
    UiState last;ApplyTodoList(&last,Page(1),false);CHECK(HandleTodoInput(&last,UiInput::kConfirm)==RefreshSchedule::kSelection);
    result={};result.op=TodoCloudOp::kComplete;result.result=ESP_OK;std::snprintf(result.todo_id,sizeof(result.todo_id),"task-0");
    CHECK(ApplyTodoCloudResult(&last,result,&changed));busy=false;CHECK(last.todo.todos.empty());CHECK(last.todo.selected==0);CHECK(last.todo.window_start==0);CHECK(last.todo.total_pending==0);
    UiState refreshed;ApplyTodoList(&refreshed,Page(2),false);auto updated=Page(2);updated.todos[0].subject_name="生物";
    CHECK(!TodoPageMatchesState(refreshed,updated));result={};result.op=TodoCloudOp::kRefresh;result.result=ESP_OK;result.page=updated;
    CHECK(ApplyTodoCloudResult(&refreshed,result,&changed));CHECK(changed);CHECK(refreshed.todo.todos[0].subject_name=="生物");
}
void Save(const std::string& folder,const std::string& name){
    if(folder.empty())return;
    std::ofstream out(folder+"/"+name+".pgm",std::ios::binary);out<<"P5\n400 300\n255\n";for(auto pixel:pixels)out.put(pixel?0:255);
}
bool HasText(const std::string& text){return std::any_of(drawn.begin(),drawn.end(),[&](const auto& run){return run.text==text;});}
void Paint(const UiFrame& frame,const std::string& folder="",const std::string& name=""){
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kSelection)==ESP_OK);CHECK(presented_schedule==RefreshSchedule::kSelection);
    for(const auto& run:drawn){
        CHECK(run.x>=0&&run.y>=0);CHECK(run.x+MeasureTextWithFont(run.font,run.text.c_str())<=400);
        CHECK(run.y+run.font->line_height<=300);CHECK(run.text.find('\n')==std::string::npos);
        CHECK(run.y<kTodoListTop+4*kTodoRowPitch);
    }
    CHECK(pixels[27*400]==1&&pixels[27*400+399]==1);Save(folder,name);
}
void TestCompletionPaging(){
    busy=false;admit=true;token_valid=true;timeline_result=complete_result=ESP_OK;
    network_rows=Page(52).todos;WqnTodoListPage initial;WqnTodoTimelineRequest request;
    CHECK(FetchTodoTimeline("host",request,&initial)==ESP_OK);UiState state;ApplyTodoList(&state,initial,false);
    state.todo.selected=23;ClampUiSelection(&state);
    CHECK(HandleTodoInput(&state,UiInput::kConfirm)==RefreshSchedule::kSelection);
    ExecuteTodoCloudRequest(last_request);CHECK(g_todo_result_slot.has_refreshed_page);
    bool changed=false;CHECK(ApplyTodoCloudResult(&state,g_todo_result_slot,&changed));busy=false;
    CHECK(state.todo.total_pending==51);CHECK(state.todo.todos.size()==24);
    CHECK(state.todo.todos[23].id=="task-24");CHECK(state.todo.selected==23);CHECK(!state.todo.paging_stale);
    CHECK(HandleTodoInput(&state,UiInput::kDown)==RefreshSchedule::kSelection);CHECK(std::string(last_request.cursor)=="offset:24");
    ExecuteTodoCloudRequest(last_request);CHECK(ApplyTodoCloudResult(&state,g_todo_result_slot,&changed));busy=false;
    CHECK(state.todo.todos[0].id=="task-25");CHECK(state.todo.current_cursor=="offset:24");
    // If completion succeeds but the follow-up GET fails, keep success and
    // force an anchor-preserving reload before using an old offset cursor.
    CHECK(HandleTodoInput(&state,UiInput::kConfirm)==RefreshSchedule::kSelection);timeline_result=ESP_FAIL;
    ExecuteTodoCloudRequest(last_request);CHECK(g_todo_result_slot.result==ESP_OK);CHECK(!g_todo_result_slot.has_refreshed_page);
    CHECK(ApplyTodoCloudResult(&state,g_todo_result_slot,&changed));busy=false;CHECK(state.todo.paging_stale);CHECK(state.todo.sync_status==TodoSyncStatus::kCompleted);
    timeline_result=ESP_OK;CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kSelection);
    CHECK(std::string(last_request.cursor)=="offset:24");CHECK(last_request.op==TodoCloudOp::kRefresh);
    ExecuteTodoCloudRequest(last_request);CHECK(ApplyTodoCloudResult(&state,g_todo_result_slot,&changed));busy=false;
    CHECK(!state.todo.paging_stale);CHECK(state.todo.todos[state.todo.selected].id=="task-26");
    CHECK(state.todo.total_pending==50);
    // Rebase failure keeps the stale flag; another boundary press reloads again.
    state.todo.paging_stale=true;timeline_result=ESP_FAIL;
    CHECK(HandleTodoInput(&state,UiInput::kUp)==RefreshSchedule::kSelection);ExecuteTodoCloudRequest(last_request);
    CHECK(ApplyTodoCloudResult(&state,g_todo_result_slot,&changed));busy=false;CHECK(state.todo.paging_stale);timeline_result=ESP_OK;
}
void TestDates(){
    TodoDue due;CHECK(ParseTodoDue("2026-10-19T12:00:00Z",&due));clock_now=due.instant;
    CHECK(TodoDueTimeLabel("2026-10-19T12:40:00Z")=="20:40");
    CHECK(TodoDueTimeLabel("2026-10-19T20:40:00+08:00")=="20:40");
    CHECK(TodoDueTimeLabel("2026-10-19T05:40:00-07:00")=="20:40");
    CHECK(TodoDueTimeLabel("2026-10-19T20:40")=="20:40");
    CHECK(TodoDueTimeLabel("2026-10-19T16:05:03.123Z")=="00:05");
    CHECK(TodoDueDateLabel("2026-10-19T16:05:03.123Z")=="10月20日");
    CHECK(TodoDueTimeLabel("2026-10-19")=="--:--");CHECK(TodoDueDateLabel("2026-10-19")=="10月19日");
    for(const auto& text:{"", "bad 12:40", "2026-02-29", "2026-04-31", "2026-00-01", "2026-10-19T24:00Z", "2026-10-19T12:99Z", "2026-10-19T12:00+99:00", "2026-10-19T12:00trash", "2026-10-19T12:00:00.Z"})CHECK(!ParseTodoDue(text,&due));
    CHECK(ParseTodoDue("2028-02-29T23:59:59Z",&due));CHECK(TodoDueDateLabel("2028-02-29T23:59:59Z")=="3月1日");
    WqnTodoItem item;item.status="pending";item.due_at="2026-10-19T11:59:59Z";CHECK(TodoIsOverdue(item));
    item.due_at="2026-10-19T12:00:00Z";CHECK(!TodoIsOverdue(item));item.due_at="2026-10-19";CHECK(!TodoIsOverdue(item));
    item.status="completed";item.due_at="2026-10-18";CHECK(!TodoIsOverdue(item));
    item.status="pending";item.completed_at="2026-10-18T12:00:00Z";CHECK(!TodoIsOverdue(item));
    item.completed_at.clear();const auto now=clock_now;clock_now=0;CHECK(!TodoIsOverdue(item));clock_now=now;
}
void TestRender(const std::string& folder){
    UiState state;auto page=Page(8);ApplyTodoList(&state,page,false);UiFrame frame;frame.todo=state.todo;
    const char* names[]={"整理导数专题错题","复习细胞代谢调控","英语阅读精练两篇","核对 WQN 同步结果","订正物理磁场综合题","整理化学实验现象","阅读一篇科普文章并记录要点","准备周测复习清单"};
    for(size_t i=0;i<frame.todo.todos.size();++i){auto& task=frame.todo.todos[i];task.title=names[i];task.subject_name=i==1?"生物":"数学";task.due_at=i==4?"2026-10-18T10:00:00Z":"2026-10-19T12:40:00Z";}
    Paint(frame,folder,"list");CHECK(HasText("待办事项"));CHECK(HasText("20:40"));CHECK(HasText("数学 · 10月19日"));
    CHECK(pixels[98*400+15]==1);CHECK(pixels[144*400+15]==0);CHECK(HasText("01"));CHECK(!HasText("05"));
    CHECK(std::any_of(drawn.begin(),drawn.end(),[](const auto& run){return run.text=="整理导数专题错题"&&!run.ink;}));
    frame.todo.selected=5;frame.todo.window_start=2;Paint(frame,folder,"later-window");CHECK(HasText("03")&&HasText("06"));CHECK(!HasText("02"));CHECK(HasText("逾期"));CHECK(HasText("18:00"));
    frame.todo.todos[3].title="这是一个特别长的任务标题用于检查原生字体下的省略与换行\n也不应该跨行覆盖到日期";
    frame.todo.todos[3].subject_name="一个很长的学科名字与日期仍然需要保持界限";Paint(frame,folder,"long-title");
    for(const auto& run:drawn)if(run.y>=kTodoListTop&&run.x==kTodoLeft+37)CHECK(run.x+MeasureTextWithFont(run.font,run.text.c_str())<=kTodoLeft+kTodoWidth-87);
    frame.todo.sync_status=TodoSyncStatus::kCompleting;frame.todo.completion_task_id="task-5";Paint(frame,folder,"completing");CHECK(HasText("完成中"));
    // The marker follows request ID even if a synthetic snapshot moves focus.
    frame.todo.completion_task_id="task-3";Paint(frame);
    CHECK(std::any_of(drawn.begin(),drawn.end(),[](const auto& run){return run.text=="完成中"&&run.y==kTodoListTop+46+16;}));
    frame.todo.completion_task_id="task-5";frame.todo.sync_status=TodoSyncStatus::kCompleteFailed;Paint(frame,folder,"completion-error");CHECK(HasText("重试"));
    frame.todo.sync_status=TodoSyncStatus::kSyncFailed;Paint(frame,folder,"sync-error");CHECK(HasText("同步失败 · 当前缓存"));CHECK(HasText("03"));
    frame.todo.sync_status=TodoSyncStatus::kReady;frame.todo.paging_stale=true;Paint(frame);CHECK(HasText("列表待刷新"));frame.todo.paging_stale=false;
    frame.todo.sync_status=TodoSyncStatus::kReady;frame.todo.has_earlier=frame.todo.has_later=true;frame.todo.total_pending=70;Paint(frame,folder,"remote-pages");CHECK(HasText("待完成"));CHECK(!HasText("待完成 · 逾期 1"));CHECK(pixels[(kTodoListTop-7)*400+kTodoScrollX]);
    frame.todo.todos.clear();Paint(frame,folder,"page-completed");CHECK(HasText("本页已完成"));CHECK(!HasText("事情都处理好了。"));
    frame.todo={};frame.todo.loaded_once=true;frame.todo.sync_status=TodoSyncStatus::kReady;Paint(frame,folder,"empty");CHECK(HasText("事情都处理好了。"));
    frame.todo.sync_status=TodoSyncStatus::kAuthRequired;Paint(frame,folder,"unpaired");CHECK(HasText("请先连接账户"));
    frame.todo.sync_status=TodoSyncStatus::kLoading;Paint(frame,folder,"loading");CHECK(HasText("正在同步待办"));
    frame.todo.sync_status=TodoSyncStatus::kSyncFailed;Paint(frame,folder,"sync-error-empty");CHECK(HasText("待办同步失败"));
    frame.todo.sync_status=TodoSyncStatus::kIdle;frame.todo.loaded_once=false;Paint(frame);CHECK(HasText("待办尚未加载"));
    for(int count:{1,4,5,24})for(int selected=0;selected<count;++selected){
        frame.todo.todos=Page(count).todos;frame.todo.selected=selected;frame.todo.window_start=ClampTodoListWindow(0,selected,count);frame.todo.total_pending=count;frame.todo.loaded_once=true;Paint(frame);
    }
}
void TestMinutePolicy(){
    UiRuntime runtime;
    for(auto screen:{UiScreen::kHome,UiScreen::kTodo,UiScreen::kTime}) {
        runtime.state_.screen=screen;
        runtime.state_.navigation.visible=false;
        CHECK(ScreenUsesClockMinute(runtime.state_));
        CHECK(runtime.DispatchClockMinute(true).refresh==RefreshSchedule::kClock);
        CHECK(runtime.DispatchClockMinute(false).refresh==RefreshSchedule::kNone);
        runtime.state_.navigation.visible=true;
        const int before=primary_time_updates;
        CHECK(!ScreenUsesClockMinute(runtime.state_));
        CHECK(runtime.DispatchClockMinute(true).refresh==RefreshSchedule::kNone);
        CHECK(primary_time_updates==before+1);
        runtime.state_.navigation.visible=false;
        CHECK(runtime.DispatchClockMinute(true).refresh==RefreshSchedule::kClock);
    }
    runtime.state_.screen=UiScreen::kTime;runtime.state_.time_app.config_mode=true;
    CHECK(!ScreenUsesClockMinute(runtime.state_));
    CHECK(runtime.DispatchClockMinute(true).refresh==RefreshSchedule::kNone);
    runtime.state_.time_app.config_mode=false;runtime.state_.time_app.tile=TimeTile::kCountdown;
    CHECK(!ScreenUsesClockMinute(runtime.state_));
}
void TestMinuteRender(){
    UiFrame frame;frame.todo.todos=Page(6).todos;frame.todo.loaded_once=true;
    frame.todo.sync_status=TodoSyncStatus::kReady;frame.todo.total_pending=6;
    for(auto& item:frame.todo.todos)item.due_at="2026-10-19T12:40:00Z";
    TodoDue now;CHECK(ParseTodoDue("2026-10-19T12:00:00Z",&now));clock_now=now.instant;
    date_label="10月19日 星期一";clock_label="20:00";
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kCommit)==ESP_OK);
    const auto first_frame=pixels;
    const int old_clears=full_clears,old_presented=presented,old_regions=regional;
    clock_now+=60;clock_label="20:01";drawn.clear();
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK);
    CHECK(full_clears==old_clears&&presented==old_presented&&regional==old_regions+1);
    CHECK(!drawn.empty()&&std::all_of(drawn.begin(),drawn.end(),[](const auto& run){return run.y<kStatusBarHeight;}));
    CHECK(std::equal(pixels.begin()+kStatusBarHeight*400,pixels.end(),first_frame.begin()+kStatusBarHeight*400));
    auto clock_frame=pixels;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kCommit)==ESP_OK);
    CHECK(pixels==clock_frame);
    // The minute producer pauses cosmetic drawing while navigation is open;
    // closing the bar composes the current clock instead of the old minute.
    frame.navigation.visible=true;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kNavigation)==ESP_OK);
    const auto with_nav=pixels;
    clock_label="20:17";clock_now+=16*60;
    UiRuntime runtime;runtime.state_.screen=UiScreen::kTodo;runtime.state_.navigation.visible=true;
    CHECK(runtime.DispatchClockMinute(true).refresh==RefreshSchedule::kNone);
    CHECK(pixels==with_nav);
    frame.navigation.visible=false;
    int before=presented;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kNavigation)==ESP_OK&&presented==before+1);
    CHECK(HasText("20:17"));
    frame.home.battery_percent=20;frame.home.wifi_rssi=-90;clock_label="20:03";
    const int before_icons=regional;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&regional==before_icons+1);
    clock_frame=pixels;CHECK(RenderTodoToEpd(frame,RefreshSchedule::kCommit)==ESP_OK);CHECK(pixels==clock_frame);

    // Time affects the body too: a newly overdue visible/hidden task or a
    // date rollover must repaint the rows/date/aggregate, not just the clock.
    for(int index:{0,4}) {
        frame.todo.todos[index].due_at="2026-10-19T12:03:30Z";
        clock_now=now.instant+180;clock_label="20:03";
        CHECK(RenderTodoToEpd(frame,RefreshSchedule::kCommit)==ESP_OK);
        before=presented;clock_now+=60;clock_label="20:04";
        CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&presented==before+1);
        CHECK(HasText("待完成 · 逾期 "+std::to_string(index==0?1:2)));
        clock_frame=pixels;CHECK(RenderTodoToEpd(frame,RefreshSchedule::kCommit)==ESP_OK);CHECK(pixels==clock_frame);
    }
    before=presented;date_label="10月20日 星期二";
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&presented==before+1);
    CHECK(HasText(date_label));
    before=presented;frame.todo.selected=1;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&presented==before+1);
    before=presented;frame.todo.todos[1].title="变更的任务标题";
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&presented==before+1);
    CHECK(HasText("变更的任务标题"));

    // Failures cannot prime the body cache, even when the hardware port still
    // reports a synchronized mirror. Cold mirrors and other pages also fall back.
    render_result=ESP_FAIL;const int before_failure=regional;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_FAIL&&regional==before_failure+1);
    render_result=ESP_OK;before=presented;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&presented==before+1);
    render_result=ESP_FAIL;before=presented;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kCommit)==ESP_FAIL&&presented==before+1);
    render_result=ESP_OK;before=presented;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&presented==before+1);
    wqn::framebuffer_ready=false;before=presented;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&presented==before+1);
    g_last_rendered_screen=UiScreen::kHome;before=presented;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&presented==before+1);
    // A failed different page can dirty RAM without advancing the successful
    // screen marker. Dispatcher invalidation prevents retaining that RAM.
    const auto clean_body=pixels;
    InvalidateTodoRenderCache();pixels.fill(1);before=presented;
    CHECK(RenderTodoToEpd(frame,RefreshSchedule::kClock)==ESP_OK&&presented==before+1);
    CHECK(pixels==clean_body);
    before=presented;CHECK(RenderTodoToEpd(frame,RefreshSchedule::kSelection)==ESP_OK&&presented==before+1);
    date_label="10月19日 星期一";clock_label="20:00";clock_now=now.instant;
}
void TestSignature(){
    UiFrame frame;frame.todo.todos=Page(2).todos;const auto original=TodoSignature(frame);
    auto changes=[&](auto apply){auto next=frame;apply(next);CHECK(TodoSignature(next)!=original);};
    changes([](auto& f){f.todo.window_start=1;});changes([](auto& f){f.todo.selected=1;});changes([](auto& f){f.todo.loaded_once=true;});
    changes([](auto& f){f.todo.has_later=true;});changes([](auto& f){f.todo.has_earlier=true;});changes([](auto& f){f.todo.total_pending=80;});
    changes([](auto& f){f.todo.todos[0].subject_name="生物";});changes([](auto& f){f.todo.todos[0].completed_at="completed";});
    changes([](auto& f){f.todo.completion_task_id="task-0";});changes([](auto& f){f.todo.completion_title="done";});
    changes([](auto& f){f.home.wifi_rssi=-90;});changes([](auto& f){f.home.battery_percent=20;});changes([](auto& f){f.home.charging=true;});
    const auto date=date_label;date_label="10月20日 星期二";CHECK(TodoSignature(frame)!=original);date_label=date;
    auto one=frame,two=frame;one.todo.todos[0].title="a:b";one.todo.todos[0].status="c";two.todo.todos[0].title="a";two.todo.todos[0].status="b:c";CHECK(TodoSignature(one)!=TodoSignature(two));
}
int main(int argc,char** argv){TestWindows();TestPaging();TestCompletion();TestCompletionPaging();TestDates();TestRender(argc>1?argv[1]:"");TestMinutePolicy();TestMinuteRender();TestSignature();std::printf("Todo: %d assertions passed\n",checks);}
