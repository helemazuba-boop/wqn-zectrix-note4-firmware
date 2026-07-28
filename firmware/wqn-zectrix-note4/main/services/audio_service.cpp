#include "services/audio_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_manager.h"
#include "runtime/sleep_coordinator.h"

namespace {

constexpr char kTag[] = "audio_service";
constexpr gpio_num_t kCodecPower = GPIO_NUM_42;
constexpr gpio_num_t kAmplifierEnable = GPIO_NUM_46;
constexpr gpio_num_t kI2sMclk = GPIO_NUM_14;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_15;
constexpr gpio_num_t kI2sWordSelect = GPIO_NUM_38;
constexpr gpio_num_t kI2sDataIn = GPIO_NUM_16;
constexpr gpio_num_t kI2sDataOut = GPIO_NUM_45;
constexpr uint8_t kEs8311Address = 0x18;
constexpr UBaseType_t kCommandQueueDepth = 12;
constexpr UBaseType_t kReplyQueueDepth = 8;
constexpr uint32_t kTaskStackBytes = 4096;
constexpr UBaseType_t kTaskPriority = 7;
constexpr TickType_t kCallTimeout = pdMS_TO_TICKS(5000);
constexpr int64_t kCallTimeoutUs = 5 * 1000 * 1000;
constexpr int64_t kEmergencyForceCallBudgetUs = 500 * 1000;
constexpr size_t kMaxCodecHandles = 2;
constexpr size_t kMaxChannelHandles = 2;
// The board currently relies on the ESP32-S3's internal I2C pull-ups. They
// are much weaker than the recommended external 2-5 kOhm pull-ups, so the
// ES8311's two-byte register transactions are marginal at 100 kHz even when
// the address-only probe succeeds. Keep the shared RTC/PMU devices at their
// normal speed, but run codec transactions more conservatively.
constexpr uint32_t kCodecI2cClockHz = 50000;
constexpr int kCodecI2cMaxAttempts = 5;
constexpr TickType_t kCodecI2cRetryDelay = pdMS_TO_TICKS(5);
constexpr TickType_t kCodecPowerOffSettle = pdMS_TO_TICKS(20);
constexpr TickType_t kCodecPowerOnWarmup = pdMS_TO_TICKS(250);

enum class CommandType : uint8_t {
    kBegin,
    kEnd,
    kSetAmplifier,
    kRecoverCodec,
    kPrepareSleep,
    kRollbackSleep,
};

struct AudioCommand {
    CommandType type = CommandType::kBegin;
    uint32_t request_id = 0;
    uint32_t session_id = 0;
    wqn::services::AudioActivity activity =
        wqn::services::AudioActivity::kCapture;
    bool enabled = false;
    wqn::power::PrepareSleepCommand sleep = {};
};

struct AudioReply {
    uint32_t request_id = 0;
    uint32_t session_id = 0;
    esp_err_t result = ESP_FAIL;
};

StaticQueue_t g_command_queue_storage;
uint8_t g_command_queue_buffer[
    kCommandQueueDepth * sizeof(AudioCommand)] = {};
QueueHandle_t g_command_queue = nullptr;

StaticQueue_t g_reply_queue_storage;
uint8_t g_reply_queue_buffer[kReplyQueueDepth * sizeof(AudioReply)] = {};
QueueHandle_t g_reply_queue = nullptr;

StaticSemaphore_t g_call_mutex_storage;
SemaphoreHandle_t g_call_mutex = nullptr;
TaskHandle_t g_task = nullptr;
portMUX_TYPE g_start_lock = portMUX_INITIALIZER_UNLOCKED;
bool g_starting = false;
uint32_t g_next_request_id = 1;
uint32_t g_next_session_id = 1;

std::atomic<wqn::services::AudioState> g_state{
    wqn::services::AudioState::kIdle};
std::atomic<uint32_t> g_session_id{0};
std::atomic<bool> g_codec_powered{true};
std::atomic<bool> g_amplifier_enabled{false};
std::atomic<wqn::services::AudioActivity> g_activity{
    wqn::services::AudioActivity::kCapture};
std::atomic<bool> g_accept_operations{false};
std::atomic<uint32_t> g_inflight_operations{0};
wqn::runtime::SleepLease g_audio_lease;
uint32_t g_prepared_generation = 0;
wqn::services::AudioState g_prepared_previous_state =
    wqn::services::AudioState::kIdle;

struct ResourceSlot {
    void* handle = nullptr;
    uint32_t owner_session_id = 0;
    uint16_t inflight = 0;
    bool closing = false;
};

ResourceSlot g_codec_slots[kMaxCodecHandles] = {};
ResourceSlot g_channel_slots[kMaxChannelHandles] = {};
portMUX_TYPE g_resource_lock = portMUX_INITIALIZER_UNLOCKED;

wqn::services::AudioState StateForActivity(
    wqn::services::AudioActivity activity);

bool SessionMatches(const wqn::services::AudioSession& session)
{
    return session.id != 0 &&
        session.id == g_session_id.load(std::memory_order_acquire) &&
        session.activity == g_activity.load(std::memory_order_acquire) &&
        g_state.load(std::memory_order_acquire) ==
            StateForActivity(session.activity);
}

bool IsValidActivity(wqn::services::AudioActivity activity)
{
    switch (activity) {
        case wqn::services::AudioActivity::kCapture:
        case wqn::services::AudioActivity::kPlayback:
        case wqn::services::AudioActivity::kFlash:
        case wqn::services::AudioActivity::kSelfTest:
            return true;
        default:
            return false;
    }
}

bool IsRetryableCodecI2cError(esp_err_t result)
{
    // ESP-IDF 5.5 reports a synchronous I2C NACK as INVALID_STATE; newer
    // releases use INVALID_RESPONSE. TIMEOUT is also recoverable because the
    // driver resets its hardware FSM before the next synchronous transaction.
    return result == ESP_ERR_INVALID_STATE ||
        result == ESP_ERR_INVALID_RESPONSE ||
        result == ESP_ERR_TIMEOUT;
}

class DriverOperation {
public:
    enum class Kind : uint8_t { kSession, kCodec, kChannel };

