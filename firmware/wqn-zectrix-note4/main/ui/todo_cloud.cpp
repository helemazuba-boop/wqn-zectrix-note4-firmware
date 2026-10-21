// Todo cloud task: timeline refresh, complete a single todo, result dispatch.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "esp_log.h"
#include "runtime/sleep_coordinator.h"
#include "wqn_api.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

static std::atomic<bool> g_todo_cloud_busy{false};
wqn::runtime::SleepLease g_todo_sleep_lease;
TodoCloudResult g_todo_result_slot;
uint32_t g_todo_result_generation = 0;

void FinishTodoCloudRequest()
{
    // Release the lease before publishing idle. A producer that observes
    // idle can then safely acquire a fresh lease without racing this reset.
    g_todo_sleep_lease.Reset();
    ClearCloudDomainBusyWatch(CloudDomain::kTodo);
    g_todo_cloud_busy.store(false, std::memory_order_release);
}

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
    return g_todo_cloud_busy.load(std::memory_order_acquire);
}

bool QueueTodoCloudRequest(const TodoCloudRequest& request)
{
    bool expected = false;
    if (!g_todo_cloud_busy.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    wqn::runtime::SleepLease lease =
        wqn::runtime::SleepLease::TryAcquire(
            wqn::runtime::SleepBlocker::kTodoCloud, "todo-cloud", __FILE__, __LINE__);
    if (!lease) {
        g_todo_cloud_busy.store(false, std::memory_order_release);
        return false;
    }
    g_todo_sleep_lease = std::move(lease);
    CloudJob job;
    job.domain = CloudDomain::kTodo;
    job.todo = request;
    if (!EnqueueCloudJob(job)) {
        FinishTodoCloudRequest();
        return false;
    }
    return true;
}

bool QueueTodoRefresh()
{
    TodoCloudRequest request;
    request.op = TodoCloudOp::kRefresh;
    return QueueTodoCloudRequest(request);
}

bool QueueTodoRefreshForRevision(uint64_t target_revision)
{
    if (target_revision == 0) {
        return false;
    }
    TodoCloudRequest request;
    request.op = TodoCloudOp::kRefresh;
    request.content_target_revision = target_revision;
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

bool QueueTodoComplete(const std::string& todo_id, const std::string& cursor)
{
    if (todo_id.empty()) {
        return false;
    }
    TodoCloudRequest request;
    request.op = TodoCloudOp::kComplete;
    std::snprintf(request.cursor, sizeof(request.cursor), "%s", cursor.c_str());
    std::snprintf(request.todo_id, sizeof(request.todo_id), "%s", todo_id.c_str());
    return QueueTodoCloudRequest(request);
}

const TodoCloudResult* PeekTodoCloudResult(uint32_t generation)
{
    if (generation == 0 || generation != g_todo_result_generation) {
        return nullptr;
    }
    return &g_todo_result_slot;
}

void SendTodoCloudResult()
{
    CloudResultReady ready;
    ready.domain = CloudDomain::kTodo;
    ready.generation = g_todo_result_generation;
    // [ack-mailbox] Publish never fails; busy is held until the UI applies and
    // acks, so the terminal result cannot be silently dropped.
    PublishCloudResult(CloudDomain::kTodo, g_todo_result_generation);
    (void)ready;
}

void ApplyTodoList(wqn::UiState* state, wqn::WqnTodoListPage page, bool cursor_request)
{
    if (state == nullptr) {
        return;
    }
    const std::string selected_id = !cursor_request && state->todo.selected < state->todo.todos.size()
        ? state->todo.todos[state->todo.selected].id : std::string();
    int selected_index = page.selected_index;
    if (!selected_id.empty()) {
        for (size_t i = 0; i < page.todos.size(); ++i) {
            if (page.todos[i].id == selected_id) { selected_index = static_cast<int>(i); break; }
        }
    }
    if (cursor_request) state->todo.window_start = 0;
    state->todo.todos = std::move(page.todos);
    state->todo.loaded_once = true;
    state->todo.paging_stale = false;
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
    state->todo.completion_task_id.clear();
    state->todo.completion_title.clear();
    state->todo.status_message.clear();
    wqn::ClampUiSelection(state);
}

// True when a refreshed page would change nothing the user can see: same
// items in the same order (id + rendered fields), same totals and paging.
// Refreshing an unchanged timeline then only needs to clear the "syncing"
// status line instead of flashing a full commit refresh.
bool TodoPageMatchesState(const wqn::UiState& state, const wqn::WqnTodoListPage& page)
{
    const auto& todos = state.todo.todos;
    if (page.todos.size() != todos.size()) return false;
    for (size_t i = 0; i < todos.size(); ++i) {
        const wqn::WqnTodoItem& a = todos[i];
        const wqn::WqnTodoItem& b = page.todos[i];
        if (a.id != b.id || a.title != b.title || a.status != b.status ||
            a.priority != b.priority || a.due_at != b.due_at ||
            a.updated_at != b.updated_at || a.subject_name != b.subject_name ||
            a.completed_at != b.completed_at) {
            return false;
        }
    }
    const int page_total =
        page.total > 0 ? page.total : static_cast<int>(page.todos.size());
    return page_total == state.todo.total_pending &&
        page.previous_cursor == state.todo.previous_cursor &&
        page.next_cursor == state.todo.next_cursor &&
        page.has_earlier == state.todo.has_earlier &&
        page.has_later == state.todo.has_later;
}

bool ApplyTodoCloudResult(
    wqn::UiState* state, const TodoCloudResult& result, bool* content_changed)
{
    if (content_changed != nullptr) {
        *content_changed = true;
    }
    if (state == nullptr) {
        return false;
    }

    if (result.op == TodoCloudOp::kRefresh) {
        if (result.result == ESP_OK) {
            state->todo.current_cursor = result.cursor;
            const bool was_stale = state->todo.paging_stale;
            if (!result.cursor_request && state->todo.loaded_once &&
                TodoPageMatchesState(*state, result.page)) {
                // Nothing the user can see changed; just retire the "syncing"
                // hint. The dispatcher downgrades this to a partial repaint
                // instead of the old unconditional full-refresh flash.
                const bool hint_was_visible =
                    state->todo.sync_status != wqn::TodoSyncStatus::kReady ||
                    !state->todo.status_message.empty();
                state->todo.sync_status = wqn::TodoSyncStatus::kReady;
                state->todo.paging_stale = false;
                state->todo.status_message.clear();
                if (content_changed != nullptr) {
                    *content_changed = false;
                }
                return hint_was_visible;
            }
            ApplyTodoList(state, result.page, result.cursor_request && !was_stale);
        } else if (result.auth_required) {
            state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
            state->todo.status_message = "Pair again";
        } else {
            state->todo.sync_status = wqn::TodoSyncStatus::kSyncFailed;
            state->todo.status_message = "Todo sync failed";
        }
        BuildHomeSummary(state);
        return true;
    }

    if (result.op == TodoCloudOp::kComplete) {
        if (state->todo.sync_status != wqn::TodoSyncStatus::kCompleting ||
            state->todo.completion_task_id != result.todo_id) {
            return false;
        }
        if (result.result == ESP_OK && !result.todo.id.empty() && result.todo.id != result.todo_id) {
            state->todo.sync_status = wqn::TodoSyncStatus::kCompleteFailed;
            state->todo.status_message = "完成结果不匹配，确认重试";
            return true;
        }
        if (result.result == ESP_OK) {
            // The accepted request owns the operation. A malformed response ID
            // must not erase some other task from the visible page.
            const std::string completed_id(result.todo_id);
            const size_t selected_before = state->todo.selected;
            auto it = std::find_if(
                state->todo.todos.begin(),
                state->todo.todos.end(),
                [&completed_id](const wqn::WqnTodoItem& item) { return item.id == completed_id; });
            if (it != state->todo.todos.end()) {
                state->todo.completion_title = it->title;
                state->todo.todos.erase(it);
                if (state->todo.total_pending > 0) --state->todo.total_pending;
            }
            const std::string completed_title = state->todo.completion_title;
            const bool had_next = selected_before < state->todo.todos.size();
            wqn::ClampUiSelection(state);
            if (result.has_refreshed_page) {
                // Deleting a task shifts the server's offset-based cursors.
                // Install fresh neighbours before permitting a remote page jump.
                ApplyTodoList(state, result.page, false);
                if (!had_next && !state->todo.todos.empty()) {
                    state->todo.selected = std::min(selected_before, state->todo.todos.size() - 1);
                }
            } else {
                state->todo.paging_stale = true;
            }
            state->todo.completion_task_id = completed_id;
            state->todo.completion_title = completed_title;
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
    if (IsTodoCloudBusy()) return false;  // Keep the accepted operation's status.
    if (state == nullptr) {
        return false;
    }

    {
        std::string token;
        if (!LoadValidTokenForTodo(&token)) {
            state->todo.sync_status = wqn::TodoSyncStatus::kAuthRequired;
            state->todo.status_message = "Pair again";
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
    if (IsTodoCloudBusy()) return RefreshSchedule::kNone;
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
    if (!QueueTodoComplete(todo_id, state->todo.current_cursor)) {
        if (IsTodoCloudBusy()) return RefreshSchedule::kNone;
        state->todo.completion_task_id = todo_id;
        state->todo.completion_title = state->todo.todos[selected].title;
        state->todo.sync_status = wqn::TodoSyncStatus::kCompleteFailed;
        state->todo.status_message = "完成失败，确认重试";
        return RefreshSchedule::kSelection;
    }

    state->todo.completion_task_id = todo_id;
    state->todo.completion_title = state->todo.todos[selected].title;
    state->todo.sync_status = wqn::TodoSyncStatus::kCompleting;
    state->todo.status_message = "完成中";
    return RefreshSchedule::kSelection;
}

// Called after global-navigation and raw/held-key guards have consumed input.
RefreshSchedule HandleTodoInput(wqn::UiState* state, wqn::UiInput input)
{
    if (state == nullptr || state->screen != wqn::UiScreen::kTodo) return RefreshSchedule::kNone;
    wqn::ClampUiSelection(state);
    if (input == wqn::UiInput::kConfirm) return CompleteSelectedTodo(state);
    if (input != wqn::UiInput::kUp && input != wqn::UiInput::kDown) return RefreshSchedule::kNone;
    auto& todo = state->todo;
    if (todo.sync_status == wqn::TodoSyncStatus::kCompleting) return RefreshSchedule::kNone;
    const bool up = input == wqn::UiInput::kUp;
    const bool boundary = up ? todo.selected == 0
        : todo.todos.empty() || todo.selected + 1 >= todo.todos.size();
    const bool has_page = up ? todo.has_earlier : todo.has_later;
    const std::string& cursor = up ? todo.previous_cursor : todo.next_cursor;
    if (boundary && has_page && !cursor.empty()) {
        if (IsTodoCloudBusy()) return RefreshSchedule::kNone;
        // If the post-completion reload failed, repair the current window
        // first. Never consume an offset cursor computed before the deletion.
        const bool queued = todo.paging_stale
            ? (todo.current_cursor.empty() ? QueueTodoRefresh() : QueueTodoRefreshCursor(todo.current_cursor))
            : QueueTodoRefreshCursor(cursor);
        if (queued) {
            todo.sync_status = wqn::TodoSyncStatus::kLoading;
            todo.status_message = "正在加载待办";
        } else {
            todo.sync_status = wqn::TodoSyncStatus::kSyncFailed;
            todo.status_message = "加载失败，请重试";
        }
        return RefreshSchedule::kSelection;
    }
    const size_t previous = todo.selected;
    if (up && todo.selected > 0) --todo.selected;
    if (!up && todo.selected + 1 < todo.todos.size()) ++todo.selected;
    wqn::ClampUiSelection(state);
    if (previous == todo.selected) return RefreshSchedule::kNone;
    if (todo.sync_status == wqn::TodoSyncStatus::kCompleteFailed ||
        todo.sync_status == wqn::TodoSyncStatus::kCompleted) {
        todo.sync_status = wqn::TodoSyncStatus::kReady;
        todo.status_message.clear();
        todo.completion_task_id.clear();
        todo.completion_title.clear();
    }
    return RefreshSchedule::kSelection;
}

void ExecuteTodoCloudRequest(const TodoCloudRequest& request)
{
    g_todo_result_slot = TodoCloudResult{};
    ++g_todo_result_generation;
    if (g_todo_result_generation == 0) {
        ++g_todo_result_generation;
    }
    TodoCloudResult& result = g_todo_result_slot;
    result.op = request.op;
    result.cursor_request = request.op == TodoCloudOp::kRefresh && request.cursor[0] != '\0';
    std::snprintf(result.cursor, sizeof(result.cursor), "%s", request.cursor);
    result.content_target_revision = request.content_target_revision;
    std::snprintf(result.todo_id, sizeof(result.todo_id), "%s", request.todo_id);

    std::string token;
    if (!LoadValidTokenForTodo(&token)) {
        result.auth_required = true;
        result.result = ESP_ERR_INVALID_STATE;
        SendTodoCloudResult();
        return;
    }

    if (request.op == TodoCloudOp::kRefresh) {
        wqn::WqnTodoTimelineRequest timeline_request;
        timeline_request.cursor = request.cursor;
        timeline_request.limit = 24;
        result.result = wqn::FetchTodoTimeline(token, timeline_request, &result.page);
    } else if (request.op == TodoCloudOp::kComplete) {
        result.result = wqn::CompleteTodo(token, request.todo_id, &result.todo);
        if (result.result == ESP_OK && result.todo.id == request.todo_id) {
            wqn::WqnTodoTimelineRequest timeline_request;
            timeline_request.cursor = request.cursor;
            timeline_request.limit = 24;
            result.has_refreshed_page =
                wqn::FetchTodoTimeline(token, timeline_request, &result.page) == ESP_OK;
        }
    } else {
        result.result = ESP_ERR_INVALID_ARG;
    }

    if (result.result != ESP_OK) {
        std::string after_token;
        result.auth_required = !LoadValidTokenForTodo(&after_token);
    }
    SendTodoCloudResult();
}

}  // namespace device_ui_internal
