// Unified cloud runner: one executor for the Todo/Word/Note cloud domains.
// Replaces the three identical per-domain task/queue stacks. Two lanes keep
// bulk transfers (pack sync: multi-MB downloads, SHA checks, index rebuilds)
// out of the way of interactive work the user is actively waiting on (session
// start, candidate pages, note images, todo refresh). Per-domain busy flags,
// sleep leases, result slots and generations keep their pre-runner semantics.

#include "ui_internal.h"

#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"

namespace device_ui_internal {

namespace {

constexpr char kTag[] = "wqn_cloud";

QueueHandle_t g_lane_queue[2] = {nullptr, nullptr};
TaskHandle_t g_lane_task[2] = {nullptr, nullptr};

// [hang-fix] Busy-watch. Each domain admits one in-flight request gated by
// its busy CAS; if an Execute* path ever fails to reach Send*Result, or a
// network call overruns every inner timeout, that domain stays busy forever
// and all of its UI entry points silently go dead. Stage one is detection:
// log loudly past a hard per-lane budget so field logs pin the stuck domain.
// Forced recovery is deliberately not attempted -- resetting the busy flag
// under a still-running job would race the shared result slot.
std::atomic<int64_t> g_domain_busy_warn_deadline_us[4] = {};
constexpr int64_t kInteractiveBusyWarnUs = 60LL * 1000 * 1000;
constexpr int64_t kBulkBusyWarnUs = 600LL * 1000 * 1000;

// [transfer-progress] Mailbox storage (see ui_internal.h for the contract).
// generation is written last on Begin / first on read so a consumer that
// matched the generation is looking at bytes from the right transfer; torn
// done/total pairs only wobble the quantized bucket by one step.
std::atomic<uint8_t> g_transfer_kind{0};
std::atomic<uint32_t> g_transfer_generation{0};
std::atomic<uint32_t> g_transfer_done_bytes{0};
std::atomic<uint32_t> g_transfer_total_bytes{0};

const char* CloudDomainName(CloudDomain domain)
{
    switch (domain) {
        case CloudDomain::kTodo:
            return "todo";
        case CloudDomain::kWord:
            return "word";
        case CloudDomain::kNote:
            return "note";
        case CloudDomain::kProblem:
            return "problem";
    }
    return "unknown";
}

bool CloudDomainBusy(CloudDomain domain)
{
    switch (domain) {
        case CloudDomain::kTodo:
            return IsTodoCloudBusy();
        case CloudDomain::kWord:
            return IsWordCloudBusy();
        case CloudDomain::kNote:
            return IsNoteCloudBusy();
        case CloudDomain::kProblem:
            return IsProblemCloudBusy();
    }
    return false;
}

CloudLane LaneForJob(const CloudJob& job)
{
    switch (job.domain) {
        case CloudDomain::kTodo:
            // Timeline refresh / complete: the user is looking at the page.
            return CloudLane::kInteractive;
        case CloudDomain::kWord:
            return job.word.op == WordCloudOp::kPackSync ? CloudLane::kBulk
                                                         : CloudLane::kInteractive;
        case CloudDomain::kNote:
            return job.note.op == NoteCloudOp::kPackSync ? CloudLane::kBulk
                                                         : CloudLane::kInteractive;
        case CloudDomain::kProblem:
            // Pack sync moves multi-MB content; image fetches and the verdict
            // commit are things the user is actively waiting on.
            return job.problem.op == ProblemCloudOp::kPackSync
                ? CloudLane::kBulk
                : CloudLane::kInteractive;
    }
    return CloudLane::kBulk;
}

void CloudLaneTask(void* arg)
{
    QueueHandle_t queue = static_cast<QueueHandle_t>(arg);
    while (true) {
        CloudJob job;
        if (xQueueReceive(queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (job.domain) {
            case CloudDomain::kTodo:
                ExecuteTodoCloudRequest(job.todo);
                break;
            case CloudDomain::kWord:
                ExecuteWordCloudRequest(job.word);
                break;
            case CloudDomain::kNote:
                ExecuteNoteCloudRequest(job.note);
                break;
            case CloudDomain::kProblem:
                ExecuteProblemCloudRequest(job.problem);
                break;
        }
    }
}

}  // namespace

namespace {
// [input-capture] UI task handle for prompt result/sync consumption. Cloud
// results and sync events used to only sit in a queue until the UI's next
// idle poll (up to 500 ms); notifying wakes it within one scheduler hop.
TaskHandle_t g_ui_task_to_notify = nullptr;
}  // namespace

void SetUiTaskToNotify(TaskHandle_t ui_task)
{
    g_ui_task_to_notify = ui_task;
}

void NotifyUiTask()
{
    if (g_ui_task_to_notify != nullptr) {
        xTaskNotifyGive(g_ui_task_to_notify);
    }
}

namespace {
// [ack-mailbox] Per-domain terminal-result mailbox. The producer writes the
// domain's result slot then publishes pending_generation here; the UI scans
// every domain, applies the matching slot, and writes acked_generation.
// Only after the ACK does the domain clear busy (Finish*) and reuse its slot,
// so a terminal result can never be silently lost the way the old
// xQueueSend-then-Finish-on-failure branch could when the depth-4 queue was
// full. generation is monotonic per domain (0 = none); pending!=acked means
// "unapplied result waiting".
struct DomainResultMailbox {
    std::atomic<uint32_t> pending_generation{0};
    std::atomic<uint32_t> acked_generation{0};
};
DomainResultMailbox g_result_mailbox[4];
}  // namespace

void PublishCloudResult(CloudDomain domain, uint32_t generation)
{
    g_result_mailbox[static_cast<size_t>(domain)].pending_generation.store(
        generation, std::memory_order_release);
    NotifyUiTask();
}

bool TakeCloudResultToApply(CloudDomain domain, uint32_t* generation)
{
    DomainResultMailbox& box = g_result_mailbox[static_cast<size_t>(domain)];
    const uint32_t pending = box.pending_generation.load(std::memory_order_acquire);
    if (pending == 0 ||
        pending == box.acked_generation.load(std::memory_order_relaxed)) {
        return false;
    }
    if (generation != nullptr) {
        *generation = pending;
    }
    return true;
}

void AckCloudResult(CloudDomain domain, uint32_t generation)
{
    g_result_mailbox[static_cast<size_t>(domain)].acked_generation.store(
        generation, std::memory_order_release);
}

esp_err_t StartCloudRunner()
{
    static constexpr const char* kLaneNames[2] = {"wqn_cloud_int", "wqn_cloud_blk"};
    for (int lane = 0; lane < 2; ++lane) {
        if (g_lane_queue[lane] == nullptr) {
            g_lane_queue[lane] = xQueueCreate(4, sizeof(CloudJob));
            if (g_lane_queue[lane] == nullptr) {
                return ESP_ERR_NO_MEM;
            }
        }
        if (g_lane_task[lane] == nullptr) {
            // Same stack/priority the three dedicated cloud tasks used.
            const BaseType_t created = xTaskCreate(
                CloudLaneTask, kLaneNames[lane], 8192, g_lane_queue[lane], 3,
                &g_lane_task[lane]);
            if (created != pdPASS) {
                g_lane_task[lane] = nullptr;
                return ESP_ERR_NO_MEM;
            }
        }
    }
    return ESP_OK;
}

bool EnqueueCloudJob(const CloudJob& job)
{
    const CloudLane lane_kind = LaneForJob(job);
    const int lane = static_cast<int>(lane_kind);
    if (g_lane_queue[lane] == nullptr) {
        return false;
    }
    if (xQueueSend(g_lane_queue[lane], &job, 0) != pdTRUE) {
        return false;
    }
    // Arm the busy-watch only for jobs that actually entered a lane; rejected
    // jobs roll their domain's busy flag back via Finish*CloudRequest.
    const int64_t budget_us = lane_kind == CloudLane::kBulk
        ? kBulkBusyWarnUs
        : kInteractiveBusyWarnUs;
    g_domain_busy_warn_deadline_us[static_cast<size_t>(job.domain)].store(
        esp_timer_get_time() + budget_us, std::memory_order_release);
    return true;
}

void ClearCloudDomainBusyWatch(CloudDomain domain)
{
    g_domain_busy_warn_deadline_us[static_cast<size_t>(domain)].store(
        0, std::memory_order_release);
}

void BeginCloudTransferProgress(CloudTransferKind kind, uint32_t generation)
{
    g_transfer_done_bytes.store(0, std::memory_order_relaxed);
    g_transfer_total_bytes.store(0, std::memory_order_relaxed);
    g_transfer_kind.store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
    g_transfer_generation.store(generation, std::memory_order_release);
}

void ReportCloudTransferBytes(uint32_t done_bytes, uint32_t total_bytes)
{
    g_transfer_done_bytes.store(done_bytes, std::memory_order_relaxed);
    g_transfer_total_bytes.store(total_bytes, std::memory_order_relaxed);
}

void EndCloudTransferProgress()
{
    g_transfer_generation.store(0, std::memory_order_release);
    g_transfer_kind.store(0, std::memory_order_relaxed);
    g_transfer_done_bytes.store(0, std::memory_order_relaxed);
    g_transfer_total_bytes.store(0, std::memory_order_relaxed);
}

CloudTransferSnapshot ReadCloudTransferProgress()
{
    CloudTransferSnapshot snapshot;
    snapshot.generation = g_transfer_generation.load(std::memory_order_acquire);
    snapshot.kind =
        static_cast<CloudTransferKind>(g_transfer_kind.load(std::memory_order_relaxed));
    snapshot.done_bytes = g_transfer_done_bytes.load(std::memory_order_relaxed);
    snapshot.total_bytes = g_transfer_total_bytes.load(std::memory_order_relaxed);
    return snapshot;
}

void WarnStuckCloudDomains()
{
    const int64_t now_us = esp_timer_get_time();
    for (size_t i = 0; i < 4; ++i) {
        const int64_t deadline = g_domain_busy_warn_deadline_us[i].load(
            std::memory_order_acquire);
        if (deadline <= 0 || now_us < deadline) {
            continue;
        }
        const CloudDomain domain = static_cast<CloudDomain>(i);
        // Warn once per stuck episode; re-armed by the next request.
        g_domain_busy_warn_deadline_us[i].store(0, std::memory_order_release);
        if (CloudDomainBusy(domain)) {
            ESP_LOGE(kTag,
                     "cloud domain '%s' busy past hard budget; request likely stuck "
                     "(runner lane blocked or Execute path never reached Send)",
                     CloudDomainName(domain));
        }
    }
}

}  // namespace device_ui_internal