    DriverOperation(
        const wqn::services::AudioSession& session,
        Kind kind = Kind::kSession,
        void* handle = nullptr)
        : kind_(kind)
    {
        taskENTER_CRITICAL(&g_resource_lock);
        const bool accepts_operations =
            g_accept_operations.load(std::memory_order_acquire);
        const uint32_t active_session =
            g_session_id.load(std::memory_order_acquire);
        const wqn::services::AudioState active_state =
            g_state.load(std::memory_order_acquire);
        if (!accepts_operations || !SessionMatches(session)) {
            taskEXIT_CRITICAL(&g_resource_lock);
            ESP_LOGE(kTag,
                     "driver op rejected: kind=%u session=%lu active_session=%lu activity=%s state=%s accept=%d",
                     static_cast<unsigned>(kind_),
                     static_cast<unsigned long>(session.id),
                     static_cast<unsigned long>(active_session),
                     wqn::services::AudioActivityName(session.activity),
                     wqn::services::AudioStateName(active_state),
                     accepts_operations ? 1 : 0);
            return;
        }
        if (kind_ != Kind::kSession) {
            ResourceSlot* slots = kind_ == Kind::kCodec
                ? g_codec_slots : g_channel_slots;
            const size_t count = kind_ == Kind::kCodec
                ? kMaxCodecHandles : kMaxChannelHandles;
            for (size_t i = 0; i < count; ++i) {
                if (slots[i].handle == handle &&
                    slots[i].owner_session_id == session.id &&
                    !slots[i].closing) {
                    slot_ = &slots[i];
                    ++slot_->inflight;
                    break;
                }
            }
            if (slot_ == nullptr) {
                taskEXIT_CRITICAL(&g_resource_lock);
                ESP_LOGE(kTag,
                         "driver op rejected: unregistered handle kind=%u session=%lu handle=%p",
                         static_cast<unsigned>(kind_),
                         static_cast<unsigned long>(session.id), handle);
                return;
            }
        }
        g_inflight_operations.fetch_add(1, std::memory_order_acq_rel);
        entered_ = true;
        taskEXIT_CRITICAL(&g_resource_lock);
    }

    ~DriverOperation()
    {
        if (!entered_) {
            return;
        }
        taskENTER_CRITICAL(&g_resource_lock);
        if (slot_ != nullptr && slot_->inflight > 0) {
            --slot_->inflight;
        }
        g_inflight_operations.fetch_sub(1, std::memory_order_acq_rel);
        taskEXIT_CRITICAL(&g_resource_lock);
    }

    explicit operator bool() const { return entered_; }

