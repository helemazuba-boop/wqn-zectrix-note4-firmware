#include "runtime/sleep_coordinator.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {

constexpr char kTag[] = "wqn_sleep";
constexpr size_t kBlockerCount = static_cast<size_t>(wqn::runtime::SleepBlocker::kCount);

std::array<std::atomic<uint32_t>, kBlockerCount> g_blocker_counts{};
std::atomic<uint32_t> g_total_blockers{0};
std::atomic<bool> g_quiescing{false};
std::atomic<uint32_t> g_quiesce_generation{0};

constexpr size_t kMaxLeaseRecords = 32;

struct LeaseRecord {
    bool active = false;
    uint32_t lease_id = 0;
    wqn::runtime::SleepBlocker blocker = wqn::runtime::SleepBlocker::kDisplay;
    const char* holder = "unspecified";
    const char* file = "unknown";
    int line = 0;
    int64_t acquired_us = 0;
    int64_t last_warning_us = 0;
};

struct LeaseWarning {
    bool valid = false;
    wqn::runtime::SleepBlocker blocker = wqn::runtime::SleepBlocker::kDisplay;
    const char* holder = "unspecified";
    const char* file = "unknown";
    int line = 0;
    int64_t held_us = 0;
};

std::array<LeaseRecord, kMaxLeaseRecords> g_lease_records{};
portMUX_TYPE g_lease_lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_next_lease_id = 1;
esp_pm_lock_handle_t g_no_light_sleep_lock = nullptr;
esp_pm_lock_handle_t g_cpu_freq_max_lock = nullptr;

size_t BlockerIndex(wqn::runtime::SleepBlocker blocker)
{
    return static_cast<size_t>(blocker);
}

void ReleaseBlocker(wqn::runtime::SleepBlocker blocker, uint8_t slot, uint32_t lease_id)
{
    const size_t index = BlockerIndex(blocker);
    if (index >= kBlockerCount || slot >= kMaxLeaseRecords) {
        return;
    }

    bool released = false;
    taskENTER_CRITICAL(&g_lease_lock);
    LeaseRecord& record = g_lease_records[slot];
    if (record.active && record.lease_id == lease_id && record.blocker == blocker) {
        record = {};
        released = true;
    }
    taskEXIT_CRITICAL(&g_lease_lock);
    if (!released) {
        ESP_LOGE(kTag, "stale sleep lease release: blocker=%s slot=%u id=%u",
                 wqn::runtime::SleepBlockerName(blocker),
                 static_cast<unsigned>(slot),
                 static_cast<unsigned>(lease_id));
        return;
    }

    const uint32_t previous = g_blocker_counts[index].fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0) {
        g_blocker_counts[index].store(0, std::memory_order_release);
        ESP_LOGE(kTag, "sleep blocker underflow: %s", wqn::runtime::SleepBlockerName(blocker));
        return;
    }
    const uint32_t total_previous =
        g_total_blockers.fetch_sub(1, std::memory_order_acq_rel);
    if (total_previous == 0) {
        g_total_blockers.store(0, std::memory_order_release);
        ESP_LOGE(kTag, "total sleep blocker underflow");
        return;
    }
#if CONFIG_PM_ENABLE
    if (total_previous == 1 && g_no_light_sleep_lock != nullptr) {
        const esp_err_t result = esp_pm_lock_release(g_no_light_sleep_lock);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "release NO_LIGHT_SLEEP lock failed: %s", esp_err_to_name(result));
        }
    }
#endif
}

}  // namespace

namespace wqn::runtime {

esp_err_t InitSleepCoordinator()
{
#if CONFIG_PM_ENABLE
    if (g_no_light_sleep_lock == nullptr) {
        const esp_err_t result = esp_pm_lock_create(
            ESP_PM_NO_LIGHT_SLEEP, 0, "wqn_sleep_lease", &g_no_light_sleep_lock);
        if (result != ESP_OK) {
            return result;
        }
    }
    if (g_cpu_freq_max_lock == nullptr) {
        return esp_pm_lock_create(
            ESP_PM_CPU_FREQ_MAX, 0, "wqn_cpu_work", &g_cpu_freq_max_lock);
    }
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

CpuPerformanceLease::~CpuPerformanceLease()
{
    Reset();
}

CpuPerformanceLease::CpuPerformanceLease(CpuPerformanceLease&& other) noexcept
    : active_(std::exchange(other.active_, false))
{
}

CpuPerformanceLease& CpuPerformanceLease::operator=(
    CpuPerformanceLease&& other) noexcept
{
    if (this != &other) {
        Reset();
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

CpuPerformanceLease CpuPerformanceLease::TryAcquire()
{
#if CONFIG_PM_ENABLE
    if (g_cpu_freq_max_lock == nullptr) {
        ESP_LOGE(kTag, "CPU performance lock is not initialized");
        return {};
    }
    const esp_err_t result = esp_pm_lock_acquire(g_cpu_freq_max_lock);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "acquire CPU_FREQ_MAX lock failed: %s", esp_err_to_name(result));
        return {};
    }
#endif
    return CpuPerformanceLease(true);
}

void CpuPerformanceLease::Reset()
{
    if (!active_) {
        return;
    }
    active_ = false;
#if CONFIG_PM_ENABLE
    if (g_cpu_freq_max_lock != nullptr) {
        const esp_err_t result = esp_pm_lock_release(g_cpu_freq_max_lock);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "release CPU_FREQ_MAX lock failed: %s", esp_err_to_name(result));
        }
    }
#endif
}

