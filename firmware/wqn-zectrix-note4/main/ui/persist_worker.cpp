#include "persist_worker.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "runtime/sleep_coordinator.h"
#include "ui_internal.h"  // NotifyUiTask

namespace device_ui_internal {
namespace {

constexpr char kTag[] = "wqn_persist";

// Pool depth == worker queue depth. A command only enters the queue while its
// slot is reserved (Filling -> Queued); with N slots and an N-deep queue, at
// most N-1 other slots can be Queued when we send, so the send always fits.
// This is the invariant that lets the submit path never block on the queue.
constexpr size_t kPoolDepth = 8;
// Keep the reviewed 8 KiB / priority 4 until HIL reports the real stack
// high-water mark (logged per command below). The worker itself only builds a
// CommitContext and blocks on the storage semaphore -- the heavy SPIFFS work
// runs on the 20 KiB storage task -- but error logging and the full commit
// wrappers still need headroom, and a clean compile does not prove 4 KiB safe.
constexpr uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 4;

// Slot lifecycle: Free -> Filling (reserve) -> Queued (enqueue) -> Running
// (worker popped) -> ResultPending (result published) -> Free (UI ack).
enum SlotState : uint32_t {
    kSlotFree = 0,
    kSlotFilling,
    kSlotQueued,
    kSlotRunning,
    kSlotResultPending,
};

struct PersistCommand {
    std::atomic<uint32_t> state{kSlotFree};
    PersistKind kind = PersistKind::kWordObservation;
    uint32_t operation_id = 0;
    esp_err_t result = ESP_FAIL;
    wqn::runtime::SleepLease lease;
    // Owned payload. Only the active kind's members are populated; vectors and
    // strings keep their heap data, so holding every kind's members inline
    // costs headers (a few hundred bytes) not the session arrays.
    wqn::DurableWordObservation word_obs;
    wqn::PersistedWordSession word_advanced;
    wqn::DurableNoteObservation note_obs;
    wqn::PersistedNoteSession note_advanced;
    wqn::DurableProblemObservation problem_obs;
    int settings_int = 0;
    std::string settings_str;
};

PersistCommand g_pool[kPoolDepth];
QueueHandle_t g_worker_queue = nullptr;  // carries slot indices (uint8_t)
TaskHandle_t g_worker_task = nullptr;
portMUX_TYPE g_start_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_starting = false;
std::atomic<uint32_t> g_next_operation_id{1};

// Per-kind busy: set when a reservation is taken, cleared only at UI ack (NOT
// at worker finish). One in-flight per kind + duplicate-Confirm guard.
std::atomic<bool> g_kind_busy[static_cast<size_t>(PersistKind::kCount)];

// Per-kind ACK mailbox. result/operation_id/slot are written before
// pending_generation (release) and read after it (acquire); the busy gate keeps
// one in-flight per kind, so the single slot is race-free behind that fence.
struct PersistMailbox {
    std::atomic<uint32_t> pending_generation{0};
    std::atomic<uint32_t> acked_generation{0};
    esp_err_t result = ESP_FAIL;
    uint32_t operation_id = 0;
    uint8_t slot_index = 0;
};
PersistMailbox g_mailbox[static_cast<size_t>(PersistKind::kCount)];
std::atomic<uint32_t> g_next_publish_generation{1};

bool ValidKind(PersistKind kind)
{
    return static_cast<uint8_t>(kind) < static_cast<uint8_t>(PersistKind::kCount);
}

size_t KindIndex(PersistKind kind) { return static_cast<size_t>(kind); }

uint32_t NextOperationId()
{
    uint32_t id = g_next_operation_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        id = g_next_operation_id.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
}

// Reserve the per-kind busy gate (one in-flight per kind). False if already busy.
bool ReserveKind(PersistKind kind)
{
    bool expected = false;
    return g_kind_busy[KindIndex(kind)].compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

void ReleaseKind(PersistKind kind)
{
    g_kind_busy[KindIndex(kind)].store(false, std::memory_order_release);
}

// Acquire a Free slot and move it to Filling. UI task only. Null when full.
PersistCommand* AcquireSlot(uint8_t* out_index)
{
    for (uint8_t i = 0; i < kPoolDepth; ++i) {
        uint32_t expected = kSlotFree;
        if (g_pool[i].state.compare_exchange_strong(
                expected, kSlotFilling,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            *out_index = i;
            return &g_pool[i];
        }
    }
    return nullptr;
}

// Returns the reserved command iff the ticket still owns a Filling slot of its
// exact kind/op id. Null (with a log) on a stale or corrupted ticket.
PersistCommand* ReservedCommand(const PersistTicket& ticket)
{
    if (!ticket.valid() || ticket.slot_index >= kPoolDepth || !ValidKind(ticket.kind)) {
        return nullptr;
    }
    PersistCommand& command = g_pool[ticket.slot_index];
    if (command.state.load(std::memory_order_acquire) != kSlotFilling ||
        command.kind != ticket.kind ||
        command.operation_id != ticket.operation_id) {
        return nullptr;
    }
    return &command;
}

// Enqueue a filled, reserved slot. Filling -> Queued via CAS so a corrupted or
// double-enqueued slot is caught rather than silently double-run.
void EnqueueReserved(PersistCommand& command, uint8_t slot_index, PersistKind kind)
{
    uint32_t expected = kSlotFilling;
    if (!command.state.compare_exchange_strong(
            expected, kSlotQueued,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        ESP_LOGE(kTag, "enqueue: slot %u not Filling (state=%lu)",
                 static_cast<unsigned>(slot_index),
                 static_cast<unsigned long>(expected));
        return;
    }
    if (xQueueSend(g_worker_queue, &slot_index, 0) != pdTRUE) {
        // Unreachable given the pool==queue-depth invariant; roll back rather
        // than leak a lease/slot/busy if that invariant is ever broken.
        command.lease.Reset();
        command.state.store(kSlotFree, std::memory_order_release);
        ReleaseKind(kind);
        ESP_LOGE(kTag, "persist enqueue failed despite reserved slot %u",
                 static_cast<unsigned>(slot_index));
    }
}

// Runs the owned storage transaction. WORKER TASK ONLY. Word/note/problem all
// call their foreground commit entries; settings kinds are wired in c4 once
// they gain a foreground/worker-dedicated storage entry, and no submit API
// enqueues them until then.
esp_err_t ExecutePersistCommand(PersistCommand& command)
{
    switch (command.kind) {
        case PersistKind::kWordObservation:
            return wqn::CommitWordObservation(command.word_obs, command.word_advanced);
        case PersistKind::kNoteObservation:
            return wqn::CommitNoteObservation(command.note_obs, command.note_advanced);
        case PersistKind::kProblemVerdict:
            return wqn::CommitProblemObservation(command.problem_obs);
        case PersistKind::kSettingsAutoSync:
        case PersistKind::kSettingsVolume:
        case PersistKind::kSettingsDefaultDeck:
        case PersistKind::kCount:
            return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_INVALID_ARG;
}

void PublishPersistResult(PersistCommand& command, uint8_t slot_index)
{
    PersistMailbox& box = g_mailbox[KindIndex(command.kind)];
    box.result = command.result;
    box.operation_id = command.operation_id;
    box.slot_index = slot_index;
    uint32_t generation =
        g_next_publish_generation.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0) {
        generation = g_next_publish_generation.fetch_add(1, std::memory_order_relaxed);
    }
    box.pending_generation.store(generation, std::memory_order_release);
    NotifyUiTask();
}

void PersistWorkerTask(void*)
{
    ESP_LOGI(kTag, "persist worker started: pool_depth=%u stack=%lu",
             static_cast<unsigned>(kPoolDepth),
             static_cast<unsigned long>(kTaskStackBytes));
    while (true) {
        uint8_t slot_index = 0;
        if (xQueueReceive(g_worker_queue, &slot_index, portMAX_DELAY) != pdTRUE ||
            slot_index >= kPoolDepth) {
            continue;
        }
        PersistCommand& command = g_pool[slot_index];
        uint32_t expected = kSlotQueued;
        if (!command.state.compare_exchange_strong(
                expected, kSlotRunning,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // A queue entry whose slot is not Queued means a duplicate index or
            // state corruption; skip it rather than run a half-owned command.
            ESP_LOGE(kTag, "worker: slot %u not Queued (state=%lu), skipping",
                     static_cast<unsigned>(slot_index),
                     static_cast<unsigned long>(expected));
            continue;
        }
        command.result = ExecutePersistCommand(command);
        // Storage has ended: the SleepLease lifetime ends with the write, NOT
        // with the UI ack. Release it here; the busy gate stays set until ack.
        command.lease.Reset();
        const UBaseType_t stack_free = uxTaskGetStackHighWaterMark(nullptr);
        ESP_LOGI(kTag, "persist done: kind=%u op=%lu result=%s stack_free=%u",
                 static_cast<unsigned>(command.kind),
                 static_cast<unsigned long>(command.operation_id),
                 esp_err_to_name(command.result),
                 static_cast<unsigned>(stack_free));
        command.state.store(kSlotResultPending, std::memory_order_release);
        PublishPersistResult(command, slot_index);
    }
}

}  // namespace

esp_err_t StartPersistWorker()
{
    taskENTER_CRITICAL(&g_start_lock);
    if (g_worker_task != nullptr) {
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_OK;
    }
    if (g_starting) {
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_starting = true;
    taskEXIT_CRITICAL(&g_start_lock);

    if (g_worker_queue == nullptr) {
        g_worker_queue = xQueueCreate(kPoolDepth, sizeof(uint8_t));
        if (g_worker_queue == nullptr) {
            taskENTER_CRITICAL(&g_start_lock);
            g_starting = false;
            taskEXIT_CRITICAL(&g_start_lock);
            return ESP_ERR_NO_MEM;
        }
    }
    TaskHandle_t created = nullptr;
    if (xTaskCreate(PersistWorkerTask, "wqn_persist", kTaskStackBytes, nullptr,
                    kTaskPriority, &created) != pdPASS) {
        taskENTER_CRITICAL(&g_start_lock);
        g_starting = false;
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&g_start_lock);
    g_worker_task = created;
    g_starting = false;
    taskEXIT_CRITICAL(&g_start_lock);
    return ESP_OK;
}

PersistTicket TryReservePersist(PersistKind kind)
{
    PersistTicket ticket;  // invalid by default
    if (!ValidKind(kind) || g_worker_task == nullptr) {
        return ticket;
    }
    if (!ReserveKind(kind)) {
        return ticket;  // one in-flight per kind / duplicate-Confirm
    }
    uint8_t slot_index = 0;
    PersistCommand* command = AcquireSlot(&slot_index);
    if (command == nullptr) {
        ReleaseKind(kind);
        return ticket;
    }
    const uint32_t operation_id = NextOperationId();
    wqn::runtime::SleepLease lease = wqn::runtime::SleepLease::TryAcquire(
        wqn::runtime::SleepBlocker::kStorage, "persist-worker", __FILE__, __LINE__);
    if (!lease) {
        command->state.store(kSlotFree, std::memory_order_release);
        ReleaseKind(kind);
        return ticket;
    }
    command->kind = kind;
    command->operation_id = operation_id;
    command->result = ESP_FAIL;
    command->lease = std::move(lease);
    // Slot stays Filling until EnqueueReserved* or CancelPersistReservation.
    ticket.slot_index = slot_index;
    ticket.operation_id = operation_id;
    ticket.kind = kind;
    return ticket;
}

void CancelPersistReservation(const PersistTicket& ticket)
{
    PersistCommand* command = ReservedCommand(ticket);
    if (command == nullptr) {
        if (ticket.valid()) {
            ESP_LOGW(kTag, "cancel: stale ticket slot=%u op=%lu",
                     static_cast<unsigned>(ticket.slot_index),
                     static_cast<unsigned long>(ticket.operation_id));
        }
        return;
    }
    command->lease.Reset();
    command->state.store(kSlotFree, std::memory_order_release);
    ReleaseKind(ticket.kind);
}

void EnqueueReservedWordObservation(
    const PersistTicket& ticket,
    wqn::DurableWordObservation observation,
    wqn::PersistedWordSession advanced_session)
{
    PersistCommand* command = ReservedCommand(ticket);
    if (command == nullptr || ticket.kind != PersistKind::kWordObservation) {
        ESP_LOGE(kTag, "enqueue word: stale/mismatched ticket");
        return;
    }
    command->word_obs = std::move(observation);
    command->word_advanced = std::move(advanced_session);
    EnqueueReserved(*command, ticket.slot_index, ticket.kind);
}

void EnqueueReservedNoteObservation(
    const PersistTicket& ticket,
    wqn::DurableNoteObservation observation,
    wqn::PersistedNoteSession advanced_session)
{
    PersistCommand* command = ReservedCommand(ticket);
    if (command == nullptr || ticket.kind != PersistKind::kNoteObservation) {
        ESP_LOGE(kTag, "enqueue note: stale/mismatched ticket");
        return;
    }
    command->note_obs = std::move(observation);
    command->note_advanced = std::move(advanced_session);
    EnqueueReserved(*command, ticket.slot_index, ticket.kind);
}

void EnqueueReservedProblemVerdict(
    const PersistTicket& ticket,
    wqn::DurableProblemObservation observation)
{
    PersistCommand* command = ReservedCommand(ticket);
    if (command == nullptr || ticket.kind != PersistKind::kProblemVerdict) {
        ESP_LOGE(kTag, "enqueue problem: stale/mismatched ticket");
        return;
    }
    command->problem_obs = std::move(observation);
    EnqueueReserved(*command, ticket.slot_index, ticket.kind);
}

bool TakePersistResultToApply(PersistKind kind, PersistResultReceipt* out)
{
    if (!ValidKind(kind)) {
        return false;
    }
    PersistMailbox& box = g_mailbox[KindIndex(kind)];
    const uint32_t pending = box.pending_generation.load(std::memory_order_acquire);
    if (pending == 0 ||
        pending == box.acked_generation.load(std::memory_order_relaxed)) {
        return false;
    }
    if (out != nullptr) {
        out->result = box.result;
        out->operation_id = box.operation_id;
        out->generation = pending;
    }
    return true;
}

bool AckPersistResult(PersistKind kind, uint32_t generation, uint32_t operation_id)
{
    if (!ValidKind(kind)) {
        return false;
    }
    PersistMailbox& box = g_mailbox[KindIndex(kind)];
    const uint32_t pending = box.pending_generation.load(std::memory_order_acquire);
    const uint32_t acked = box.acked_generation.load(std::memory_order_relaxed);
    if (pending == 0 || pending == acked) {
        ESP_LOGW(kTag, "ack ignored: no pending result (kind=%u gen=%lu)",
                 static_cast<unsigned>(kind), static_cast<unsigned long>(generation));
        return false;
    }
    if (pending != generation || box.operation_id != operation_id) {
        ESP_LOGW(kTag,
                 "ack ignored: stale (kind=%u pending=%lu req_gen=%lu box_op=%lu req_op=%lu)",
                 static_cast<unsigned>(kind), static_cast<unsigned long>(pending),
                 static_cast<unsigned long>(generation),
                 static_cast<unsigned long>(box.operation_id),
                 static_cast<unsigned long>(operation_id));
        return false;
    }
    const uint8_t slot_index = box.slot_index;
    if (slot_index >= kPoolDepth) {
        ESP_LOGE(kTag, "ack: bad slot index %u", static_cast<unsigned>(slot_index));
        return false;
    }
    PersistCommand& command = g_pool[slot_index];
    if (command.kind != kind || command.operation_id != operation_id) {
        ESP_LOGE(kTag, "ack: slot %u kind/op mismatch", static_cast<unsigned>(slot_index));
        return false;
    }
    // Claim the slot before touching the ack watermark / busy so a fresh submit
    // that observes "not busy" is guaranteed to find this slot free.
    uint32_t expected = kSlotResultPending;
    if (!command.state.compare_exchange_strong(
            expected, kSlotFree,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        ESP_LOGE(kTag, "ack: slot %u not ResultPending (state=%lu)",
                 static_cast<unsigned>(slot_index),
                 static_cast<unsigned long>(expected));
        return false;
    }
    box.acked_generation.store(generation, std::memory_order_release);
    ReleaseKind(kind);
    return true;
}

bool IsPersistKindBusy(PersistKind kind)
{
    if (!ValidKind(kind)) {
        return false;
    }
    return g_kind_busy[KindIndex(kind)].load(std::memory_order_acquire);
}

bool IsAnyPersistBusy()
{
    for (size_t i = 0; i < static_cast<size_t>(PersistKind::kCount); ++i) {
        if (g_kind_busy[i].load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

}  // namespace device_ui_internal