    DriverOperation(const DriverOperation&) = delete;
    DriverOperation& operator=(const DriverOperation&) = delete;

private:
    Kind kind_ = Kind::kSession;
    ResourceSlot* slot_ = nullptr;
    bool entered_ = false;
};

esp_err_t RegisterResource(
    ResourceSlot* slots,
    size_t count,
    const wqn::services::AudioSession& session,
    void* handle)
{
    if (handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_ERR_NO_MEM;
    taskENTER_CRITICAL(&g_resource_lock);
    if (g_accept_operations.load(std::memory_order_acquire) &&
        SessionMatches(session)) {
        for (size_t i = 0; i < count; ++i) {
            if (slots[i].handle == nullptr) {
                slots[i].handle = handle;
                slots[i].owner_session_id = session.id;
                slots[i].inflight = 0;
                slots[i].closing = false;
                result = ESP_OK;
                break;
            }
        }
    } else {
        result = ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&g_resource_lock);
    if (result != ESP_OK) {
        ESP_LOGE(kTag,
                 "resource register failed: session=%lu handle=%p result=%s (%d) accept=%d state=%s",
                 static_cast<unsigned long>(session.id), handle,
                 esp_err_to_name(result), static_cast<int>(result),
                 g_accept_operations.load(std::memory_order_acquire) ? 1 : 0,
                 wqn::services::AudioStateName(
                     g_state.load(std::memory_order_acquire)));
    }
    return result;
}

esp_err_t BeginCloseResource(
    ResourceSlot* slots,
    size_t count,
    const wqn::services::AudioSession& session,
    void* handle,
    ResourceSlot** closing_slot)
{
    if (handle == nullptr || closing_slot == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *closing_slot = nullptr;
    taskENTER_CRITICAL(&g_resource_lock);
    if (g_accept_operations.load(std::memory_order_acquire) &&
        SessionMatches(session)) {
        for (size_t i = 0; i < count; ++i) {
            if (slots[i].handle == handle &&
                slots[i].owner_session_id == session.id &&
                !slots[i].closing) {
                slots[i].closing = true;
                *closing_slot = &slots[i];
                g_inflight_operations.fetch_add(
                    1, std::memory_order_acq_rel);
                break;
            }
        }
    }
    taskEXIT_CRITICAL(&g_resource_lock);
    return *closing_slot == nullptr ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t WaitForResourceDrain(ResourceSlot* slot, int64_t deadline_us)
{
    while (true) {
        uint16_t inflight = 0;
        taskENTER_CRITICAL(&g_resource_lock);
        inflight = slot->inflight;
        taskEXIT_CRITICAL(&g_resource_lock);
        if (inflight == 0) {
            return ESP_OK;
        }
        if (deadline_us > 0 && esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

void FinishCloseResource(ResourceSlot* slot, bool removed)
{
    taskENTER_CRITICAL(&g_resource_lock);
    if (removed) {
        *slot = {};
    } else {
        slot->closing = false;
    }
    g_inflight_operations.fetch_sub(1, std::memory_order_acq_rel);
    taskEXIT_CRITICAL(&g_resource_lock);
}

void DiscardRegisteredResource(
    ResourceSlot* slots, size_t count, uint32_t session_id, void* handle)
{
    taskENTER_CRITICAL(&g_resource_lock);
    for (size_t i = 0; i < count; ++i) {
        if (slots[i].handle == handle &&
            slots[i].owner_session_id == session_id &&
            slots[i].inflight == 0) {
            slots[i] = {};
            break;
        }
    }
    taskEXIT_CRITICAL(&g_resource_lock);
}

bool HasRegisteredResources(uint32_t session_id)
{
    bool found = false;
    taskENTER_CRITICAL(&g_resource_lock);
    for (const ResourceSlot& slot : g_codec_slots) {
        found = found || slot.owner_session_id == session_id;
    }
    for (const ResourceSlot& slot : g_channel_slots) {
        found = found || slot.owner_session_id == session_id;
    }
    taskEXIT_CRITICAL(&g_resource_lock);
    return found;
}

esp_err_t DrainDriverOperations(int64_t deadline_us)
{
    // [hang-fix] A non-positive deadline would disable the timeout check and
    // turn this into an unbounded spin if an aborted channel close ever
    // leaves g_inflight_operations nonzero. No current caller passes 0, but
    // the sleep path must never be able to spin forever, so clamp instead of
    // trusting every future caller.
    if (deadline_us <= 0) {
        deadline_us = esp_timer_get_time() + kCallTimeoutUs;
    }
    while (g_inflight_operations.load(std::memory_order_acquire) != 0) {
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

i2c_master_bus_handle_t AsBus(wqn::services::AudioBusHandle handle)
{
    return reinterpret_cast<i2c_master_bus_handle_t>(handle);
}

i2c_master_dev_handle_t AsCodec(wqn::services::AudioCodecHandle handle)
{
    return reinterpret_cast<i2c_master_dev_handle_t>(handle);
}

i2s_chan_handle_t AsChannel(wqn::services::AudioChannelHandle handle)
{
    return reinterpret_cast<i2s_chan_handle_t>(handle);
}

esp_err_t HoldOutput(gpio_num_t pin, bool enabled)
{
    ESP_RETURN_ON_ERROR(gpio_hold_dis(pin), kTag, "release GPIO hold");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(pin, enabled ? 1 : 0), kTag, "set GPIO output");
    return gpio_hold_en(pin);
}

esp_err_t ApplyAmplifier(bool enabled)
{
    ESP_RETURN_ON_ERROR(
        HoldOutput(kAmplifierEnable, enabled), kTag, "set amplifier GPIO");
    g_amplifier_enabled.store(enabled, std::memory_order_release);
    return ESP_OK;
}

esp_err_t ApplyCodecPower(bool enabled)
{
    ESP_RETURN_ON_ERROR(
        HoldOutput(kCodecPower, enabled), kTag, "set codec power GPIO");
    g_codec_powered.store(enabled, std::memory_order_release);
    return ESP_OK;
}

esp_err_t RecoverCodecPower(const AudioCommand& command)
{
    if (command.session_id == 0 ||
        command.session_id != g_session_id.load(std::memory_order_acquire) ||
        command.activity != g_activity.load(std::memory_order_acquire) ||
        !g_accept_operations.load(std::memory_order_acquire) ||
        g_state.load(std::memory_order_acquire) !=
            StateForActivity(command.activity)) {
        return ESP_ERR_INVALID_STATE;
    }
    // A power recovery must not race an active I2C/I2S transaction. Capture
    // calls this before registering the next codec/channel, but keep the
    // guard here as the final owner-side safety check.
    if (HasRegisteredResources(command.session_id)) {
        ESP_LOGW(kTag,
                 "codec power recovery rejected: session=%lu resources still registered",
                 static_cast<unsigned long>(command.session_id));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ApplyAmplifier(false);
    if (result == ESP_OK) {
        result = ApplyCodecPower(false);
    }
    ESP_LOGI(kTag, "codec power recovery: session=%lu power_off=%s (%d)",
             static_cast<unsigned long>(command.session_id),
             esp_err_to_name(result), static_cast<int>(result));
    if (result != ESP_OK) {
        return result;
    }

    vTaskDelay(kCodecPowerOffSettle);
    result = ApplyCodecPower(true);
    ESP_LOGI(kTag, "codec power recovery: session=%lu power_on=%s (%d)",
             static_cast<unsigned long>(command.session_id),
             esp_err_to_name(result), static_cast<int>(result));
    if (result == ESP_OK) {
        // ES8311's analog/reference rail needs a full warm-up after a real
        // power cycle. The delay is paid only after a cold start or a prior
        // failed capture; normal consecutive turns keep the warm rail.
        vTaskDelay(kCodecPowerOnWarmup);
    }
    return result;
}

void SetState(wqn::services::AudioState state)
{
    const wqn::services::AudioState previous =
        g_state.exchange(state, std::memory_order_acq_rel);
    if (previous != state) {
        ESP_LOGI(kTag, "state: %s -> %s",
                 wqn::services::AudioStateName(previous),
                 wqn::services::AudioStateName(state));
    }
}

wqn::services::AudioState StateForActivity(
    wqn::services::AudioActivity activity)
{
    switch (activity) {
        case wqn::services::AudioActivity::kCapture:
            return wqn::services::AudioState::kCapturing;
        case wqn::services::AudioActivity::kPlayback:
            return wqn::services::AudioState::kPlaying;
        case wqn::services::AudioActivity::kFlash:
            return wqn::services::AudioState::kFlash;
        case wqn::services::AudioActivity::kSelfTest:
            return wqn::services::AudioState::kSelfTest;
        default:
            return wqn::services::AudioState::kIdle;
    }
}

i2s_chan_config_t MakeChannelConfig(uint32_t dma_frame_count)
{
    i2s_chan_config_t config = {};
    config.id = I2S_NUM_0;
    config.role = I2S_ROLE_MASTER;
    config.dma_desc_num = 6;
    config.dma_frame_num = dma_frame_count;
    config.auto_clear_after_cb = true;
    config.auto_clear_before_cb = false;
    config.intr_priority = 0;
    return config;
}

i2s_std_config_t MakeStandardConfig(
    uint32_t sample_rate_hz, bool left_aligned, bool receive)
{
    i2s_std_config_t config = {};
    config.clk_cfg.sample_rate_hz = sample_rate_hz;
    config.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    config.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    config.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    config.slot_cfg.ws_pol = false;
    config.slot_cfg.bit_shift = true;
    config.slot_cfg.left_align = left_aligned;
    config.slot_cfg.big_endian = false;
    config.slot_cfg.bit_order_lsb = false;
    config.gpio_cfg.mclk = kI2sMclk;
    config.gpio_cfg.bclk = kI2sBclk;
    config.gpio_cfg.ws = kI2sWordSelect;
    config.gpio_cfg.dout = receive ? I2S_GPIO_UNUSED : kI2sDataOut;
    config.gpio_cfg.din = receive ? kI2sDataIn : I2S_GPIO_UNUSED;
    config.gpio_cfg.invert_flags.mclk_inv = false;
    config.gpio_cfg.invert_flags.bclk_inv = false;
    config.gpio_cfg.invert_flags.ws_inv = false;
    return config;
}

esp_err_t BeginActivity(const AudioCommand& command, AudioReply* reply)
{
    if (reply == nullptr || !IsValidActivity(command.activity)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_state.load(std::memory_order_acquire) !=
        wqn::services::AudioState::kIdle) {
        return ESP_ERR_INVALID_STATE;
    }

    wqn::runtime::SleepLease lease =
        wqn::runtime::SleepLease::TryAcquire(
            wqn::runtime::SleepBlocker::kAudio,
            wqn::services::AudioActivityName(command.activity),
            __FILE__,
            __LINE__);
    if (!lease) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t session_id = g_next_session_id++;
    if (session_id == 0) {
        session_id = g_next_session_id++;
    }
    g_audio_lease = std::move(lease);
    g_activity.store(command.activity, std::memory_order_release);
    g_session_id.store(session_id, std::memory_order_release);
    SetState(StateForActivity(command.activity));
    g_accept_operations.store(true, std::memory_order_release);
    reply->session_id = session_id;
    ESP_LOGI(kTag, "activity begin: activity=%s session=%lu",
             wqn::services::AudioActivityName(command.activity),
             static_cast<unsigned long>(session_id));
    return ESP_OK;
}

esp_err_t EndActivity(const AudioCommand& command)
{
    const uint32_t active_session =
        g_session_id.load(std::memory_order_acquire);
    if (command.session_id == 0 || command.session_id != active_session ||
        command.activity != g_activity.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    g_accept_operations.store(false, std::memory_order_release);
    const int64_t deadline_us = esp_timer_get_time() + kCallTimeoutUs;
    esp_err_t result = DrainDriverOperations(deadline_us);
    const bool resources_live = HasRegisteredResources(active_session);
    if (result != ESP_OK || resources_live) {
        ESP_LOGE(kTag,
                 "activity end denied: session=%lu inflight=%lu resources_live=%d error=%s",
                 static_cast<unsigned long>(active_session),
                 static_cast<unsigned long>(
                     g_inflight_operations.load(std::memory_order_acquire)),
                 resources_live ? 1 : 0,
                 esp_err_to_name(result));
        g_accept_operations.store(true, std::memory_order_release);
        return result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
    }
    result = ApplyAmplifier(false);
    if (result != ESP_OK) {
        g_accept_operations.store(true, std::memory_order_release);
        return result;
    }
    g_session_id.store(0, std::memory_order_release);
    SetState(wqn::services::AudioState::kIdle);
    g_audio_lease.Reset();
    ESP_LOGI(kTag, "activity end: activity=%s session=%lu",
             wqn::services::AudioActivityName(command.activity),
             static_cast<unsigned long>(command.session_id));
    return ESP_OK;
}

esp_err_t PrepareForSleep(const AudioCommand& command)
{
    if (command.sleep.generation == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool emergency = command.sleep.mode ==
        wqn::power::SleepMode::kBatteryEmergency;
    if (!emergency && command.sleep.deadline_us > 0 &&
        esp_timer_get_time() >= command.sleep.deadline_us) {
        return ESP_ERR_TIMEOUT;
    }
    const wqn::services::AudioState current_state =
        g_state.load(std::memory_order_acquire);
    if (current_state == wqn::services::AudioState::kQuiescing) {
        return g_prepared_generation == command.sleep.generation
            ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (!emergency && current_state != wqn::services::AudioState::kIdle) {
        return ESP_ERR_INVALID_STATE;
    }

    g_accept_operations.store(false, std::memory_order_release);
    esp_err_t result = DrainDriverOperations(command.sleep.deadline_us);
    if (result != ESP_OK && !emergency) {
        g_accept_operations.store(
            current_state != wqn::services::AudioState::kIdle,
            std::memory_order_release);
        return result;
    }
    if (result != ESP_OK) {
        ESP_LOGW(kTag,
                 "emergency audio I/O drain exceeded deadline; forcing rails off");
    }
    result = ApplyAmplifier(false);
    if (result == ESP_OK) {
        result = ApplyCodecPower(false);
    }
    if (result != ESP_OK) {
        g_accept_operations.store(
            current_state != wqn::services::AudioState::kIdle,
            std::memory_order_release);
        return result;
    }
    g_prepared_previous_state = current_state;
    g_prepared_generation = command.sleep.generation;
    SetState(wqn::services::AudioState::kQuiescing);
    ESP_LOGI(kTag, "prepared for sleep: generation=%lu mode=%s",
             static_cast<unsigned long>(command.sleep.generation),
             wqn::power::SleepModeName(command.sleep.mode));
    return ESP_OK;
}

esp_err_t RollbackSleep(const AudioCommand& command)
{
    if (g_state.load(std::memory_order_acquire) !=
        wqn::services::AudioState::kQuiescing) {
        return ESP_OK;
    }
    if (command.sleep.generation == 0 ||
        command.sleep.generation != g_prepared_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(
        ApplyCodecPower(true), kTag, "restore codec after sleep abort");
    ESP_RETURN_ON_ERROR(
        ApplyAmplifier(false), kTag, "keep amplifier off after sleep abort");
    const wqn::services::AudioState restore_state =
        g_prepared_previous_state;
    g_prepared_generation = 0;
    g_prepared_previous_state = wqn::services::AudioState::kIdle;
    SetState(restore_state);
    g_accept_operations.store(
        restore_state != wqn::services::AudioState::kIdle,
        std::memory_order_release);
    ESP_LOGI(kTag, "sleep preparation rolled back: generation=%lu",
             static_cast<unsigned long>(command.sleep.generation));
    return ESP_OK;
}

esp_err_t HandleCommand(const AudioCommand& command, AudioReply* reply)
{
    switch (command.type) {
        case CommandType::kBegin:
            return BeginActivity(command, reply);
        case CommandType::kEnd:
            return EndActivity(command);
        case CommandType::kSetAmplifier:
            if (command.session_id == 0 ||
                command.session_id !=
                    g_session_id.load(std::memory_order_acquire) ||
                command.activity != g_activity.load(std::memory_order_acquire) ||
                !g_accept_operations.load(std::memory_order_acquire) ||
                g_state.load(std::memory_order_acquire) !=
                    StateForActivity(command.activity)) {
                return ESP_ERR_INVALID_STATE;
            }
            return ApplyAmplifier(command.enabled);
        case CommandType::kRecoverCodec:
            return RecoverCodecPower(command);
        case CommandType::kPrepareSleep:
            return PrepareForSleep(command);
        case CommandType::kRollbackSleep:
            return RollbackSleep(command);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

void AudioServiceTask(void*)
{
    ESP_LOGI(kTag, "started: command_depth=%u reply_depth=%u",
             static_cast<unsigned>(kCommandQueueDepth),
             static_cast<unsigned>(kReplyQueueDepth));
    while (true) {
        AudioCommand command;
        if (xQueueReceive(g_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        AudioReply reply;
        reply.request_id = command.request_id;
        reply.result = HandleCommand(command, &reply);
        if (xQueueSend(g_reply_queue, &reply, 0) != pdTRUE) {
            ESP_LOGW(kTag, "reply dropped after caller timeout: request=%lu",
                     static_cast<unsigned long>(command.request_id));
        }
    }
}

TickType_t RemainingCallTicks(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    const uint64_t tick_us = static_cast<uint64_t>(portTICK_PERIOD_MS) * 1000;
    const uint64_t ticks =
        (static_cast<uint64_t>(remaining_us) + tick_us - 1) / tick_us;
    return static_cast<TickType_t>(ticks > portMAX_DELAY
        ? portMAX_DELAY : ticks);
}

esp_err_t Call(
    AudioCommand* command,
    AudioReply* reply,
    int64_t requested_deadline_us = 0)
{
    if (command == nullptr || reply == nullptr || g_task == nullptr ||
        g_command_queue == nullptr || g_reply_queue == nullptr ||
        g_call_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == g_task) {
        reply->request_id = command->request_id;
        reply->result = HandleCommand(*command, reply);
        return reply->result;
    }
    const int64_t default_deadline_us = esp_timer_get_time() + kCallTimeoutUs;
    const int64_t deadline_us = requested_deadline_us > 0
        ? std::min(requested_deadline_us, default_deadline_us)
        : default_deadline_us;
    TickType_t remaining = RemainingCallTicks(deadline_us);
    if (remaining == 0 ||
        xSemaphoreTake(g_call_mutex, remaining) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    command->request_id = g_next_request_id++;
    if (command->request_id == 0) {
        command->request_id = g_next_request_id++;
    }
    // A timed-out caller can leave a late reply behind. Drain it while no
    // other synchronous caller is allowed past g_call_mutex, then wait for
    // this request's exact terminal result.
    AudioReply stale_reply;
    while (xQueueReceive(g_reply_queue, &stale_reply, 0) == pdTRUE) {
        ESP_LOGW(kTag, "discard stale reply: request=%lu",
                 static_cast<unsigned long>(stale_reply.request_id));
    }

    esp_err_t result = ESP_ERR_TIMEOUT;
    remaining = RemainingCallTicks(deadline_us);
    if (remaining > 0 &&
        xQueueSend(g_command_queue, command, remaining) == pdTRUE) {
        remaining = RemainingCallTicks(deadline_us);
        while (remaining > 0 &&
               xQueueReceive(g_reply_queue, reply, remaining) == pdTRUE) {
            if (reply->request_id == command->request_id) {
                result = reply->result;
                break;
            }
            ESP_LOGW(kTag, "discard mismatched reply: expected=%lu actual=%lu",
                     static_cast<unsigned long>(command->request_id),
                     static_cast<unsigned long>(reply->request_id));
            remaining = RemainingCallTicks(deadline_us);
        }
    }
    xSemaphoreGive(g_call_mutex);
    return result;
}

}  // namespace

namespace wqn::services {

const char* AudioActivityName(AudioActivity activity)
{
    switch (activity) {
        case AudioActivity::kCapture:
            return "audio-capture";
        case AudioActivity::kPlayback:
            return "audio-playback";
        case AudioActivity::kFlash:
            return "audio-flash";
        case AudioActivity::kSelfTest:
            return "audio-selftest";
        default:
            return "audio-unknown";
    }
}

const char* AudioStateName(AudioState state)
{
    switch (state) {
        case AudioState::kIdle:
            return "idle";
        case AudioState::kCapturing:
            return "capturing";
        case AudioState::kPlaying:
            return "playing";
        case AudioState::kFlash:
            return "flash";
        case AudioState::kSelfTest:
            return "selftest";
        case AudioState::kQuiescing:
            return "quiescing";
        default:
            return "unknown";
    }
}

esp_err_t StartAudioService()
{
    const int64_t deadline_us = esp_timer_get_time() + kCallTimeoutUs;
    while (true) {
        bool create_service = false;
        taskENTER_CRITICAL(&g_start_lock);
        if (g_task != nullptr) {
            taskEXIT_CRITICAL(&g_start_lock);
            return ESP_OK;
        }
        if (!g_starting) {
            g_starting = true;
            create_service = true;
        }
        taskEXIT_CRITICAL(&g_start_lock);
        if (create_service) {
            break;
        }
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    taskENTER_CRITICAL(&g_start_lock);
    if (g_command_queue == nullptr) {
        g_command_queue = xQueueCreateStatic(
            kCommandQueueDepth, sizeof(AudioCommand),
            g_command_queue_buffer, &g_command_queue_storage);
    }
    if (g_reply_queue == nullptr) {
        g_reply_queue = xQueueCreateStatic(
            kReplyQueueDepth, sizeof(AudioReply),
            g_reply_queue_buffer, &g_reply_queue_storage);
    }
    if (g_call_mutex == nullptr) {
        g_call_mutex = xSemaphoreCreateMutexStatic(&g_call_mutex_storage);
    }
    taskEXIT_CRITICAL(&g_start_lock);

    if (g_command_queue == nullptr || g_reply_queue == nullptr ||
        g_call_mutex == nullptr) {
        taskENTER_CRITICAL(&g_start_lock);
        g_starting = false;
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t task = nullptr;
    if (xTaskCreate(AudioServiceTask, "audio_svc", kTaskStackBytes, nullptr,
                    kTaskPriority, &task) != pdPASS) {
        taskENTER_CRITICAL(&g_start_lock);
        g_starting = false;
        taskEXIT_CRITICAL(&g_start_lock);
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&g_start_lock);
    g_task = task;
    g_starting = false;
    taskEXIT_CRITICAL(&g_start_lock);
    return ESP_OK;
}

esp_err_t BeginAudioActivity(AudioActivity activity, AudioSession* session)
{
    if (session == nullptr || *session || !IsValidActivity(activity)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(StartAudioService(), kTag, "start audio service");
    AudioCommand command;
    command.type = CommandType::kBegin;
    command.activity = activity;
    AudioReply reply;
    const esp_err_t result = Call(&command, &reply);
    if (result == ESP_OK) {
        session->id = reply.session_id;
        session->activity = activity;
    }
    return result;
}

esp_err_t EndAudioActivity(AudioSession* session)
{
    if (session == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!*session) {
        return ESP_OK;
    }
    AudioCommand command;
    command.type = CommandType::kEnd;
    command.session_id = session->id;
    command.activity = session->activity;
    AudioReply reply;
    const esp_err_t result = Call(&command, &reply);
    if (result == ESP_OK) {
        session->id = 0;
    }
    return result;
}

esp_err_t SetAudioAmplifier(const AudioSession& session, bool enabled)
{
    if (!session) {
        return ESP_ERR_INVALID_STATE;
    }
    AudioCommand command;
    command.type = CommandType::kSetAmplifier;
    command.session_id = session.id;
    command.activity = session.activity;
    command.enabled = enabled;
    AudioReply reply;
    return Call(&command, &reply);
}

esp_err_t RecoverAudioCodec(const AudioSession& session)
{
    if (!session) {
        return ESP_ERR_INVALID_STATE;
    }
    AudioCommand command;
    command.type = CommandType::kRecoverCodec;
    command.session_id = session.id;
    command.activity = session.activity;
    AudioReply reply;
    return Call(&command, &reply);
}

esp_err_t GetSharedAudioBus(
    const AudioSession& session, AudioBusHandle* bus)
{
    ESP_LOGI(kTag, "get shared I2C bus: session=%lu bus_in=%p state=%s accept=%d",
             static_cast<unsigned long>(session.id),
             bus == nullptr ? nullptr : *bus,
             wqn::services::AudioStateName(
                 g_state.load(std::memory_order_acquire)),
             g_accept_operations.load(std::memory_order_acquire) ? 1 : 0);
    DriverOperation operation(session);
    if (!operation || bus == nullptr) {
        ESP_LOGE(kTag, "get shared I2C bus rejected: session=%lu result=%s (%d)",
                 static_cast<unsigned long>(session.id),
                 esp_err_to_name(ESP_ERR_INVALID_STATE),
                 static_cast<int>(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_bus_handle_t shared = wqn::GetSharedI2cBusHandle();
    if (shared == nullptr) {
        *bus = nullptr;
        ESP_LOGE(kTag, "get shared I2C bus returned null: session=%lu",
                 static_cast<unsigned long>(session.id));
        return ESP_ERR_INVALID_STATE;
    }
    *bus = reinterpret_cast<AudioBusHandle>(shared);
    ESP_LOGI(kTag, "get shared I2C bus ok: session=%lu bus=%p",
             static_cast<unsigned long>(session.id), *bus);
    return ESP_OK;
}

esp_err_t AddAudioCodec(
    const AudioSession& session,
    AudioBusHandle bus,
    AudioCodecHandle* codec)
{
    ESP_LOGI(kTag, "add ES8311: session=%lu bus=%p codec_in=%p state=%s accept=%d",
             static_cast<unsigned long>(session.id), bus,
             codec == nullptr ? nullptr : *codec,
             wqn::services::AudioStateName(
                 g_state.load(std::memory_order_acquire)),
             g_accept_operations.load(std::memory_order_acquire) ? 1 : 0);
    DriverOperation operation(session);
    if (!operation || bus == nullptr || codec == nullptr || *codec != nullptr) {
        ESP_LOGE(kTag, "add ES8311 rejected: session=%lu result=%s (%d)",
                 static_cast<unsigned long>(session.id),
                 esp_err_to_name(ESP_ERR_INVALID_STATE),
                 static_cast<int>(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }
    // `i2c_master_bus_add_device` only allocates and attaches a software
    // device object; it does not verify that the codec ACKs on the wires.
    // Probe while the AudioService operation is held so a stale/unpowered
    // ES8311 is reported before any register sequence is attempted.
    const esp_err_t probe_result =
        i2c_master_probe(AsBus(bus), kEs8311Address, 100);
    ESP_LOGI(kTag, "probe ES8311: session=%lu bus=%p addr=0x%02x result=%s (%d)",
             static_cast<unsigned long>(session.id), bus,
             static_cast<unsigned>(kEs8311Address),
             esp_err_to_name(probe_result), static_cast<int>(probe_result));
    if (probe_result != ESP_OK) {
        return probe_result;
    }
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kEs8311Address;
    config.scl_speed_hz = kCodecI2cClockHz;
    i2c_master_dev_handle_t device = nullptr;
    esp_err_t result = i2c_master_bus_add_device(
        AsBus(bus), &config, &device);
    if (result == ESP_OK) {
        result = RegisterResource(
            g_codec_slots, kMaxCodecHandles, session, device);
    }
    if (result == ESP_OK) {
        *codec = reinterpret_cast<AudioCodecHandle>(device);
    } else if (device != nullptr) {
        i2c_master_bus_rm_device(device);
    }
    ESP_LOGI(kTag, "add ES8311 result: session=%lu result=%s (%d) device=%p scl_hz=%lu",
             static_cast<unsigned long>(session.id),
             esp_err_to_name(result), static_cast<int>(result), device,
             static_cast<unsigned long>(kCodecI2cClockHz));
    return result;
}

esp_err_t RemoveAudioCodec(
    const AudioSession& session, AudioCodecHandle* codec)
{
    if (codec == nullptr || *codec == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ResourceSlot* slot = nullptr;
    ESP_RETURN_ON_ERROR(
        BeginCloseResource(
            g_codec_slots, kMaxCodecHandles, session, *codec, &slot),
        kTag, "begin codec removal");
    esp_err_t result = WaitForResourceDrain(
        slot, esp_timer_get_time() + kCallTimeoutUs);
    if (result == ESP_OK) {
        result = i2c_master_bus_rm_device(AsCodec(*codec));
    }
    FinishCloseResource(slot, result == ESP_OK);
    if (result == ESP_OK) {
        *codec = nullptr;
    }
    return result;
}

esp_err_t WriteAudioCodecRegister(
    const AudioSession& session,
    AudioCodecHandle codec,
    uint8_t reg,
    uint8_t value)
{
    DriverOperation operation(
        session, DriverOperation::Kind::kCodec, codec);
    if (!operation || codec == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t data[] = {reg, value};
    esp_err_t result = ESP_FAIL;
    int attempts_used = 0;
    for (int attempt = 1; attempt <= kCodecI2cMaxAttempts; ++attempt) {
        attempts_used = attempt;
        result = i2c_master_transmit(
            AsCodec(codec), data, sizeof(data), pdMS_TO_TICKS(100));
        if (result == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(kTag,
                         "codec write recovered: session=%lu dev=%p reg=0x%02x attempt=%d",
                         static_cast<unsigned long>(session.id), codec, reg,
                         attempt);
            }
            return ESP_OK;
        }
        if (!IsRetryableCodecI2cError(result) ||
            attempt == kCodecI2cMaxAttempts) {
            break;
        }
        ESP_LOGW(kTag,
                 "codec write retry: session=%lu dev=%p reg=0x%02x attempt=%d/%d result=%s (%d)",
                 static_cast<unsigned long>(session.id), codec, reg, attempt,
                 kCodecI2cMaxAttempts, esp_err_to_name(result),
                 static_cast<int>(result));
        vTaskDelay(kCodecI2cRetryDelay);
    }
    ESP_LOGE(kTag,
             "codec write API=i2c_master_transmit session=%lu dev=%p addr=0x%02x reg=0x%02x value=0x%02x attempts=%d result=%s (%d)",
             static_cast<unsigned long>(session.id), codec,
             static_cast<unsigned>(kEs8311Address), reg, value,
             attempts_used, esp_err_to_name(result),
             static_cast<int>(result));
    return result;
}

esp_err_t ReadAudioCodecRegister(
    const AudioSession& session,
    AudioCodecHandle codec,
    uint8_t reg,
    uint8_t* value)
{
    DriverOperation operation(
        session, DriverOperation::Kind::kCodec, codec);
    if (!operation || codec == nullptr || value == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_FAIL;
    int attempts_used = 0;
    for (int attempt = 1; attempt <= kCodecI2cMaxAttempts; ++attempt) {
        attempts_used = attempt;
        result = i2c_master_transmit_receive(
            AsCodec(codec), &reg, sizeof(reg), value, sizeof(*value),
            pdMS_TO_TICKS(100));
        if (result == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(kTag,
                         "codec read recovered: session=%lu dev=%p reg=0x%02x attempt=%d",
                         static_cast<unsigned long>(session.id), codec, reg,
                         attempt);
            }
            return ESP_OK;
        }
        if (!IsRetryableCodecI2cError(result) ||
            attempt == kCodecI2cMaxAttempts) {
            break;
        }
        ESP_LOGW(kTag,
                 "codec read retry: session=%lu dev=%p reg=0x%02x attempt=%d/%d result=%s (%d)",
                 static_cast<unsigned long>(session.id), codec, reg, attempt,
                 kCodecI2cMaxAttempts, esp_err_to_name(result),
                 static_cast<int>(result));
        vTaskDelay(kCodecI2cRetryDelay);
    }
    ESP_LOGE(kTag,
             "codec read API=i2c_master_transmit_receive session=%lu dev=%p addr=0x%02x reg=0x%02x attempts=%d result=%s (%d)",
             static_cast<unsigned long>(session.id), codec,
             static_cast<unsigned>(kEs8311Address), reg,
             attempts_used, esp_err_to_name(result),
             static_cast<int>(result));
    return result;
}

esp_err_t CreateAudioRxChannel(
    const AudioSession& session,
    uint32_t sample_rate_hz,
    uint32_t dma_frame_count,
    bool left_aligned,
    AudioChannelHandle* rx)
{
    ESP_LOGI(kTag,
             "create I2S RX: session=%lu rx_in=%p rate=%lu dma=%lu left_aligned=%d state=%s accept=%d",
             static_cast<unsigned long>(session.id),
             rx == nullptr ? nullptr : *rx,
             static_cast<unsigned long>(sample_rate_hz),
             static_cast<unsigned long>(dma_frame_count),
             left_aligned ? 1 : 0,
             wqn::services::AudioStateName(
                 g_state.load(std::memory_order_acquire)),
             g_accept_operations.load(std::memory_order_acquire) ? 1 : 0);
    DriverOperation operation(session);
    if (!operation || rx == nullptr || *rx != nullptr) {
        ESP_LOGE(kTag, "create I2S RX rejected: session=%lu result=%s (%d)",
                 static_cast<unsigned long>(session.id),
                 esp_err_to_name(ESP_ERR_INVALID_STATE),
                 static_cast<int>(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }
    i2s_chan_handle_t channel = nullptr;
    i2s_chan_config_t channel_config = MakeChannelConfig(dma_frame_count);
    esp_err_t result = i2s_new_channel(
        &channel_config, nullptr, &channel);
    ESP_LOGI(kTag, "create I2S RX: i2s_new_channel result=%s (%d) channel=%p",
             esp_err_to_name(result), static_cast<int>(result), channel);
    if (result == ESP_OK) {
        i2s_std_config_t standard = MakeStandardConfig(
            sample_rate_hz, left_aligned, true);
        result = i2s_channel_init_std_mode(channel, &standard);
        ESP_LOGI(kTag,
                 "create I2S RX: i2s_channel_init_std_mode result=%s (%d) channel=%p",
                 esp_err_to_name(result), static_cast<int>(result), channel);
    }
    if (result == ESP_OK) {
        result = i2s_channel_enable(channel);
        ESP_LOGI(kTag, "create I2S RX: i2s_channel_enable result=%s (%d) channel=%p",
                 esp_err_to_name(result), static_cast<int>(result), channel);
    }
    if (result != ESP_OK) {
        if (channel != nullptr) {
            const esp_err_t cleanup_result = i2s_del_channel(channel);
            ESP_LOGI(kTag,
                     "create I2S RX: cleanup i2s_del_channel result=%s (%d) channel=%p",
                     esp_err_to_name(cleanup_result),
                     static_cast<int>(cleanup_result), channel);
        }
        return result;
    }
    result = RegisterResource(
        g_channel_slots, kMaxChannelHandles, session, channel);
    if (result != ESP_OK) {
        const esp_err_t disable_result = i2s_channel_disable(channel);
        const esp_err_t delete_result = i2s_del_channel(channel);
        ESP_LOGE(kTag,
                 "create I2S RX: register resource failed=%s (%d), disable=%s, delete=%s",
                 esp_err_to_name(result), static_cast<int>(result),
                 esp_err_to_name(disable_result), esp_err_to_name(delete_result));
        return result;
    }
    *rx = reinterpret_cast<AudioChannelHandle>(channel);
    ESP_LOGI(kTag, "create I2S RX ok: session=%lu rx=%p",
             static_cast<unsigned long>(session.id), *rx);
    return ESP_OK;
}

esp_err_t CreateAudioTxChannel(
    const AudioSession& session,
    uint32_t sample_rate_hz,
    uint32_t dma_frame_count,
    bool left_aligned,
    AudioChannelHandle* tx)
{
    DriverOperation operation(session);
    if (!operation || tx == nullptr || *tx != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    i2s_chan_handle_t channel = nullptr;
    i2s_chan_config_t channel_config = MakeChannelConfig(dma_frame_count);
    esp_err_t result = i2s_new_channel(
        &channel_config, &channel, nullptr);
    if (result == ESP_OK) {
        i2s_std_config_t standard = MakeStandardConfig(
            sample_rate_hz, left_aligned, false);
        result = i2s_channel_init_std_mode(channel, &standard);
    }
    if (result == ESP_OK) {
        result = i2s_channel_enable(channel);
    }
    if (result != ESP_OK) {
        if (channel != nullptr) {
            i2s_del_channel(channel);
        }
        return result;
    }
    result = RegisterResource(
        g_channel_slots, kMaxChannelHandles, session, channel);
    if (result != ESP_OK) {
        i2s_channel_disable(channel);
        i2s_del_channel(channel);
        return result;
    }
    *tx = reinterpret_cast<AudioChannelHandle>(channel);
    return ESP_OK;
}

esp_err_t CreateAudioDuplexChannels(
    const AudioSession& session,
    uint32_t sample_rate_hz,
    uint32_t dma_frame_count,
    bool left_aligned,
    AudioChannelHandle* rx,
    AudioChannelHandle* tx)
{
    DriverOperation operation(session);
    if (!operation || rx == nullptr || tx == nullptr ||
        *rx != nullptr || *tx != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    i2s_chan_handle_t rx_channel = nullptr;
    i2s_chan_handle_t tx_channel = nullptr;
    i2s_chan_config_t channel_config = MakeChannelConfig(dma_frame_count);
    esp_err_t result = i2s_new_channel(
        &channel_config, &tx_channel, &rx_channel);
    if (result == ESP_OK) {
        i2s_std_config_t receive = MakeStandardConfig(
            sample_rate_hz, left_aligned, true);
        result = i2s_channel_init_std_mode(rx_channel, &receive);
    }
    if (result == ESP_OK) {
        result = i2s_channel_enable(rx_channel);
    }
    if (result == ESP_OK) {
        i2s_std_config_t transmit = MakeStandardConfig(
            sample_rate_hz, left_aligned, false);
        result = i2s_channel_init_std_mode(tx_channel, &transmit);
    }
    if (result == ESP_OK) {
        result = i2s_channel_enable(tx_channel);
    }
    if (result != ESP_OK) {
        if (tx_channel != nullptr) {
            i2s_channel_disable(tx_channel);
            i2s_del_channel(tx_channel);
        }
        if (rx_channel != nullptr) {
            i2s_channel_disable(rx_channel);
            i2s_del_channel(rx_channel);
        }
        return result;
    }
    result = RegisterResource(
        g_channel_slots, kMaxChannelHandles, session, rx_channel);
    if (result == ESP_OK) {
        result = RegisterResource(
            g_channel_slots, kMaxChannelHandles, session, tx_channel);
    }
    if (result != ESP_OK) {
        DiscardRegisteredResource(
            g_channel_slots, kMaxChannelHandles, session.id, rx_channel);
        i2s_channel_disable(tx_channel);
        i2s_del_channel(tx_channel);
        i2s_channel_disable(rx_channel);
        i2s_del_channel(rx_channel);
        return result;
    }
    *rx = reinterpret_cast<AudioChannelHandle>(rx_channel);
    *tx = reinterpret_cast<AudioChannelHandle>(tx_channel);
    return ESP_OK;
}

esp_err_t EnableAudioChannel(
    const AudioSession& session, AudioChannelHandle channel)
{
    DriverOperation operation(
        session, DriverOperation::Kind::kChannel, channel);
    if (!operation || channel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_enable(AsChannel(channel));
}

esp_err_t DisableAudioChannel(
    const AudioSession& session, AudioChannelHandle channel)
{
    DriverOperation operation(
        session, DriverOperation::Kind::kChannel, channel);
    if (!operation || channel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_disable(AsChannel(channel));
}

esp_err_t DeleteAudioChannel(
    const AudioSession& session, AudioChannelHandle* channel)
{
    if (channel == nullptr || *channel == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ResourceSlot* slot = nullptr;
    ESP_RETURN_ON_ERROR(
        BeginCloseResource(
            g_channel_slots, kMaxChannelHandles, session, *channel, &slot),
        kTag, "begin channel deletion");
    esp_err_t result = WaitForResourceDrain(
        slot, esp_timer_get_time() + kCallTimeoutUs);
    if (result == ESP_OK) {
        result = i2s_del_channel(AsChannel(*channel));
    }
    FinishCloseResource(slot, result == ESP_OK);
    if (result == ESP_OK) {
        *channel = nullptr;
    }
    return result;
}

esp_err_t ReadAudioChannel(
    const AudioSession& session,
    AudioChannelHandle channel,
    void* destination,
    size_t bytes,
    size_t* bytes_read,
    TickType_t timeout)
{
    DriverOperation operation(
        session, DriverOperation::Kind::kChannel, channel);
    if (!operation || channel == nullptr ||
        destination == nullptr || bytes_read == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_read(
        AsChannel(channel), destination, bytes, bytes_read, timeout);
}

esp_err_t WriteAudioChannel(
    const AudioSession& session,
    AudioChannelHandle channel,
    const void* source,
    size_t bytes,
    size_t* bytes_written,
    TickType_t timeout)
{
    DriverOperation operation(
        session, DriverOperation::Kind::kChannel, channel);
    if (!operation || channel == nullptr ||
        source == nullptr || bytes_written == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_write(
        AsChannel(channel), source, bytes, bytes_written, timeout);
}

esp_err_t ResetAudioTxChannel(
    const AudioSession& session,
    AudioChannelHandle channel,
    size_t silence_bytes)
{
    if (channel == nullptr || silence_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // Keep one resource operation across disable, preload and enable. Calling
    // the public helpers separately would leave a window in which the session
    // teardown task could delete the channel between the two operations.
    DriverOperation operation(
        session, DriverOperation::Kind::kChannel, channel);
    if (!operation) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = i2s_channel_disable(AsChannel(channel));
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "reset audio TX: disable failed result=%s (%d)",
                 esp_err_to_name(result), static_cast<int>(result));
        return result;
    }

    // i2s_channel_disable resets the TX queue/current descriptor, but it does
    // not clear the descriptor payloads.  Preload the entire ring while the
    // channel is READY so a subsequent enable cannot replay samples already
    // fetched by DMA before a barge-in/abort.
    std::array<uint8_t, 256> silence = {};
    size_t total_loaded = 0;
    while (total_loaded < silence_bytes) {
        const size_t remaining = silence_bytes - total_loaded;
        const size_t request = std::min(remaining, silence.size());
        size_t loaded = 0;
        const esp_err_t preload_result = i2s_channel_preload_data(
            AsChannel(channel), silence.data(), request, &loaded);
        if (preload_result != ESP_OK) {
            ESP_LOGE(kTag,
                     "reset audio TX: preload silence failed after %u/%u bytes result=%s (%d)",
                     static_cast<unsigned>(total_loaded),
                     static_cast<unsigned>(silence_bytes),
                     esp_err_to_name(preload_result),
                     static_cast<int>(preload_result));
            return preload_result;
        }
        if (loaded == 0) {
            ESP_LOGE(kTag,
                     "reset audio TX: preload made no progress at %u/%u bytes",
                     static_cast<unsigned>(total_loaded),
                     static_cast<unsigned>(silence_bytes));
            return ESP_ERR_INVALID_STATE;
        }
        total_loaded += loaded;
    }
    ESP_LOGD(kTag, "reset audio TX: preloaded %u bytes of silence",
             static_cast<unsigned>(total_loaded));
    return i2s_channel_enable(AsChannel(channel));
}

esp_err_t PrepareAudioServiceForSleep(
    const power::PrepareSleepCommand& command)
{
    AudioCommand audio_command;
    audio_command.type = CommandType::kPrepareSleep;
    audio_command.sleep = command;
    AudioReply reply;
    int64_t call_deadline_us = command.deadline_us;
    if (command.mode == power::SleepMode::kBatteryEmergency) {
        call_deadline_us = std::max(
            call_deadline_us,
            esp_timer_get_time() + kEmergencyForceCallBudgetUs);
    }
    return Call(&audio_command, &reply, call_deadline_us);
}

void RollbackAudioServiceAfterSleepAbort(uint32_t generation)
{
    AudioCommand command;
    command.type = CommandType::kRollbackSleep;
    command.sleep.generation = generation;
    AudioReply reply;
    const esp_err_t result = Call(&command, &reply);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "rollback failed: %s", esp_err_to_name(result));
    }
}

AudioSnapshot GetAudioSnapshot()
{
    AudioSnapshot snapshot;
    snapshot.state = g_state.load(std::memory_order_acquire);
    snapshot.session_id = g_session_id.load(std::memory_order_acquire);
    snapshot.codec_powered =
        g_codec_powered.load(std::memory_order_acquire);
    snapshot.amplifier_enabled =
        g_amplifier_enabled.load(std::memory_order_acquire);
    return snapshot;
}

}  // namespace wqn::services
