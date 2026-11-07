#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include "button_input.h"
#include "services/storage_recovery_service.h"
#include "ui/storage_recovery_ui.h"
#include "ui/ui_internal.h"
using namespace wqn;
using namespace wqn::runtime;
using namespace wqn::services;
using namespace wqn::display;
using namespace device_ui_internal;
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL recovery-ui:%d: %s\n", __LINE__, #x); } } while (false)
struct Restart {};
struct Exhausted {};
enum Command { Down, Up, Confirm, Press, Hold };
int scenario = 0, attempts = 0, operation_count = 0, inspect_count = 0, reset_count = 0, steps = 0;
int64_t now = 100;
uint32_t sequence = 0;
DisplayRevision outstanding = 0, received = 0;
UiFrame frame;
std::deque<ButtonEvent> buttons;
std::deque<DisplayResult> results;
std::deque<Command> commands;
StorageRecoveryResult result;
bool operation_pending = false, operation_ready = false, display_pending = false;
bool warning_presented = false, stale_warning = false, stale_outcome = false;
bool saw_uncertain = false, saw_diagnostics = false, paused_probe = false;
bool startup_failed = false, export_started = false, export_cancelled = false;
unsigned export_progress_polls = 0;
esp_err_t input_error = ESP_OK, button_task_error = ESP_OK;
unsigned restarts = 0;
DisplayRecoveryPolicy recovery;
std::vector<TickType_t> waits;
bool Contains(const char* text) {
    for (const auto& line : frame.lines) if (line.text.find(text) != std::string::npos) return true;
    return false;
}
void Event(ButtonId id, ButtonEventType type, int64_t timestamp, int64_t duration = 0) {
    buttons.push_back({id, type, duration, false, timestamp, ++sequence});
}
TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<void*>(1); }
int64_t esp_timer_get_time() { return now * 1000; }
void esp_restart() {
    CHECK(!operation_pending && outstanding == 0);
    ++restarts; throw Restart{};
}
namespace wqn {
esp_err_t InitButtonInput() { return input_error; }
esp_err_t StartButtonInputTask(TaskHandle_t target) {
    CHECK(target == xTaskGetCurrentTaskHandle()); return button_task_error;
}
bool ReceiveButtonEvent(ButtonEvent* out) {
    if (buttons.empty()) return false;
    *out = buttons.front(); buttons.pop_front(); return true;
}
}
namespace wqn::services {
esp_err_t StartStorageRecoveryService(TaskHandle_t target) {
    CHECK(target == xTaskGetCurrentTaskHandle());
    if (scenario == 5 && !startup_failed) { startup_failed = true; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
bool SubmitStorageRecoveryExport(StorageRecoveryExportKind kind, bool confirmed, uint32_t* id) {
    CHECK((scenario == 6 || scenario == 7) && !operation_pending && confirmed);
    CHECK(kind == (scenario == 6 ? StorageRecoveryExportKind::kRaw : StorageRecoveryExportKind::kLogical));
    export_started = true; operation_pending = true; operation_ready = false;
    result = {}; *id = ++operation_count; result.operation_id = *id;
    result.operation = kind == StorageRecoveryExportKind::kRaw ? StorageRecoveryOperation::kExportRaw : StorageRecoveryOperation::kExportLogical;
    result.error = scenario == 6 ? ESP_OK : ESP_ERR_INVALID_STATE;
    result.exported.commit_acknowledged = scenario == 6; result.exported.source_complete = scenario == 6;
    return true;
}
bool CancelStorageRecoveryExport(uint32_t id) {
    CHECK(scenario == 7 && export_started && operation_pending && result.operation_id == id);
    export_cancelled = true; return true;
}
bool ReadStorageRecoveryExportProgress(uint32_t id, uint32_t* bytes) {
    if (!export_started || !operation_pending || id != result.operation_id) return false;
    ++export_progress_polls; *bytes = 1024; return true;
}
bool SubmitStorageRecoveryInspect(uint32_t* id) {
    CHECK(!operation_pending); ++inspect_count;
    operation_pending = true; operation_ready = false;
    result = {}; *id = ++operation_count; result.operation_id = *id;
    result.operation = StorageRecoveryOperation::kInspect; result.error = ESP_OK;
    result.snapshot.error = ESP_OK; result.snapshot.generation_found = true;
    result.snapshot.observed_generation = scenario == 3 ? 4 : 2;
    result.snapshot.reset_policy = scenario == 3 ? StorageRecoveryResetPolicy::kNewerFirmwareRequired
                                                : StorageRecoveryResetPolicy::kAllowed;
    return true;
}
bool SubmitStorageRecoveryReset(const StorageRecoverySnapshot& expected, uint32_t* id) {
    CHECK(!operation_pending && warning_presented && scenario != 3);
    CHECK(expected.observed_generation == 2);
    ++reset_count; operation_pending = true; operation_ready = false;
    result = {}; *id = ++operation_count; result.operation_id = *id;
    result.operation = StorageRecoveryOperation::kAuthorizeReset;
    result.error = scenario == 2 ? ESP_FAIL : ESP_OK;
    result.reset = {result.error, scenario == 2 ? StorageRecoveryResetOutcome::kUncertain
                                               : StorageRecoveryResetOutcome::kRecorded};
    return true;
}
bool ReadStorageRecoveryResult(StorageRecoveryResult* out) {
    if (!operation_pending || !operation_ready) return false;
    *out = result; return true;
}
bool AcknowledgeStorageRecoveryResult(uint32_t id) {
    CHECK(operation_pending && operation_ready && result.operation_id == id);
    operation_pending = operation_ready = false; return true;
}
}
namespace device_ui_internal {
void* g_display_result_queue = reinterpret_cast<void*>(2);
std::string FrameSignature(const UiFrame& input) {
    std::string signature;
    for (const auto& line : input.lines) signature += line.text;
    return signature;
}
DisplaySubmission RequestEpdUiRefresh(const UiFrame& input, const std::string& signature,
    DisplayRevision revision, RefreshSchedule, WaveformRequirement waveform) {
    CHECK(outstanding == 0 && revision != 0 && waveform == WaveformRequirement::kFull);
    CHECK(signature == FrameSignature(input) && input.prefer_full_refresh);
    CHECK(recovery.TryBegin(now));
    frame = input; outstanding = revision; display_pending = true;
    return {true, revision, waveform, 0};
}
void AcknowledgeDisplayResult(DisplayRevision revision) {
    CHECK(received == revision && outstanding == revision);
    outstanding = received = 0;
}
DisplayRecoveryStatus CopyDisplayRecoveryStatus() { return recovery.status(); }
bool RequestDisplayRecovery(int64_t input) { return recovery.RequestProbe(input, now); }
}
int xQueueReceive(void* queue, DisplayResult* out, TickType_t wait) {
    CHECK(queue == g_display_result_queue && wait == 0);
    if (results.empty()) return pdFALSE;
    *out = results.front(); results.pop_front(); received = out->revision; return pdTRUE;
}
uint32_t ulTaskNotifyTake(int clear, TickType_t delay) {
    CHECK(clear == pdTRUE);
    if (++steps > 150) throw Exhausted{};
    waits.push_back(delay);
    if (display_pending) {
        CHECK(delay == pdMS_TO_TICKS(50));
        display_pending = false;
        const bool warning = Contains("确认清除全部");
        const bool outcome = Contains("重置请求已保存") || Contains("重置请求可能已保存");
        // Inject an event produced before the success ACK. It must not consent
        // merely because its consumption occurs after that ACK.
        if (outcome && !stale_outcome) {
            Event(ButtonId::kConfirm, ButtonEventType::kShortPress, now);
            stale_outcome = true;
        }
        now += 50;
        const bool fail = (scenario == 1 && warning && attempts++ == 0) ||
                          (scenario == 4 && attempts++ < 4);
        if (fail) {
            recovery.Failed(now);
            results.push_back({outstanding, DisplayStatus::kFailed, 0, 0, ESP_FAIL});
        } else {
            recovery.Presented();
            results.push_back({outstanding, DisplayStatus::kPresented, outstanding, 0, ESP_OK});
            if (warning) warning_presented = true;
            if (Contains("重置请求可能已保存")) saw_uncertain = true;
            if (Contains("诊断状态已输出串口")) saw_diagnostics = true;
        }
        return 1;
    }
    if (operation_pending && !operation_ready) {
        if (scenario == 7 && export_started && !export_cancelled) {
            now += 10; Event(ButtonId::kUp, ButtonEventType::kPress, now); return 1;
        }
        now += 10; operation_ready = true; return 1;
    }
    if (recovery.status().paused) {
        CHECK(delay == portMAX_DELAY);
        CHECK(!paused_probe); paused_probe = true;
        now += 1100; Event(ButtonId::kConfirm, ButtonEventType::kPress, now);
        return 1;
    }
    if (recovery.status().HasPendingRetry()) {
        CHECK(delay != portMAX_DELAY);
        if (scenario == 1 && !stale_warning) {
            CHECK(!warning_presented && reset_count == 0);
            Event(ButtonId::kConfirm, ButtonEventType::kPress, now + 1);
            now += 2501;
            Event(ButtonId::kConfirm, ButtonEventType::kRelease, now, 2500);
            stale_warning = true;
        } else now += static_cast<int64_t>(delay) * 1000 / configTICK_RATE_HZ + 1;
        return 1;
    }
    CHECK(delay == portMAX_DELAY); // Idle has no permanent 50 ms polling.
    if (commands.empty()) throw Exhausted{};
    now += 20;
    const auto command = commands.front(); commands.pop_front();
    if (command == Down || command == Up)
        Event(command == Down ? ButtonId::kDownPower : ButtonId::kUp, ButtonEventType::kShortPress, now);
    if (command == Confirm) Event(ButtonId::kConfirm, ButtonEventType::kShortPress, now);
    if (command == Press) Event(ButtonId::kConfirm, ButtonEventType::kPress, now);
    if (command == Hold) {
        Event(ButtonId::kConfirm, ButtonEventType::kPress, now);
        now += 2500; Event(ButtonId::kConfirm, ButtonEventType::kRelease, now, 2500);
    }
    return 1;
}
int main() {
    for (scenario = 0; scenario < 8; ++scenario) {
        now = 100; sequence = 0; operation_count = inspect_count = reset_count = steps = attempts = 0;
        outstanding = received = 0; frame.lines.clear(); buttons.clear(); results.clear(); waits.clear();
        operation_pending = operation_ready = display_pending = false;
        warning_presented = stale_warning = stale_outcome = saw_uncertain = saw_diagnostics = paused_probe = false;
        startup_failed = export_started = export_cancelled = false; export_progress_polls = 0;
        recovery = {};
        if (scenario == 0) commands = {Confirm, Confirm, Down, Confirm, Press, Confirm};
        else if (scenario == 3) commands = {Down, Down, Down, Confirm, Up, Up, Confirm, Press, Confirm};
        else if (scenario == 4) commands = {Down, Confirm, Press, Confirm};
        else if (scenario == 5) commands = {Confirm, Confirm, Confirm, Down, Confirm, Press, Confirm};
        else if (scenario == 6) commands = {Down, Down, Confirm, Down, Press, Confirm, Hold, Confirm, Up, Confirm, Press, Confirm};
        else if (scenario == 7) commands = {Down, Down, Confirm, Press, Confirm, Hold, Confirm, Up, Confirm, Press, Confirm};
        else commands = {Down, Down, Down, Confirm, Hold, Press, Confirm};
        const unsigned before = restarts;
        try { RunStorageRecoveryUi(ESP_FAIL); CHECK(false); }
        catch (const Restart&) {}
        catch (const Exhausted&) { CHECK(false); }
        CHECK(restarts == before+1 && commands.empty());
        CHECK(reset_count == ((scenario == 1 || scenario == 2) ? 1 : 0));
        CHECK(scenario != 0 || (inspect_count == 2 && saw_diagnostics));
        CHECK(scenario != 1 || (stale_warning && warning_presented && stale_outcome));
        CHECK(scenario != 2 || (saw_uncertain && stale_outcome));
        CHECK(scenario != 4 || paused_probe);
        CHECK(scenario < 6 || (export_started && export_progress_polls != 0));
        CHECK(scenario != 7 || export_cancelled);
        CHECK(scenario != 5 || (startup_failed && saw_diagnostics && inspect_count == 1));
    }
    const auto inspected = inspect_count;
    input_error = ESP_FAIL; CHECK(RunStorageRecoveryUi(ESP_FAIL) == ESP_FAIL);
    input_error = ESP_OK; button_task_error = ESP_ERR_NO_MEM;
    CHECK(RunStorageRecoveryUi(ESP_FAIL) == ESP_ERR_NO_MEM);
    CHECK(inspect_count == inspected && !operation_pending);
    std::printf("Storage recovery UI: %u checks, %u failures (NOT HIL)\n", checks, failures);
    return failures ? 1 : 0;
}