const char* SleepBlockerName(SleepBlocker blocker)
{
    switch (blocker) {
        case SleepBlocker::kDisplay:
            return "display";
        case SleepBlocker::kTodoCloud:
            return "todo-cloud";
        case SleepBlocker::kWordCloud:
            return "word-cloud";
        case SleepBlocker::kNoteCloud:
            return "note-cloud";
        case SleepBlocker::kOnlineSync:
            return "online-sync";
        case SleepBlocker::kProvisioning:
            return "provisioning";
        case SleepBlocker::kAudio:
            return "audio";
        case SleepBlocker::kAiSession:
            return "ai-session";
        case SleepBlocker::kFlashSession:
            return "flash-session";
        case SleepBlocker::kStorage:
            return "storage";
        case SleepBlocker::kConnectivity:
            return "connectivity";
        case SleepBlocker::kUsbPower:
            return "usb-power";
        case SleepBlocker::kCount:
        default:
            return "unknown";
    }
}

SleepLease::~SleepLease()
{
    Reset();
}

SleepLease::SleepLease(SleepLease&& other) noexcept
    : blocker_(other.blocker_),
      slot_(std::exchange(other.slot_, UINT8_MAX)),
      lease_id_(std::exchange(other.lease_id_, 0)),
      active_(std::exchange(other.active_, false))
{
}

SleepLease& SleepLease::operator=(SleepLease&& other) noexcept
{
    if (this != &other) {
        Reset();
        blocker_ = other.blocker_;
        slot_ = std::exchange(other.slot_, UINT8_MAX);
        lease_id_ = std::exchange(other.lease_id_, 0);
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

SleepLease SleepLease::TryAcquire(
    SleepBlocker blocker,
    const char* holder,
    const char* file,
    int line)
{
    const size_t index = BlockerIndex(blocker);
    if (index >= kBlockerCount || g_quiescing.load(std::memory_order_acquire)) {
        return {};
    }

    uint8_t slot = UINT8_MAX;
    uint32_t lease_id = 0;
    bool acquire_pm_lock = false;
    const int64_t acquired_us = esp_timer_get_time();
    taskENTER_CRITICAL(&g_lease_lock);
    if (!g_quiescing.load(std::memory_order_acquire)) {
        for (size_t i = 0; i < g_lease_records.size(); ++i) {
            if (g_lease_records[i].active) {
                continue;
            }
            lease_id = g_next_lease_id++;
            if (lease_id == 0) {
                lease_id = g_next_lease_id++;
            }
            LeaseRecord& record = g_lease_records[i];
            record.active = true;
            record.lease_id = lease_id;
            record.blocker = blocker;
            record.holder = holder == nullptr ? "unspecified" : holder;
            record.file = file == nullptr ? "unknown" : file;
            record.line = line;
            record.acquired_us = acquired_us;
            record.last_warning_us = 0;
            slot = static_cast<uint8_t>(i);
            g_blocker_counts[index].fetch_add(1, std::memory_order_acq_rel);
            const uint32_t total_previous =
                g_total_blockers.fetch_add(1, std::memory_order_acq_rel);
#if CONFIG_PM_ENABLE
            acquire_pm_lock = total_previous == 0 && g_no_light_sleep_lock != nullptr;
#endif
            break;
        }
    }
    taskEXIT_CRITICAL(&g_lease_lock);
    if (slot == UINT8_MAX) {
        if (!g_quiescing.load(std::memory_order_acquire)) {
            ESP_LOGE(kTag, "sleep lease registry exhausted: blocker=%s holder=%s",
                     SleepBlockerName(blocker), holder == nullptr ? "unspecified" : holder);
        }
        return {};
    }
#if CONFIG_PM_ENABLE
    if (acquire_pm_lock) {
        const esp_err_t result = esp_pm_lock_acquire(g_no_light_sleep_lock);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "acquire NO_LIGHT_SLEEP lock failed: %s",
                     esp_err_to_name(result));
        }
    }
#endif
    return SleepLease(blocker, slot, lease_id);
}

