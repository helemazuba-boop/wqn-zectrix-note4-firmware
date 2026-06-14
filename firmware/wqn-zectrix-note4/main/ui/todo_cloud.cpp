// Todo cloud task: timeline refresh, complete a single todo, result dispatch.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>
#include <cstdio>
#include <new>
#include <string>

#include "esp_log.h"
#include "wqn_api.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

QueueHandle_t g_todo_request_queue = nullptr;
QueueHandle_t g_todo_result_queue = nullptr;
TaskHandle_t g_todo_task = nullptr;
volatile bool g_todo_cloud_busy = false;

bool LoadValidTokenForTodo(std::string* token)
{
    if (token == nullptr) {
        return false;
    }
    token->clear();
    const esp_err_t result = wqn::LoadAccessToken(token);
    return result == ESP_OK && !token->empty() && wqn::IsValidAccessToken(*token);
}

bool IsTodoCloudBusy()
{
    return g_todo_cloud_busy;
}

bool QueueTodoCloudRequest(const TodoCloudRequest& request)
{
    if (g_todo_request_queue == nullptr || IsTodoCloudBusy()) {
        return false;
    }
    if (xQueueSend(g_todo_request_queue, &request, 0) != pdTRUE) {
        return false;
    }
    g_todo_cloud_busy = true;
    return true;
}

bool QueueTodoRefresh()
{
    TodoCloudRequest request;
    request.op = TodoCloudOp::kRefresh;
    return QueueTodoCloudRequest(request);
}

bool QueueTodoRefreshCursor(const std::string& cursor)
{
    if (cursor.empty()) {
        return false;
    }
    TodoCloudRequest request;
    request.op = TodoCloudOp::kRefresh;
    std::snprintf(request.cursor, sizeof(request.cursor), "%s", cursor.c_str());
    return QueueTodoCloudRequest(request);
}

bool QueueTodoComplete(const std::string& todo_id)
{
    if (todo_id.empty()) {
        return false;
    }
    TodoCloudRequest request;
    request.op = TodoCloudOp::kComplete;
    std::snprintf(request.todo_id, sizeof(request.todo_id), "%s", todo_id.c_str());
    return QueueTodoCloudRequest(request);
}

void SendTodoCloudResult(TodoCloudResult* result)
{
    if (result == nullptr) {
        return;
    }
    if (g_todo_result_queue == nullptr || xQueueSend(g_todo_result_queue, &result, pdMS_TO_TICKS(100)) != pdTRUE) {
        g_todo_cloud_busy = false;
        delete result;
    }
}

void ApplyTodoList(wqn::UiState* state, wqn::WqnTodoListPage page)
{
    if (state == nullptr) {
        return;
    }
    const int selected_index = page.selected_index;
    state->todo.todos = std::move(page.todos);
    state->todo.loaded_once = true;
    state->todo.total_pending = page.total > 0 ? page.total : static_cast<int>(state->todo.todos.size());
    state->todo.previous_cursor = page.previous_cursor;
    state->todo.next_cursor = page.next_cursor;
    state->todo.has_earlier = page.has_earlier;
    state->todo.has_later = page.has_later;
    if (!state->todo.todos.empty()) {
        if (selected_index >= 0 && selected_index < static_cast<int>(state->todo.todos.size())) {
            state->todo.selected = static_cast<size_t>(selected_index);
        } else if (state->todo.selected >= state->todo.todos.size()) {
            state->todo.selected = state->todo.todos.size() - 1;
        }
    }
    state->todo.sync_status = wqn::TodoSyncStatus::kReady;
    state->todo.status_message.clear();
    wqn::ClampUiSelection(state);
}

bool ApplyTodoCloudResult(wqn::UiState* state, const TodoCloudResult& result)
{
    if (state == nullptr) {
        return false;
    }

    if (result.op == TodoCloudOp::kRefresh) {
        if (result.result == ESP_OK) {
            ApplyTodoList(state, result.page);
        } else if (result.auth_required) {
            state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
            state->todo.status_message = "Pair again";
            state->todo.total_pending = static_cast<int>(state->todo.todos.size());
        } else {
            state->todo.sync_status = wqn::TodoSyncStatus::kSyncFailed;
            state->todo.status_message = "Todo sync failed";
            state->todo.total_pending = static_cast<int>(state->todo.todos.size());
        }
        BuildHomeSummary(state);
        return true;
    }

    if (result.op == TodoCloudOp::kComplete) {
        if (result.result == ESP_OK) {
            const std::string completed_id =
                !result.todo.id.empty() ? result.todo.id : std::string(result.todo_id);
            auto it = std::find_if(
                state->todo.todos.begin(),
                state->todo.todos.end(),
                [&completed_id](const wqn::WqnTodoItem& item) { return item.id == completed_id; });
            if (it != state->todo.todos.end()) {
                state->todo.todos.erase(it);
            }
            if (state->todo.total_pending > 0) {
                --state->todo.total_pending;
            }
            state->todo.sync_status = wqn::TodoSyncStatus::kCompleted;
            state->todo.status_message = "Completed";
        } else if (result.auth_required) {
            state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
            state->todo.status_message = "Pair again";
        } else {
            state->todo.sync_status = wqn::TodoSyncStatus::kCompleteFailed;
            state->todo.status_message = "Complete failed";
        }
        wqn::ClampUiSelection(state);
        BuildHomeSummary(state);
        return true;
    }

    return false;
}

