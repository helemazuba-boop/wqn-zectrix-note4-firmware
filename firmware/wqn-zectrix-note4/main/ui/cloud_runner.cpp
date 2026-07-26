// Unified cloud runner: one executor for the Todo/Word/Note cloud domains.
// Replaces the three identical per-domain task/queue stacks. Two lanes keep
// bulk transfers (pack sync: multi-MB downloads, SHA checks, index rebuilds)
// out of the way of interactive work the user is actively waiting on (session
// start, candidate pages, note images, todo refresh). Per-domain busy flags,
// sleep leases, result slots and generations keep their pre-runner semantics.

#include "ui_internal.h"

#include "esp_log.h"

namespace device_ui_internal {

namespace {

constexpr char kRunnerTag[] = "wqn_cloud";

QueueHandle_t g_lane_queue[2] = {nullptr, nullptr};
TaskHandle_t g_lane_task[2] = {nullptr, nullptr};

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
            break;
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
                ESP_LOGW(kRunnerTag, "problem domain has no executor yet");
                break;
        }
    }
}

}  // namespace

QueueHandle_t g_cloud_result_queue = nullptr;

esp_err_t StartCloudRunner()
{
    if (g_cloud_result_queue == nullptr) {
        // One slot per domain: the per-domain busy CAS guarantees at most one
        // outstanding result each.
        g_cloud_result_queue = xQueueCreate(4, sizeof(CloudResultReady));
        if (g_cloud_result_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
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
    const int lane = static_cast<int>(LaneForJob(job));
    if (g_lane_queue[lane] == nullptr) {
        return false;
    }
    return xQueueSend(g_lane_queue[lane], &job, 0) == pdTRUE;
}

}  // namespace device_ui_internal