void SleepLease::Reset()
{
    if (!active_) {
        return;
    }
    active_ = false;
    ReleaseBlocker(blocker_, slot_, lease_id_);
    slot_ = UINT8_MAX;
    lease_id_ = 0;
}

bool TryBeginSleepQuiesce(uint32_t generation)
{
    if (generation == 0) {
        return false;
    }
    bool started = false;
    taskENTER_CRITICAL(&g_lease_lock);
    if (!g_quiescing.load(std::memory_order_acquire)) {
        g_quiescing.store(true, std::memory_order_release);
        g_quiesce_generation.store(generation, std::memory_order_release);
        started = g_total_blockers.load(std::memory_order_acquire) == 0;
    }
    if (!started && g_quiesce_generation.load(std::memory_order_acquire) == generation) {
        g_quiesce_generation.store(0, std::memory_order_release);
        g_quiescing.store(false, std::memory_order_release);
    }
    taskEXIT_CRITICAL(&g_lease_lock);
    return started;
}

bool BeginEmergencySleepQuiesce(uint32_t generation)
{
    if (generation == 0) {
        return false;
    }
    bool started = false;
    taskENTER_CRITICAL(&g_lease_lock);
    if (!g_quiescing.load(std::memory_order_acquire)) {
        g_quiescing.store(true, std::memory_order_release);
        g_quiesce_generation.store(generation, std::memory_order_release);
        started = true;
    } else {
        started = g_quiesce_generation.load(std::memory_order_acquire) == generation;
    }
    taskEXIT_CRITICAL(&g_lease_lock);
    return started;
}

void CancelSleepQuiesce(uint32_t generation)
{
    bool cancelled = false;
    uint32_t active_generation = 0;
    taskENTER_CRITICAL(&g_lease_lock);
    active_generation = g_quiesce_generation.load(std::memory_order_acquire);
    if (generation != 0 && active_generation == generation) {
        g_quiesce_generation.store(0, std::memory_order_release);
        g_quiescing.store(false, std::memory_order_release);
        cancelled = true;
    }
    taskEXIT_CRITICAL(&g_lease_lock);
    if (!cancelled) {
        ESP_LOGW(kTag, "ignored stale quiesce rollback: requested=%u active=%u",
                 static_cast<unsigned>(generation),
                 static_cast<unsigned>(active_generation));
    }
}

bool IsSleepQuiescing()
{
    return g_quiescing.load(std::memory_order_acquire);
}

uint32_t CurrentSleepGeneration()
{
    return g_quiesce_generation.load(std::memory_order_acquire);
}

bool HasActiveSleepBlockers()
{
    return g_total_blockers.load(std::memory_order_acquire) != 0;
}

uint32_t ActiveSleepBlockerCount(SleepBlocker blocker)
{
    const size_t index = BlockerIndex(blocker);
    if (index >= kBlockerCount) {
        return 0;
    }
    return g_blocker_counts[index].load(std::memory_order_acquire);
}

void LogLongHeldSleepLeases(int64_t now_us, int64_t warning_after_us)
{
    if (warning_after_us <= 0) {
        return;
    }

    std::array<LeaseWarning, kMaxLeaseRecords> warnings{};
    taskENTER_CRITICAL(&g_lease_lock);
    for (size_t i = 0; i < g_lease_records.size(); ++i) {
        LeaseRecord& record = g_lease_records[i];
        // External USB power is a policy lease, not a work transaction. It is
        // expected to remain held for the full duration of a development or
        // charging session, so it must not generate a false stuck-work alarm.
        if (!record.active || record.blocker == SleepBlocker::kUsbPower ||
            now_us - record.acquired_us < warning_after_us ||
            (record.last_warning_us != 0 && now_us - record.last_warning_us < warning_after_us)) {
            continue;
        }
        record.last_warning_us = now_us;
        warnings[i].valid = true;
        warnings[i].blocker = record.blocker;
        warnings[i].holder = record.holder;
        warnings[i].file = record.file;
        warnings[i].line = record.line;
        warnings[i].held_us = now_us - record.acquired_us;
    }
    taskEXIT_CRITICAL(&g_lease_lock);

    for (const LeaseWarning& warning : warnings) {
        if (!warning.valid) {
            continue;
        }
        ESP_LOGW(kTag,
                 "long-held sleep lease: blocker=%s holder=%s held_ms=%lld at=%s:%d",
                 SleepBlockerName(warning.blocker),
                 warning.holder,
                 static_cast<long long>(warning.held_us / 1000),
                 warning.file,
                 warning.line);
    }
}

}  // namespace wqn::runtime