bool RefreshTodosFromCloud(wqn::UiState* state)
{
    if (state == nullptr) {
        return false;
    }

    {
        std::string token;
        if (!LoadValidTokenForTodo(&token)) {
            state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
            state->todo.status_message = "Pair again";
            state->todo.total_pending = static_cast<int>(state->todo.todos.size());
            return true;
        }

        if (!QueueTodoRefresh()) {
            if (IsTodoCloudBusy()) {
                state->todo.sync_status = wqn::TodoSyncStatus::kLoading;
                state->todo.status_message = "Todo syncing";
            } else {
                state->todo.sync_status = wqn::TodoSyncStatus::kSyncFailed;
                state->todo.status_message = "Todo queue failed";
            }
            return true;
        }

        state->todo.sync_status = wqn::TodoSyncStatus::kLoading;
        state->todo.status_message = "Todo syncing";
        return true;
    }
}

RefreshSchedule CompleteSelectedTodo(wqn::UiState* state)
{
    if (state == nullptr || state->screen != wqn::UiScreen::kTodo || state->todo.todos.empty()) {
        return RefreshSchedule::kNone;
    }
    wqn::ClampUiSelection(state);
    const size_t selected = state->todo.selected;
    if (selected >= state->todo.todos.size()) {
        return RefreshSchedule::kNone;
    }

    std::string token;
    if (!LoadValidTokenForTodo(&token)) {
        state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
        state->todo.status_message = "请重新配对";
        return RefreshSchedule::kCommit;
    }

    const std::string todo_id = state->todo.todos[selected].id;
    if (!QueueTodoComplete(todo_id)) {
        if (IsTodoCloudBusy()) {
            state->todo.sync_status = wqn::TodoSyncStatus::kCompleting;
            state->todo.status_message = "完成中";
        } else {
            state->todo.sync_status = wqn::TodoSyncStatus::kCompleteFailed;
            state->todo.status_message = "Todo 完成排队失败";
        }
        return RefreshSchedule::kCommit;
    }

    state->todo.sync_status = wqn::TodoSyncStatus::kCompleting;
    state->todo.status_message = "完成中";
    return RefreshSchedule::kCommit;
}

void TodoCloudTask(void*)
{
    ESP_LOGI(kTag, "Todo cloud task started");
    while (true) {
        TodoCloudRequest request;
        if (xQueueReceive(g_todo_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        TodoCloudResult* result = new (std::nothrow) TodoCloudResult();
        if (result == nullptr) {
            g_todo_cloud_busy = false;
            ESP_LOGW(kTag, "alloc Todo cloud result failed");
            continue;
        }
        result->op = request.op;
        std::snprintf(result->todo_id, sizeof(result->todo_id), "%s", request.todo_id);

        std::string token;
        if (!LoadValidTokenForTodo(&token)) {
            result->auth_required = true;
            result->result = ESP_ERR_INVALID_STATE;
            SendTodoCloudResult(result);
            continue;
        }

        if (request.op == TodoCloudOp::kRefresh) {
            wqn::WqnTodoTimelineRequest timeline_request;
            timeline_request.cursor = request.cursor;
            timeline_request.limit = 24;
            result->result = wqn::FetchTodoTimeline(token, timeline_request, &result->page);
        } else if (request.op == TodoCloudOp::kComplete) {
            result->result = wqn::CompleteTodo(token, request.todo_id, &result->todo);
        } else {
            result->result = ESP_ERR_INVALID_ARG;
        }

        if (result->result != ESP_OK) {
            std::string after_token;
            result->auth_required = !LoadValidTokenForTodo(&after_token);
        }
        SendTodoCloudResult(result);
    }
}

}  // namespace device_ui_internal
