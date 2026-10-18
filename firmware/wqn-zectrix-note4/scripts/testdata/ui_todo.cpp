#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include "todo_list_model.h"
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1;
int checks=0;
#define CHECK(c) do { if(!(c)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort();}++checks; } while(0)
namespace wqn {
@@ITEM@@
@@MODEL@@
enum class UiScreen { kTodo,kHome };
enum class UiInput { kUp,kDown,kConfirm,kLongConfirm };
struct UiState { UiScreen screen=UiScreen::kTodo;TodoUiState todo; };
void ClampUiSelection(UiState* state) {
@@CLAMP@@
}
}
namespace device_ui_internal {
@@SCHEDULE@@
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
}
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
int main(){TestWindows();TestPaging();std::printf("Todo: %d assertions passed\n",checks);}
