// Real RunPrompt/lease/error/command policy; only RTOS, resources and unrelated
// UI/history side effects are ports. The network client itself remains real.
namespace owner_probe {
constexpr char kTag[] = "owner-test";
@@COMMAND@@
wqn::AgentSessionState g_state;
WorkerCommand g_command = WorkerCommand::kNone;
bool g_run_failed = false, g_observing = false, g_history_refresh = false;
bool g_upstream_run_live = false, g_observed_run_live = false;
std::string g_run_session_id, g_run_prompt, g_run_request_id;
uint8_t g_run_detail = 2;
std::atomic<bool> g_interrupt_requested{false}, g_interrupt_delivered{false};
std::atomic<bool> g_switch_requested{false}, g_switch_delivered{false};
wqn::OpenCodeOutboundQueue g_outbound_replies;
struct Lease { bool held = false; void Reset() { held = false; } };
Lease g_agent_sleep_lease, g_connectivity_demand;
int g_lock = 1, lock_depth = 0;
constexpr int portMAX_DELAY = -1;
void xSemaphoreTake(int, int) { if (lock_depth++ != 0) std::abort(); }
void xSemaphoreGive(int) { if (--lock_depth != 0) std::abort(); }
esp_err_t LoadToken(std::string* token) { *token = "synthetic-device-token"; return ESP_OK; }
esp_err_t AcquireNetwork() { g_connectivity_demand.held = true; return ESP_OK; }
esp_err_t AcquireAgentLeaseLocked() { g_agent_sleep_lease.held = true; return ESP_OK; }
void MarkChangedLocked() {}
void ClearAllAsksLocked() { g_state.pending_permission_id.clear(); g_state.pending_question_id.clear(); }
void OnOpenCodeReplyFailed(const wqn::OpenCodeOutboundReply&, esp_err_t, void*) {}
void OnOpenCodeEvent(const wqn::OpenCodeEvent&, void*) {}
bool FinishSwitchedStreamLocked() { return false; }
void ArmAgentFollowLocked() {}
void ResetAgentHistoryTurnLocked() {}
void ChainWorkerCommandLocked(WorkerCommand command) { g_command = command; }
@@OWNER@@

void Prepare()
{
    g_state = {};
    g_state.current_session_id = "ses_parent";
    g_state.session_locked = true;
    g_state.stream_active = true;
    g_state.run_live = true;
    g_state.ui.phase = wqn::AiFeaturePhase::kRunning;
    g_run_session_id = "ses_parent";
    g_run_prompt = "synthetic prompt";
    g_run_request_id = request_id;
    g_command = WorkerCommand::kRunPrompt;
    g_observing = g_history_refresh = g_upstream_run_live = g_observed_run_live = g_run_failed = false;
    g_interrupt_requested = g_interrupt_delivered = g_switch_requested = g_switch_delivered = false;
    g_agent_sleep_lease.held = g_connectivity_demand.held = true;
}
void Test()
{
    Reset(); Prepare();
    RunPrompt(); FinishCommand(WorkerCommand::kRunPrompt);
    Check(clock_ms == first_ms && g_state.ui.phase == wqn::AiFeaturePhase::kError,
          "actual session owner surfaces unconfirmed run timeout");
    Check(!g_state.stream_active && !g_state.run_live && g_command == WorkerCommand::kNone,
          "actual owner releases stream and worker slot");
    Check(!g_connectivity_demand.held && !g_agent_sleep_lease.held && lock_depth == 0,
          "actual lease criterion releases both resource ports on lost own run");
    Check(g_state.current_session_id == "ses_parent" && g_state.session_locked && g_run_request_id == request_id,
          "actual failure tail retains session and logical request id");
    OneRunOnly();
    Reset(); Prepare(); scenario.status = 409;
    RunPrompt(); FinishCommand(WorkerCommand::kRunPrompt);
    Check(g_command == WorkerCommand::kObserveSession && g_run_session_id == "ses_parent" && g_run_request_id.empty(),
          "actual 409 rejection retires rejected submission and chains same-session observe without replay");
    Check(g_upstream_run_live && g_agent_sleep_lease.held && g_connectivity_demand.held,
          "positive in-flight evidence keeps existing observe ownership policy");
    OneRunOnly();
    Reset(); Prepare();
    Event(1000, "agent.status", "{\"status\":\"idle\"}");
    RunPrompt(); FinishCommand(WorkerCommand::kRunPrompt);
    Check(g_run_request_id.empty() && g_command == WorkerCommand::kNone,
          "known terminal outcome still retires logical request identity");
}
} // namespace owner_probe
