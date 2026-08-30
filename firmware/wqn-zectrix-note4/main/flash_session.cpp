#include "flash_session.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "audio_player.h"
#include "audio_capture.h"
#include "ai_history.h"
#include "cJSON.h"
#include "config.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/audio_service.h"
#include "services/connectivity_service.h"
#include "storage.h"

namespace wqn {
esp_err_t StartFlashSessionNow(uint32_t generation);
esp_err_t StopFlashSessionNow();
}

namespace {

constexpr char kTag[] = "wqn_flash";

#if CONFIG_WQN_FLASH_PROTOCOL_V2
// Defined later in this translation unit; forward-declared here so the
// WEBSOCKET_EVENT_DATA handler below can call it regardless of ordering.
void HandleV2DownlinkAudio(const uint8_t* data, size_t len,
                           uint64_t payload_offset, uint64_t payload_len);
#endif
// ============================================================================
// wqn-flash-v2 WebSocket endpoint
//
//   * URL:       wss://wqn.helema.cn/api/esp32/realtime
//   * Subproto:  wqn-flash-v2
//   * Auth:      Bearer {access_token} (same long-term token as the v2 SSE path)
//   * Uplink:    binary frames (0x02).  Frame layout is a 24-byte little-endian
//                header followed by raw PCM s16le mono audio at 16 kHz:
//                  [u32 magic      = 0x57464C56  ('WFLV')]
//                  [u16 version    = 2]
//                  [u16 flags      = 0x0001 stream | 0x0002 final]
//                  [u32 seq        = monotonic per session]
//                  [u32 sample_rate= 24000]
//                  [u32 channels   = 1]
//                  [u32 reserved   = 0]
//                  [audio bytes ...]
//   * Downlink:  binary frames containing Opus (24 kHz) for TTS playback OR
//                text frames containing JSON control events (`session.ready`,
//                `text.delta`, `tool.*`, `state`, `error`, `done`).  The
//                MagicByte in the first 4 bytes lets us route: 'WFLV' + ver=2
//                means audio; 'WFCJ' + ver=2 means JSON control.
// ============================================================================
#if CONFIG_WQN_FLASH_PROTOCOL_V2
constexpr char kWsHost[] = WQN_API_BASE_HOST;        // https://wqn.helema.cn
constexpr char kWsPath[] = WQN_FLASH_WS_PATH;        // /api/esp32/realtime
constexpr char kWsSubprotocol[] = WQN_FLASH_WS_SUBPROTOCOL;  // wqn-flash-v2
constexpr char kDefaultVoice[] = WQN_FLASH_VOICE;
constexpr char kDefaultInstructions[] = WQN_FLASH_DEFAULT_INSTRUCTIONS;
constexpr uint32_t kAudioFrameMagic = 0x57464C56u;   // 'W','F','L','V'
constexpr uint32_t kControlFrameMagic = 0x5746434Au; // 'W','F','C','J'
constexpr uint16_t kFrameVersion = 2;
constexpr uint16_t kFlagStream = 0x0001;
constexpr uint16_t kFlagFinal = 0x0002;
#else
// Legacy v1 StepFun-shaped protocol (kept for COMPILE-ONLY fallback).
constexpr char kWsUri[] = "wss://wqn.helema.cn/v1/realtime?model=stepaudio-2.5-realtime";
constexpr char kDefaultVoice[] = "qingchunshaonv";
#endif

constexpr int kSampleRate = 24000;
constexpr int kChunkFrames = 360;
constexpr int kChunkBytes = kChunkFrames * 2;
constexpr int kChunkIntervalMs = 15;

constexpr int kMaxReconnectAttempts = 3;
constexpr TickType_t kWifiReadyWait = pdMS_TO_TICKS(35000);
constexpr TickType_t kWsConnectTimeout = pdMS_TO_TICKS(20000);
constexpr uint32_t kLifecycleTaskStackBytes = 8192;
constexpr UBaseType_t kLifecycleTaskPriority = 6;
constexpr uint32_t kPlaybackTaskStackBytes = 4096;
constexpr uint32_t kStreamTaskStackBytes = 8192;

enum class InternalStatus {
    kIdle,
    kConnecting,
    kSessionUpdating,
    kStreaming,
    kError,
};

struct FlashState {
    SemaphoreHandle_t mutex = nullptr;
    InternalStatus status = InternalStatus::kIdle;
    bool button_pressed = false;
    bool capture_started = false;
    bool ws_connected = false;
    std::string user_transcript;
    std::string assistant_text;
    std::string thinking_text;
    bool history_user_committed = false;
    bool thinking_done = false;
    bool turn_done_received = false;
    wqn::ChatMessageId thinking_message_id = wqn::kInvalidChatMessageId;
    wqn::ChatMessageId assistant_message_id = wqn::kInvalidChatMessageId;
    std::string pending_text;
    std::string tool_label;
    std::string error_message;
    int reconnect_attempts = 0;
    int64_t status_since_ms = 0;
    esp_websocket_client_handle_t ws_client = nullptr;
    bool changed = false;
    wqn::AiTier tier = wqn::AiTier::kFlash;
    // WebSocket frame reassembly buffer for fragmented payloads
    std::string ws_reassembly_buf;
    // Streaming audio state
    TaskHandle_t stream_task = nullptr;
    wqn::services::AudioChannelHandle stream_rx = nullptr;
    wqn::services::AudioChannelHandle stream_tx = nullptr;
    wqn::services::AudioBusHandle stream_i2c_bus = nullptr;
    bool stream_audio_powered = false;
    // The playback task keeps this true for the entire blocking I2S write.
    // The amp idle timer may only turn GPIO46 off after the write has returned
    // and the queued PCM count has reached zero.
    bool playback_write_active = false;
    bool playback_abort_requested = false;
    int64_t amp_idle_due_ms = 0;
    bool amp_idle_armed = false;
    // v2 uplink frame counter
    uint32_t uplink_seq = 0;
    // [barge-fix] uplink_seq snapshot at PTT press. If unchanged on release,
    // the user tapped too fast (<250ms) for the streaming task to capture any
    // audio -> skip the empty final frame, which would otherwise trigger a
    // commit + response.create that collides with the barge-in response.cancel
    // (causes interleaved/serial replies + "append is not called" error).
    uint32_t capture_base_seq = 0;
    // [inflight-fix] True after we send a turn-end frame (commit+create) and
    // until response.done/error arrives. Guards against:
    //  - barge-in sending response.cancel when no response is in-flight
    //    ("no ongoing response to cancel")
    //  - a second turn-end while the prior response is still generating
    //    ("ongoing response already exists")
    // Ref: ElatoAI/xiaozhi track response state to avoid these races.
    bool response_in_flight = false;
    bool response_started = false;  // [phase-fix] delta received for CURRENT turn
    // v2 downlink binary frame reassembly buffer
    std::string audio_reassembly_buf;
    // [playback-fix] Decouple I2S writes from the WebSocket event task.
    // The WS task only enqueues decoded PCM into a FreeRTOS byte ringbuffer;
    // a dedicated FlashPlaybackTask drains the ringbuffer and writes to I2S.
    // This prevents head-of-line blocking: a slow i2s_channel_write no longer
    // stalls the WS event loop, which would otherwise delay heartbeats and
    // subsequent response.audio.delta events.
    RingbufHandle_t playback_ringbuf = nullptr;
    TaskHandle_t playback_task = nullptr;
    std::atomic<size_t> playback_queued_bytes{0};  // [race-fix] accessed from WS task + playback task + UI task
    std::atomic<bool> drain_playback{false};       // [drain-fix] flag set by UI task, consumed by playback task
    // [i2s-handoff] Set by StopFlashSession so FlashPlaybackTask self-exits
    // before TearDownStreamChannels deletes stream_tx. Without this the
    // playback task is a persistent writer of stream_tx and cannot be deleted
    // safely. Reset to false in EnsurePlaybackRingbuf before recreating the task.
    std::atomic<bool> playback_stop{false};
    size_t playback_dropped_bytes = 0;
};

FlashState g_flash;
wqn::services::AudioSession g_flash_audio_session;
wqn::services::ConnectivityDemand g_flash_connectivity_demand;

enum class FlashTerminalReason : uint8_t {
    kNone,
    kIntentional,
    kDisconnected,
    kTransportError,
    kServerError,
};

StaticTask_t g_lifecycle_task_tcb;
StackType_t g_lifecycle_task_stack[
    kLifecycleTaskStackBytes / sizeof(StackType_t)] = {};
TaskHandle_t g_lifecycle_task = nullptr;
// These realtime workers exist only while Flash is active, but allocating
// their 12 KiB of stacks from the default internal heap made that memory also
// disappear from MALLOC_CAP_DMA.  Retain one PSRAM stack for each worker and
// keep only their TCBs in internal RAM.  The tasks use static creation, so
// their self-deletion never frees or reallocates these stable buffers.
StaticTask_t g_playback_task_tcb;
StackType_t* g_playback_task_stack = nullptr;
StaticTask_t g_stream_task_tcb;
StackType_t* g_stream_task_stack = nullptr;
std::atomic<FlashTerminalReason> g_terminal_reason{
    FlashTerminalReason::kNone};
std::atomic<bool> g_intentional_stop{false};
std::atomic<bool> g_teardown_pending{false};
std::atomic<bool> g_restart_after_teardown{false};
std::atomic<uint32_t> g_session_generation{0};
std::atomic<uint32_t> g_terminal_generation{0};
std::atomic<bool> g_start_pending{false};
std::atomic<uint32_t> g_start_generation{0};

void SetErrorLocked(const std::string& message);

StackType_t* EnsurePsramTaskStack(
    StackType_t** stack, size_t bytes, const char* owner)
{
    if (*stack != nullptr) {
        return *stack;
    }
    *stack = static_cast<StackType_t*>(heap_caps_calloc(
        1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (*stack == nullptr) {
        ESP_LOGE(kTag, "%s PSRAM task stack allocation failed: bytes=%u",
                 owner, static_cast<unsigned>(bytes));
        return nullptr;
    }
    ESP_LOGI(kTag,
             "%s task stack retained in PSRAM: bytes=%u stack=%p dma_free=%u internal_free=%u",
             owner, static_cast<unsigned>(bytes), *stack,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    return *stack;
}

const char* FlashTerminalMessage(FlashTerminalReason reason)
{
    switch (reason) {
        case FlashTerminalReason::kDisconnected:
            return "连接断开";
        case FlashTerminalReason::kTransportError:
            return "WebSocket 错误";
        case FlashTerminalReason::kServerError:
            return "服务端错误";
        default:
            return "Flash 会话已结束";
    }
}

void FlashLifecycleTask(void*)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Let the WebSocket event callback return before destroying its client.
        vTaskDelay(pdMS_TO_TICKS(20));
        const FlashTerminalReason reason =
            g_terminal_reason.exchange(
                FlashTerminalReason::kNone, std::memory_order_acq_rel);
        if (reason == FlashTerminalReason::kNone) {
            if (!g_start_pending.exchange(false, std::memory_order_acq_rel)) {
                continue;
            }
            const uint32_t generation =
                g_start_generation.load(std::memory_order_acquire);
            const esp_err_t start_result =
                wqn::StartFlashSessionNow(generation);
            if (start_result != ESP_OK &&
                !g_teardown_pending.load(std::memory_order_acquire)) {
                xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
                if (generation ==
                    g_session_generation.load(std::memory_order_acquire)) {
                    if (g_flash.status != InternalStatus::kError) {
                        SetErrorLocked("Flash 会话启动失败");
                    }
                    g_flash_connectivity_demand.Reset();
                }
                xSemaphoreGive(g_flash.mutex);
            }
            continue;
        }
        const uint32_t generation =
            g_terminal_generation.load(std::memory_order_acquire);
        if (generation != g_session_generation.load(std::memory_order_acquire)) {
            ESP_LOGW(kTag, "discard stale Flash teardown generation=%lu active=%lu",
                     static_cast<unsigned long>(generation),
                     static_cast<unsigned long>(
                         g_session_generation.load(std::memory_order_relaxed)));
            continue;
        }

        g_intentional_stop.store(true, std::memory_order_release);
        const int64_t deadline_us = esp_timer_get_time() + 30LL * 1000 * 1000;
        esp_err_t stop_result = ESP_FAIL;
        uint32_t attempts = 0;
        do {
            ++attempts;
            stop_result = wqn::StopFlashSessionNow();
            if (stop_result == ESP_OK) {
                break;
            }
            ESP_LOGW(kTag,
                     "Flash teardown deferred: generation=%lu attempt=%lu error=%s",
                     static_cast<unsigned long>(generation),
                     static_cast<unsigned long>(attempts),
                     esp_err_to_name(stop_result));
            vTaskDelay(pdMS_TO_TICKS(250));
        } while (esp_timer_get_time() < deadline_us &&
                 generation == g_session_generation.load(std::memory_order_acquire));

        if (stop_result != ESP_OK) {
            // [audio-lease-fix] A live task may still enter a wrapper with its
            // session token, so deleting its I2S handle would be a UAF. A
            // controlled restart is the only bounded safe recovery after the
            // lifecycle task has retried for 30 seconds.
            ESP_LOGE(
                kTag,
                "Flash teardown stuck; controlled restart: generation=%lu attempts=%lu error=%s internal_free=%u psram_free=%u",
                static_cast<unsigned long>(generation),
                static_cast<unsigned long>(attempts),
                esp_err_to_name(stop_result),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }

        g_teardown_pending.store(false, std::memory_order_release);
        g_intentional_stop.store(false, std::memory_order_release);
        const bool restart =
            g_restart_after_teardown.exchange(false, std::memory_order_acq_rel);
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        if (reason != FlashTerminalReason::kIntentional) {
            SetErrorLocked(FlashTerminalMessage(reason));
        }
        xSemaphoreGive(g_flash.mutex);
        if (restart) {
            const esp_err_t restart_result = wqn::StartFlashSession();
            if (restart_result != ESP_OK) {
                ESP_LOGW(kTag, "Flash reconnect after teardown failed: %s",
                         esp_err_to_name(restart_result));
            }
        }
    }
}

esp_err_t EnsureFlashLifecycleTask()
{
    if (g_lifecycle_task != nullptr) {
        return ESP_OK;
    }
    g_lifecycle_task = xTaskCreateStatic(
        FlashLifecycleTask,
        "flash_lifecycle",
        kLifecycleTaskStackBytes,
        nullptr,
        kLifecycleTaskPriority,
        g_lifecycle_task_stack,
        &g_lifecycle_task_tcb);
    return g_lifecycle_task == nullptr ? ESP_ERR_NO_MEM : ESP_OK;
}

void RequestFlashTerminalStop(FlashTerminalReason reason)
{
    if (g_intentional_stop.load(std::memory_order_acquire) ||
        g_lifecycle_task == nullptr) {
        return;
    }
    bool expected = false;
    if (!g_teardown_pending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    g_terminal_generation.store(
        g_session_generation.load(std::memory_order_acquire),
        std::memory_order_relaxed);
    g_terminal_reason.store(reason, std::memory_order_release);
    xTaskNotifyGive(g_lifecycle_task);
}

bool EnsureFlashThinkingHistoryLocked(int64_t now_ms)
{
    if (!g_flash.history_user_committed || g_flash.thinking_text.empty()) {
        return false;
    }
    const std::string label = "💭 " + g_flash.thinking_text;
    wqn::AiHistory& history = wqn::GetAiHistory(wqn::AiHistoryChannel::kFlash);
    if (g_flash.thinking_message_id == wqn::kInvalidChatMessageId) {
        g_flash.thinking_message_id = history.AppendThinking(label, now_ms);
        return g_flash.thinking_message_id != wqn::kInvalidChatMessageId;
    }
    return history.ReplaceText(g_flash.thinking_message_id,
                               wqn::ChatMessageKind::kThinking,
                               label, now_ms);
}

bool FinalizeFlashAssistantLocked(int64_t now_ms)
{
    if (!g_flash.history_user_committed || g_flash.assistant_text.empty()) {
        return false;
    }
    wqn::AiHistory& history = wqn::GetAiHistory(wqn::AiHistoryChannel::kFlash);
    if (g_flash.assistant_message_id == wqn::kInvalidChatMessageId) {
        g_flash.assistant_message_id =
            history.AppendAssistant(g_flash.assistant_text, now_ms);
        return g_flash.assistant_message_id != wqn::kInvalidChatMessageId;
    }
    return history.ReplaceText(g_flash.assistant_message_id,
                               wqn::ChatMessageKind::kAssistant,
                               g_flash.assistant_text, now_ms);
}

void ResetFlashTurnAssemblyLocked()
{
    g_flash.user_transcript.clear();
    g_flash.assistant_text.clear();
    g_flash.thinking_text.clear();
    g_flash.history_user_committed = false;
    g_flash.thinking_done = false;
    g_flash.turn_done_received = false;
    g_flash.thinking_message_id = wqn::kInvalidChatMessageId;
    g_flash.assistant_message_id = wqn::kInvalidChatMessageId;
}

void MarkChanged()
{
    g_flash.changed = true;
}

void SetErrorLocked(const std::string& message)
{
    g_flash.status = InternalStatus::kError;
    g_flash.pending_text.clear();
    g_flash.error_message = message;
    g_flash.response_in_flight = false;  // [inflight-fix] error ends any in-flight response
    g_flash.response_started = false;
    g_flash.status_since_ms = esp_timer_get_time() / 1000;
    MarkChanged();
}

[[maybe_unused]] std::string EncodeBase64(const uint8_t* data, size_t len)
{
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);

    for (size_t i = 0; i + 2 < len; i += 3) {
        const uint8_t a = data[i];
        const uint8_t b = data[i + 1];
        const uint8_t c = data[i + 2];
        out.push_back(chars[(a >> 2) & 0x3F]);
        out.push_back(chars[((a << 4) | (b >> 4)) & 0x3F]);
        out.push_back(chars[((b << 2) | (c >> 6)) & 0x3F]);
        out.push_back(chars[c & 0x3F]);
    }

    const size_t rem = len % 3;
    if (rem == 1) {
        const uint8_t a = data[len - 1];
        out.push_back(chars[(a >> 2) & 0x3F]);
        out.push_back(chars[(a << 4) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const uint8_t a = data[len - 2];
        const uint8_t b = data[len - 1];
        out.push_back(chars[(a >> 2) & 0x3F]);
        out.push_back(chars[((a << 4) | (b >> 4)) & 0x3F]);
        out.push_back(chars[(b << 2) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// Build a v2 binary uplink frame: little-endian header (24 B) + audio bytes.
struct AudioFrameHeader {
    uint32_t magic;        // 'WFLV'
    uint16_t version;      // 2
    uint16_t flags;        // bit 0: streaming, bit 1: end-of-turn
    uint32_t seq;
    uint32_t sample_rate;  // 24000
    uint32_t channels;     // 1
    uint32_t reserved;     // 0
};

void WriteLE16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xff);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void WriteLE32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xff);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

void BuildV2AudioFrame(std::vector<uint8_t>* frame, const uint8_t* pcm,
                       size_t pcm_size, uint32_t seq, bool final)
{
    AudioFrameHeader h{};
    h.magic = kAudioFrameMagic;
    h.version = kFrameVersion;
    h.flags = final ? (kFlagStream | kFlagFinal) : kFlagStream;
    h.seq = seq;
    h.sample_rate = static_cast<uint32_t>(kSampleRate);
    h.channels = 1;
    h.reserved = 0;
    const size_t base = frame->size();
    frame->resize(base + sizeof(AudioFrameHeader) + pcm_size);
    uint8_t* p = frame->data() + base;
    WriteLE32(p, h.magic);     p += 4;
    WriteLE16(p, h.version);   p += 2;
    WriteLE16(p, h.flags);     p += 2;
    WriteLE32(p, h.seq);       p += 4;
    WriteLE32(p, h.sample_rate); p += 4;
    WriteLE32(p, h.channels);  p += 4;
    WriteLE32(p, h.reserved);  p += 4;
    if (pcm_size > 0) {
        std::memcpy(p, pcm, pcm_size);
    }
}

[[maybe_unused]] std::vector<uint8_t> DecodeBase64Chunk(const char* encoded, size_t len)
{
    static const int8_t decode_table[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };

    std::vector<uint8_t> out;
    out.reserve(len * 3 / 4);

    size_t i = 0;
    int64_t val = 0;
    int bits = 0;

    while (i < len) {
        const char c = encoded[i];
        if (c == '=' || c == '\0') {
            break;
        }
        const int8_t v = decode_table[static_cast<unsigned char>(c)];
        if (v < 0) {
            ++i;
            continue;
        }
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(val >> bits));
            val &= ((1 << bits) - 1);
        }
        ++i;
    }
    return out;
}

// ============================================================================
// Audio streaming hardware (independent from audio_capture batch module)
// ============================================================================

void ParseAndHandleEvent(const char* data, size_t len);

// Six 256-frame descriptors require 6,216 bytes of explicit DMA memory per
// direction (6 * (256 stereo-s16 frames * 4 bytes + 12-byte descriptor)). At
// the observed 12-13 KiB Flash admission point, TX can consume its half and
// deterministically starve RX. Six 128-frame descriptors retain 32 ms of audio
// per direction at 24 kHz while reducing the explicit duplex DMA request from
// 12,432 to 6,288 bytes.
constexpr uint32_t kStreamDmaDescNum = 6;
constexpr uint32_t kStreamDmaFrameNum = 128;
// The duplex stream is stereo 16-bit, so one DMA frame is four bytes.  Keep
// this alongside the channel configuration: ResetAudioTxChannel preloads the
// complete TX descriptor ring with silence after a barge-in/abort.
constexpr size_t kStreamTxDmaBytes =
    kStreamDmaDescNum * static_cast<size_t>(kStreamDmaFrameNum) *
    sizeof(int16_t) * 2U;
constexpr int kStreamChunkFrames = 360;    // 15 ms at 24 kHz
constexpr int kStreamChunkBytes = kStreamChunkFrames * 2;  // 16-bit mono
constexpr int64_t kAmpIdleTailMs = 600;    // turn amp off this long after last audio-delta write
constexpr TickType_t kI2sClockWarmup = pdMS_TO_TICKS(20);
constexpr TickType_t kCodecWarmup = pdMS_TO_TICKS(250);
constexpr size_t kCodecWarmupFrames = kSampleRate / 4;

void LogFlashHeapPoint(const char* point)
{
    wqn::services::AudioChannelHandle tx = nullptr;
    wqn::services::AudioChannelHandle rx = nullptr;
    TaskHandle_t playback_task = nullptr;
    TaskHandle_t stream_task = nullptr;
    if (g_flash.mutex != nullptr) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        tx = g_flash.stream_tx;
        rx = g_flash.stream_rx;
        playback_task = g_flash.playback_task;
        stream_task = g_flash.stream_task;
        xSemaphoreGive(g_flash.mutex);
    } else {
        tx = g_flash.stream_tx;
        rx = g_flash.stream_rx;
        playback_task = g_flash.playback_task;
        stream_task = g_flash.stream_task;
    }
    ESP_LOGI(
        kTag,
        "[flash-heap] point=%s dma_free=%u dma_largest=%u internal_free=%u "
        "internal_largest=%u desc=%u frames=%u bytes_per_frame=%u tx=%p "
        "rx=%p tx_retained=%d rx_retained=%d playback_task=%p stream_task=%p",
        point,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(kStreamDmaDescNum),
        static_cast<unsigned>(kStreamDmaFrameNum),
        static_cast<unsigned>(sizeof(int16_t) * 2U), tx, rx,
        tx != nullptr, rx != nullptr, playback_task, stream_task);
}

// [playback-fix] FreeRTOS ringbuffer for decoded downlink PCM. Sized for
// ~11 s of 24 kHz mono audio = 524288 bytes (512 KB). Cloud TTS (StepFun)
// generates faster than realtime (~3-4x); this buffer absorbs burst delivery
// while the DAC plays at 1x. ESP32-S3 has 8 MB PSRAM — 256 KB is safe.
// Byte-mode ringbuffer is required so we can carry an arbitrary byte slice
// (the v2 downlink frames arrive in 24-byte-header + variable-length PCM chunks).
constexpr size_t kPlaybackRingbufBytes = 524288;
constexpr size_t kPlaybackRingbufItemMax = 4096;  // matches an average downlink chunk

// [amp-fix] Was: amp was hard-tied to power and forced off, so response.audio.delta
// PCM data went into I2S TX but the user heard nothing (ES8311 line-out disabled).
// Now amp is independent: off during capture (avoid feedback), on while the server
// is streaming audio out, off after a short idle tail so consecutive spoken turns
// don't accumulate click/pop artifacts. AudioService remains the only GPIO46
// owner and validates the active Flash session capability.
void SetStreamAudioAmp(bool enabled)
{
    if (!g_flash_audio_session) {
        return;
    }
    const esp_err_t result = wqn::services::SetAudioAmplifier(
        g_flash_audio_session, enabled);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "set Flash amplifier=%d failed: %s",
                 enabled ? 1 : 0, esp_err_to_name(result));
    }
}

// Turn the amp off only after physical playback has finished and its idle tail
// has elapsed. Server response completion is not a playback-completion signal.
void CheckAmpIdleTail()
{
    if (g_flash.mutex == nullptr) {
        return;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    if (g_flash.amp_idle_armed) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const bool queue_empty =
            g_flash.playback_queued_bytes.load(std::memory_order_acquire) == 0;
        if (!g_flash.playback_write_active && queue_empty &&
            now_ms >= g_flash.amp_idle_due_ms) {
            g_flash.amp_idle_armed = false;
            SetStreamAudioAmp(false);
        }
    }
    xSemaphoreGive(g_flash.mutex);
}

// [playback-fix] Decode PCM (mono s16le) into the playback ringbuffer.
//
// Called from two places:
//   1. v1 WS event handler for `response.audio.delta` (base64 + mono audio).
//   2. v2 WS event handler for binary downlink frames (post header parse).
//
// Returns true if the bytes were enqueued, false if the ringbuffer is full
// and we had to drop the most recent bytes (preserving older audio so the
// user still hears the leading edge of the response).
bool EnqueuePlaybackPcm(const uint8_t* pcm, size_t bytes)
{
    if (g_flash.playback_ringbuf == nullptr || pcm == nullptr || bytes == 0) {
        return false;
    }
    const size_t aligned = bytes & ~static_cast<size_t>(1);
    if (aligned == 0) {
        return false;
    }
    // No-blocking send; if there is no room we drop the *newest* chunk. Older
    // queued audio plays through so the start of the response is preserved.
    const BaseType_t ok = xRingbufferSend(g_flash.playback_ringbuf,
                                          pcm, aligned, 0);
    if (ok != pdTRUE) {
        g_flash.playback_dropped_bytes += aligned;
        ESP_LOGW(kTag, "playback ringbuf full, dropped %u B (total %u)",
                 static_cast<unsigned>(aligned),
                 static_cast<unsigned>(g_flash.playback_dropped_bytes));
        return false;
    }
    g_flash.playback_queued_bytes.fetch_add(aligned, std::memory_order_relaxed);
    return true;
}

// [playback-fix] Reset the playback ringbuffer. Used when the user barge-ins
// or when the session transitions to idle/error, so queued audio is dropped
// immediately and the next response starts from silence.
void DrainPlaybackRingbuf()
{
    // The playback task is the only ringbuffer consumer. Request a drain and
    // remember that any write currently returning must not re-arm the PA tail.
    g_flash.drain_playback.store(true, std::memory_order_release);
    if (g_flash.mutex != nullptr) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.playback_abort_requested = true;
        g_flash.amp_idle_armed = false;
        xSemaphoreGive(g_flash.mutex);
    }
}

// [playback-fix] Independent FreeRTOS task that drains the ringbuffer and
// writes to I2S. Runs on core 1 alongside the streaming (capture) task so
// neither path competes with the UI loop on core 0.
void FlashPlaybackTask(void* /*param*/)
{
    ESP_LOGI(kTag, "flash playback task started");
    std::vector<int16_t> stereo;  // reused scratch buffer
    stereo.reserve(kPlaybackRingbufItemMax);
    while (true) {
        // [i2s-handoff] StopFlashSession sets this before deleting stream_tx.
        // The receive below uses a 100 ms timeout so we re-check this flag
        // promptly even when idle (blocked on the ringbuf). A blocked
        // i2s_channel_write is unblocked separately by i2s_channel_disable
        // in TearDownStreamChannels.
        if (g_flash.playback_stop.load(std::memory_order_relaxed)) {
            ESP_LOGI(kTag, "flash playback task exiting (stop)");
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.playback_task = nullptr;
            xSemaphoreGive(g_flash.mutex);
            vTaskDelete(nullptr);
        }
        // Interrupts must be honored even while the duplex TX channel does
        // not exist yet (warmup): dropping queued PCM and resetting the
        // PA/amp bookkeeping needs no I2S handle. Keeping this above the
        // stream_tx guard means a stop pressed during first-turn codec init
        // drains the queue instead of waiting for audio to come up.
        if (g_flash.drain_playback.load(std::memory_order_relaxed)) {
            size_t drain_size = 0;
            char* drain_item = nullptr;
            while ((drain_item = static_cast<char*>(xRingbufferReceive(
                           g_flash.playback_ringbuf, &drain_size, 0))) != nullptr) {
                vRingbufferReturnItem(g_flash.playback_ringbuf, drain_item);
            }
            g_flash.playback_queued_bytes.store(0, std::memory_order_release);
            g_flash.drain_playback.store(false, std::memory_order_relaxed);
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.playback_write_active = false;
            g_flash.playback_abort_requested = false;
            g_flash.amp_idle_armed = false;
            xSemaphoreGive(g_flash.mutex);
            continue;
        }
        if (g_flash.stream_tx == nullptr) {
            // PCM cannot be played until the duplex TX channel exists.
            // Wait and retry, leaving items in the ring buffer.
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t item_size = 0;
        char* item = static_cast<char*>(xRingbufferReceiveUpTo(
            g_flash.playback_ringbuf, &item_size, pdMS_TO_TICKS(100),
            kPlaybackRingbufItemMax));
        if (item == nullptr) {
            continue;
        }
        // If playback was explicitly interrupted, do not leave the PA state
        // marked active while discarding the current and queued PCM.
        if (g_flash.drain_playback.load(std::memory_order_relaxed)) {
            // A byte-mode RingBuffer item remains owned by the receiver until
            // it is returned.  Do that before receiving any more items: the
            // FreeRTOS ringbuffer does not permit a second receive while an
            // item is outstanding, and doing so can stall/corrupt the drain.
            vRingbufferReturnItem(g_flash.playback_ringbuf, item);
            item = nullptr;
            size_t drain_size = 0;
            char* drain_item = nullptr;
            while ((drain_item = static_cast<char*>(xRingbufferReceive(
                           g_flash.playback_ringbuf, &drain_size, 0))) != nullptr) {
                vRingbufferReturnItem(g_flash.playback_ringbuf, drain_item);
            }
            g_flash.playback_queued_bytes.store(0, std::memory_order_release);
            g_flash.drain_playback.store(false, std::memory_order_relaxed);
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.playback_write_active = false;
            g_flash.playback_abort_requested = false;
            g_flash.amp_idle_armed = false;
            xSemaphoreGive(g_flash.mutex);
            continue;
        }

        const size_t sample_count = item_size / sizeof(int16_t);
        const int16_t* mono = reinterpret_cast<const int16_t*>(item);

        // [tts-diag] Downlink PCM level (mirrors uplink pcm diag). If max is
        // full-scale (~32767) -> noisy source data (StepFun content issue). If
        // speech level (few thousand) -> data OK (points to ES8311 analog domain).
        static int downlink_diag_counter = 0;
        if (downlink_diag_counter++ % 10 == 0) {
            int16_t max_sample = 0;
            int64_t sum_sq = 0;
            for (size_t i = 0; i < sample_count; ++i) {
                int16_t s = mono[i];
                int16_t a = s < 0 ? static_cast<int16_t>(-s) : s;
                if (a > max_sample) max_sample = a;
                sum_sq += static_cast<int64_t>(s) * s;
            }
            int32_t rms = sample_count > 0
                ? static_cast<int32_t>(std::sqrt(static_cast<double>(sum_sq) / sample_count))
                : 0;
            // max=peak, rms=energy. rms/max>0.5 -> noisy data (StepFun content).
            // rms/max<0.3 -> clean speech (DAC analog issue).
            ESP_LOGI(kTag, "downlink pcm diag: frames=%u max=%d rms=%d",
                     static_cast<unsigned>(sample_count), max_sample, rms);
        }

        // Mark the full blocking write as active before opening the PA. Do not
        // start the idle tail yet: a byte-buffer receive can represent much more
        // than 600 ms of PCM, and another task must not close GPIO46 mid-write.
        {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            if (g_flash.playback_abort_requested ||
                g_flash.drain_playback.load(std::memory_order_acquire)) {
                g_flash.playback_write_active = false;
                g_flash.amp_idle_armed = false;
                xSemaphoreGive(g_flash.mutex);
                vRingbufferReturnItem(g_flash.playback_ringbuf, item);
                continue;
            }
            g_flash.playback_write_active = true;
            g_flash.amp_idle_armed = false;
            xSemaphoreGive(g_flash.mutex);
        }
        SetStreamAudioAmp(true);

        // [hw-volume] PCM sent at 100% - volume is the ES8311 DAC register
        // (0x32/0x31) set by AudioService's duplex profile, not software scaling
        // (which ruined SNR + quantization + log feel).
        stereo.resize(sample_count * 2);
        for (size_t i = 0; i < sample_count; ++i) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        // A bounded 250 ms write keeps normal 85 ms audio blocks lossless but
        // still gives emergency PrepareSleep a finite drain bound. Looping
        // handles short writes without allowing a portMAX_DELAY operation to
        // pin the audio Lease forever.
        const size_t total_bytes = stereo.size() * sizeof(int16_t);
        // GPIO46 must cover the samples already queued in I2S DMA as well as
        // the blocking write itself. At 24 kHz stereo s16, 6 x 256 DMA frames
        // hold about 64 ms; add that to the post-write PA tail.
        constexpr int64_t kStreamDmaTailMs =
            (static_cast<int64_t>(kStreamDmaDescNum) *
                 kStreamDmaFrameNum * 1000 +
             kSampleRate - 1) /
            kSampleRate;
        const int64_t audio_duration_ms =
            (static_cast<int64_t>(sample_count) * 1000 + kSampleRate - 1) /
            kSampleRate;
        size_t total = 0;
        int64_t write_start_us = esp_timer_get_time();
        esp_err_t last_werr = ESP_OK;
        while (total < total_bytes) {
            size_t written = 0;
            last_werr = wqn::services::WriteAudioChannel(
                g_flash_audio_session, g_flash.stream_tx,
                reinterpret_cast<const uint8_t*>(stereo.data()) + total,
                total_bytes - total, &written, pdMS_TO_TICKS(250));
            if (last_werr != ESP_OK || written == 0) {
                break;  // channel disabled/closed on stop - drop the rest
            }
            total += written;
        }
        // The write duration should track the PCM duration at 1x. With the
        // 4096-byte mono receive cap this is about 85 ms per block.
        int64_t write_ms = (esp_timer_get_time() - write_start_us) / 1000;
        static int write_log_counter = 0;
        if (write_log_counter++ % 10 == 0 || write_ms > 200) {
            ESP_LOGI(kTag, "i2s write: ms=%lld total=%u werr=%s",
                     static_cast<long long>(write_ms),
                     static_cast<unsigned>(total), esp_err_to_name(last_werr));
        }
        vRingbufferReturnItem(g_flash.playback_ringbuf, item);

        size_t queued = g_flash.playback_queued_bytes.load(std::memory_order_relaxed);
        if (queued >= item_size) {
            g_flash.playback_queued_bytes.fetch_sub(item_size, std::memory_order_release);
        } else {
            g_flash.playback_queued_bytes.store(0, std::memory_order_release);
        }

        // Arm the PA deadline after this write. `i2s_channel_write` returns when
        // data is copied to DMA, so include this block's audio duration and the
        // DMA reservoir before applying the idle tail. If another block follows,
        // it cancels/replaces this deadline while keeping GPIO46 high.
        {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.playback_write_active = false;
            if (g_flash.playback_abort_requested) {
                g_flash.playback_abort_requested = false;
                g_flash.amp_idle_armed = false;
            } else {
                g_flash.amp_idle_due_ms = esp_timer_get_time() / 1000 +
                                          audio_duration_ms + kStreamDmaTailMs +
                                          kAmpIdleTailMs;
                g_flash.amp_idle_armed = true;
            }
            xSemaphoreGive(g_flash.mutex);
        }
    }
}

esp_err_t EnsurePlaybackRingbuf()
{
    // [lock-fix] Guard with mutex to prevent double-alloc / double-task-create
    // if StartFlashSession is called rapidly (e.g. error recovery).
    if (g_flash.mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    if (g_flash.playback_ringbuf == nullptr) {
        g_flash.playback_ringbuf = xRingbufferCreate(
            kPlaybackRingbufBytes, RINGBUF_TYPE_BYTEBUF);
        if (g_flash.playback_ringbuf == nullptr) {
            ESP_LOGE(kTag, "playback ringbuf alloc failed (%u B)",
                     static_cast<unsigned>(kPlaybackRingbufBytes));
        }
    }
    if (g_flash.playback_ringbuf != nullptr && g_flash.playback_task == nullptr) {
        size_t stale_size = 0;
        void* stale_item = nullptr;
        while ((stale_item = xRingbufferReceive(
                    g_flash.playback_ringbuf, &stale_size, 0)) != nullptr) {
            vRingbufferReturnItem(g_flash.playback_ringbuf, stale_item);
        }
        g_flash.playback_queued_bytes.store(0, std::memory_order_release);
        g_flash.drain_playback.store(false, std::memory_order_release);
        g_flash.playback_abort_requested = false;
        g_flash.playback_write_active = false;
        g_flash.amp_idle_armed = false;
        g_flash.playback_stop.store(false, std::memory_order_relaxed);  // [i2s-handoff] clear stale stop from a prior StopFlashSession
        StackType_t* stack = EnsurePsramTaskStack(
            &g_playback_task_stack, kPlaybackTaskStackBytes,
            "flash_playback");
        g_flash.playback_task = stack == nullptr ? nullptr
            : xTaskCreateStaticPinnedToCore(
                FlashPlaybackTask, "flash_playback",
                kPlaybackTaskStackBytes, nullptr, 10, stack,
                &g_playback_task_tcb, 1);
        if (g_flash.playback_task == nullptr) {
            ESP_LOGE(kTag, "flash playback task create failed");
        }
    }
    const esp_err_t result =
        g_flash.playback_ringbuf != nullptr && g_flash.playback_task != nullptr
        ? ESP_OK : ESP_ERR_NO_MEM;
    xSemaphoreGive(g_flash.mutex);
    return result;
}

esp_err_t InitStreamEs8311Adc(wqn::services::AudioBusHandle bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return wqn::services::ConfigureAudioCodec(
        g_flash_audio_session,
        wqn::services::AudioCodecProfile::kDuplex,
        wqn::GetPlaybackVolumePercent());
}

esp_err_t InitStreamI2sDuplex(
    wqn::services::AudioChannelHandle* rx_handle)
{
    if (rx_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*rx_handle != nullptr) {
        // [inflight-fix] Channels are常驻 (created once, never deleted/disabled).
        // They stay enabled across turns - no re-enable needed (i2s_channel_enable
        // on an already-enabled channel returns ESP_ERR_INVALID_STATE). Just
        // return OK; the RX/TX are still live from the first turn.
        return ESP_OK;
    }
    // [duplex-fix] Create BOTH RX and TX channels on I2S_NUM_0 together.
    // This mirrors the official firmware's CreateDuplexChannels pattern and
    // prevents the port-occupied conflict that occurs when audio_player.cpp
    // later tries to create a standalone TX on the same I2S port.
    return wqn::services::CreateAudioDuplexChannels(
        g_flash_audio_session, kSampleRate, kStreamDmaFrameNum, true,
        rx_handle, &g_flash.stream_tx);
}

esp_err_t WarmupStreamAdc()
{
    if (g_flash.stream_rx == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // Match the board reference: let the analog/reference path settle after
    // codec open, then actively drain 250 ms of ADC data before TLS starts.
    vTaskDelay(kCodecWarmup);
    uint8_t buffer[kStreamChunkBytes * 2] = {};
    size_t discarded_frames = 0;
    while (discarded_frames < kCodecWarmupFrames) {
        size_t bytes_read = 0;
        const esp_err_t read_result = wqn::services::ReadAudioChannel(
            g_flash_audio_session, g_flash.stream_rx,
            buffer, sizeof(buffer), &bytes_read,
            pdMS_TO_TICKS(200));
        if (read_result != ESP_OK) {
            ESP_LOGE(kTag,
                     "Flash ADC warmup read failed: result=%s (%d) rx=%p discarded_frames=%u",
                     esp_err_to_name(read_result),
                     static_cast<int>(read_result), g_flash.stream_rx,
                     static_cast<unsigned>(discarded_frames));
            return read_result;
        }
        if (bytes_read == 0) {
            ESP_LOGE(kTag,
                     "Flash ADC warmup made no progress: rx=%p discarded_frames=%u",
                     g_flash.stream_rx,
                     static_cast<unsigned>(discarded_frames));
            return ESP_ERR_INVALID_STATE;
        }
        discarded_frames += bytes_read / (sizeof(int16_t) * 2U);
    }
    ESP_LOGI(kTag, "Flash audio hardware warm: discarded_frames=%u",
             static_cast<unsigned>(discarded_frames));
    return ESP_OK;
}

esp_err_t PrepareStreamHardware()
{
    ESP_LOGI(kTag,
             "Flash audio reserve begin: dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    ESP_RETURN_ON_ERROR(
        wqn::services::GetSharedAudioBus(
            g_flash_audio_session, &g_flash.stream_i2c_bus),
        kTag, "get Flash shared I2C bus");
    ESP_RETURN_ON_ERROR(
        InitStreamI2sDuplex(&g_flash.stream_rx),
        kTag, "reserve Flash duplex I2S");
    vTaskDelay(kI2sClockWarmup);
    ESP_RETURN_ON_ERROR(
        InitStreamEs8311Adc(g_flash.stream_i2c_bus),
        kTag, "configure Flash ES8311 duplex profile");
    ESP_RETURN_ON_ERROR(WarmupStreamAdc(), kTag, "warm Flash audio path");
    ESP_LOGI(kTag,
             "Flash audio reserve complete: dma_free=%u dma_largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    return ESP_OK;
}

void CleanupStreamHardware()
{
    // Per-turn capture cleanup only drops the borrowed bus pointer. Keep the
    // duplex channels enabled for the response playback that follows capture;
    // session-level TearDownStreamChannels releases them before another audio
    // activity may claim I2S_NUM_0. AudioService owns the rail and amplifier.
    if (g_flash.stream_i2c_bus != nullptr) {
        g_flash.stream_i2c_bus = nullptr;
    }
    g_flash.stream_audio_powered = false;
    // Capture cleanup deliberately leaves GPIO46 to FlashPlaybackTask. TTS can
    // already be queued when the capture task exits, so closing the PA here
    // would race with the first playback block.
}

void FinishAudioStreamingTask(const char* error_message = nullptr)
{
    CleanupStreamHardware();
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    g_flash.capture_started = false;
    g_flash.stream_task = nullptr;
    if (error_message != nullptr) {
        SetErrorLocked(error_message);
    }
    xSemaphoreGive(g_flash.mutex);
    if (error_message != nullptr) {
        RequestFlashTerminalStop(FlashTerminalReason::kTransportError);
    }
    vTaskDelete(nullptr);
}

// [i2s-handoff] Session-level teardown of the duplex I2S channels. Unlike
// CleanupStreamHardware (per-turn, keeps channels常驻 so TTS can play after
// capture stops), this DELETES stream_rx/stream_tx so STD/Pro can claim
// I2S_NUM_0. Called only from StopFlashSession, AFTER StopAudioStreaming + WS
// destroy, so AudioStreamingTask is no longer reading stream_rx. The persistent
// FlashPlaybackTask (writer of stream_tx) is stopped here first.
esp_err_t TearDownStreamChannels()
{
    const wqn::services::AudioChannelHandle tx_before = g_flash.stream_tx;
    const wqn::services::AudioChannelHandle rx_before = g_flash.stream_rx;
    esp_err_t tx_disable_result = ESP_OK;
    esp_err_t tx_delete_result = ESP_OK;
    esp_err_t rx_disable_result = ESP_OK;
    esp_err_t rx_delete_result = ESP_OK;
    // 1. Stop FlashPlaybackTask so stream_tx has no writer. drain_playback
    //    discards in-flight PCM; playback_stop makes the task self-exit at its
    //    loop top (polled every 100 ms). Disabling stream_tx first breaks any
    //    blocked i2s_channel_write (returns INVALID_STATE) so the task unblocks
    //    even mid-write.
    if (g_flash.stream_tx != nullptr) {
        tx_disable_result = wqn::services::DisableAudioChannel(
            g_flash_audio_session, g_flash.stream_tx);
    }
    if (g_flash.playback_task != nullptr) {
        g_flash.drain_playback.store(true, std::memory_order_relaxed);
        g_flash.playback_stop.store(true, std::memory_order_relaxed);
        for (int i = 0; i < 40; ++i) {  // up to 400 ms (4x the 100 ms receive poll)
            vTaskDelay(pdMS_TO_TICKS(10));
            if (g_flash.playback_task == nullptr) {
                break;
            }
        }
    }

    // 2. Delete stream_tx (safe now: playback task stopped). If the playback
    //    task somehow didn't exit, skip to avoid use-after-free on stream_tx.
    if (g_flash.playback_task == nullptr && g_flash.stream_tx != nullptr) {
        tx_delete_result = wqn::services::DeleteAudioChannel(
            g_flash_audio_session, &g_flash.stream_tx);
    } else if (g_flash.playback_task != nullptr) {
        ESP_LOGE(kTag, "playback task still alive; skipping stream_tx teardown (UAF risk)");
    }

    // 3. AudioStreamingTask (reader of stream_rx) must be gone. StopFlashSession
    //    ran StopAudioStreaming + WS destroy before us, so stream_task should be
    //    null. If it isn't, skip stream_rx teardown - deleting it while the task
    //    may read would UAF.
    TaskHandle_t stream_task = nullptr;
    if (g_flash.mutex != nullptr) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        stream_task = g_flash.stream_task;
        xSemaphoreGive(g_flash.mutex);
    }
    if (stream_task == nullptr && g_flash.stream_rx != nullptr) {
        rx_disable_result = wqn::services::DisableAudioChannel(
            g_flash_audio_session, g_flash.stream_rx);
        rx_delete_result = wqn::services::DeleteAudioChannel(
            g_flash_audio_session, &g_flash.stream_rx);
    } else if (stream_task != nullptr) {
        ESP_LOGE(kTag, "stream task still alive; skipping stream_rx teardown (UAF risk)");
    }

    const char* tx_disposition = tx_before == nullptr
        ? "absent"
        : (g_flash.stream_tx == nullptr ? "released" : "retained");
    const char* rx_disposition = rx_before == nullptr
        ? "absent"
        : (g_flash.stream_rx == nullptr ? "released" : "retained");
    ESP_LOGI(
        kTag,
        "stream I2S teardown: tx=%s rx=%s tx_before=%p tx_after=%p "
        "rx_before=%p rx_after=%p tx_disable=%s tx_delete=%s "
        "rx_disable=%s rx_delete=%s",
        tx_disposition, rx_disposition, tx_before, g_flash.stream_tx,
        rx_before, g_flash.stream_rx,
        esp_err_to_name(tx_disable_result), esp_err_to_name(tx_delete_result),
        esp_err_to_name(rx_disable_result), esp_err_to_name(rx_delete_result));
    LogFlashHeapPoint("H-after-teardown");
    return g_flash.stream_tx == nullptr && g_flash.stream_rx == nullptr
        ? ESP_OK
        : ESP_ERR_INVALID_STATE;
}

void AudioStreamingTask(void* param)
{
    (void)param;
    uint8_t i2s_buf[kStreamChunkBytes * 2];  // stereo, 2 bytes/sample
#if CONFIG_WQN_FLASH_PROTOCOL_V2
    // Reuse one PSRAM-backed frame allocation for the whole session instead
    // of allocating/freeing a 744-byte vector every 15 ms.
    std::vector<uint8_t> uplink_frame;
    uplink_frame.reserve(sizeof(AudioFrameHeader) + kStreamChunkBytes);
#endif
    uint32_t uplink_append_count = 0;
    if (g_flash.stream_rx == nullptr || g_flash.stream_tx == nullptr) {
        ESP_LOGW(kTag, "stream hardware missing after preflight");
        FinishAudioStreamingTask("音频通道初始化失败");
        return;
    }

    ESP_LOGI(kTag,
             "audio streaming task started: stack=psram dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u stack_hwm=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    // The codec was warmed before WebSocket/TLS allocation. Drain the live RX
    // ring once more here so a long session handshake cannot prepend stale
    // pre-session samples to the user's turn.
    for (int warmup = 0; warmup < 4; ++warmup) {
        size_t warmup_bytes = 0;
        wqn::services::ReadAudioChannel(
            g_flash_audio_session, g_flash.stream_rx, i2s_buf,
            sizeof(i2s_buf), &warmup_bytes, pdMS_TO_TICKS(100));
    }

    while (true) {
        // Check if capture should stop
        bool should_capture = false;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        should_capture = g_flash.capture_started;
        xSemaphoreGive(g_flash.mutex);
        if (!should_capture) {
            break;
        }

        // Read one chunk (stereo 16-bit, 240 frames = 960 bytes per channel)
        size_t bytes_read = 0;
        // [timeout-fix] 200ms (was 20ms). i2s_channel_read returns immediately
        // when DMA data is ready, so a larger timeout adds zero normal-case
        // latency - it only bounds the worst case. 20ms was too tight: 960 bytes
        // @ 64 B/ms needs 15ms to fill, and EPD/WS preemption of this priority-10
        // task left no margin -> ~98% false timeouts + fragmented audio. 200ms
        // covers fill time + scheduling jitter; stop latency stays <200ms.
        esp_err_t read_err = wqn::services::ReadAudioChannel(
            g_flash_audio_session, g_flash.stream_rx, i2s_buf,
            sizeof(i2s_buf), &bytes_read, pdMS_TO_TICKS(200));
        if (read_err != ESP_OK || bytes_read == 0) {
            if (read_err == ESP_ERR_TIMEOUT) {
                // [i2s-diag] Timeout = DMA has no data = ADC path not running.
                // Distinguishes a dead capture path (timeouts, no pcm diag)
                // from a working path with silent mic (successful reads,
                // pcm diag max_sample=0). Log every 50th to avoid spam.
                static int i2s_timeout_count = 0;
                if (++i2s_timeout_count % 50 == 1) {
                    ESP_LOGW(kTag, "I2S read timeout #%d (no DMA data; ES8311 ADC clock?)", i2s_timeout_count);
                }
            } else {
                ESP_LOGW(kTag, "I2S read error: %s", esp_err_to_name(read_err));
            }
            // [cpu-fix] Yield on read failure. Without this the task (priority 10)
            // spins at 100% retrying i2s_channel_read instantly, flooding UART and
            // drowning earlier logs (e.g. es8311 readback).
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // [channel-fix] Convert stereo interleaved to mono by averaging L+R
        // (matches audio_capture.cpp:476-484). The mic may be routed to either
        // channel; left-only (old code) read a floating pin -> max_sample=0.
        // I2S stereo 16-bit layout: [L0 R0 L1 R1 ...] (2 bytes/sample).
        const int stereo_frames = static_cast<int>(bytes_read / 4);
        const int frames = std::min(stereo_frames, kStreamChunkFrames);
        int16_t mono_buf[kStreamChunkFrames];
        const int16_t* stereo_samples = reinterpret_cast<const int16_t*>(i2s_buf);
        // [mic-fix] Mic is on the L channel; R floats (pcm diag confirms
        // L_peak=5608..13170, R_peak=0). The "R only" attempt sent silence
        // (R_peak=0 -> mono_peak=0 -> cloud got nothing). Take L only for
        // full-level mono (averaging (L+0)/2 worked but halved the level).
        for (int i = 0; i < frames; ++i) {
            mono_buf[i] = stereo_samples[i * 2];  // L channel (mic)
        }
        size_t mono_bytes = static_cast<size_t>(frames) * 2;

        // [diag] Log PCM level every ~1 second (66 chunks at 15ms = ~1s).
        // If max_sample is ~0, microphone/ADC isn't working. If >1000, audio
        // has content. This is the key diagnostic for the empty-ASR issue.
        static int diag_chunk_counter = 0;
        if (diag_chunk_counter++ % 66 == 0) {
            int16_t max_sample = 0;
            int16_t left_peak = 0, right_peak = 0;
            for (int i = 0; i < frames; ++i) {
                int16_t s = mono_buf[i];
                if (s < 0) s = static_cast<int16_t>(-s);
                if (s > max_sample) max_sample = s;
                int16_t l = static_cast<int16_t>(std::abs(static_cast<int>(stereo_samples[i * 2])));
                int16_t r = static_cast<int16_t>(std::abs(static_cast<int>(stereo_samples[i * 2 + 1])));
                if (l > left_peak) left_peak = l;
                if (r > right_peak) right_peak = r;
            }
            ESP_LOGI(kTag, "pcm diag: frames=%d L_peak=%d R_peak=%d mono_peak=%d",
                     frames, left_peak, right_peak, max_sample);
        }

        // Send via WebSocket if connected.
        // v2 uses binary frames (24-byte header + PCM); v1 fallback sends
        // {"type":"input_audio_buffer.append","audio":"<base64>"}.
        bool ws_ok = false;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        ws_ok = g_flash.ws_connected && (g_flash.ws_client != nullptr);
        xSemaphoreGive(g_flash.mutex);
        if (ws_ok) {
#if CONFIG_WQN_FLASH_PROTOCOL_V2
            uplink_frame.clear();
            const uint32_t seq = ++g_flash.uplink_seq;
            BuildV2AudioFrame(
                &uplink_frame, reinterpret_cast<const uint8_t*>(mono_buf),
                mono_bytes, seq, /*final=*/false);
            // Sustained 24 kHz PCM can briefly fill the TCP send window while
            // WiFi retransmits.  The previous 1 s deadline made that ordinary
            // backpressure fatal inside esp_websocket_client.  Use the same
            // bounded 2.5 s transport budget as STD/PRO and verify the exact
            // number of bytes accepted; never report an append that failed.
            const int64_t send_started_us = esp_timer_get_time();
            const int sent = esp_websocket_client_send_bin(
                g_flash.ws_client,
                reinterpret_cast<const char*>(uplink_frame.data()),
                uplink_frame.size(), pdMS_TO_TICKS(2500));
            const int64_t send_elapsed_ms =
                (esp_timer_get_time() - send_started_us) / 1000;
            if (sent != static_cast<int>(uplink_frame.size())) {
                ESP_LOGW(kTag,
                         "uplink send failed: seq=%u sent=%d expected=%u elapsed_ms=%lld dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u stack_hwm=%u",
                         static_cast<unsigned>(seq), sent,
                         static_cast<unsigned>(uplink_frame.size()),
                         static_cast<long long>(send_elapsed_ms),
                         static_cast<unsigned>(
                             heap_caps_get_free_size(MALLOC_CAP_DMA)),
                         static_cast<unsigned>(
                             heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                         static_cast<unsigned>(
                             heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(
                             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
                RequestFlashTerminalStop(
                    FlashTerminalReason::kTransportError);
                break;
            }
            if (send_elapsed_ms >= 100) {
                ESP_LOGW(kTag,
                         "uplink send recovered after backpressure: seq=%u bytes=%u elapsed_ms=%lld",
                         static_cast<unsigned>(seq),
                         static_cast<unsigned>(uplink_frame.size()),
                         static_cast<long long>(send_elapsed_ms));
            }
            // [i2s-diag] Confirm uplink audio is actually being sent. Paired
            // with the I2S timeout log above: if this never prints but timeouts
            // do, the capture path is dead. If this prints but StepFun still
            // says "append not called", the proxy/StepFun side is dropping them.
            ++uplink_append_count;
            if (uplink_append_count % 66 == 1) {
                ESP_LOGI(kTag,
                         "uplink append #%u seq=%u bytes=%u dma_free=%u dma_largest=%u internal_free=%u stack_hwm=%u",
                         static_cast<unsigned>(uplink_append_count), seq,
                         static_cast<unsigned>(mono_bytes),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            }
#else
            std::string b64 = EncodeBase64(reinterpret_cast<const uint8_t*>(mono_buf), mono_bytes);
            std::string msg = R"({"type":"input_audio_buffer.append","audio":"}" + b64 + R"("})";
            esp_websocket_client_send_text(g_flash.ws_client, msg.c_str(), msg.size(), pdMS_TO_TICKS(1000));
#endif
        }
    }

    ESP_LOGI(kTag, "audio streaming task exiting");
    // Clear the global handle before self-deletion so StopAudioStreaming can
    // detect every exit path, including hardware-init failures above.
    FinishAudioStreamingTask();
}

void StartAudioStreaming()
{
    if (g_flash.mutex == nullptr) {
        return;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    if (g_flash.stream_task != nullptr) {
        xSemaphoreGive(g_flash.mutex);
        return;
    }
    // The codec rail is owned by AudioService and remains warm while running.
    g_flash.stream_audio_powered = true;
    // All blocking hardware setup and warm-up completed on the lifecycle task
    // before WebSocket start. This callback path only starts the PCM worker,
    // so it never sleeps while the WebSocket client lock is held.

    // Capture starts with the speaker path closed. Do not set the drain flag
    // here: on the first turn the playback task may be blocked waiting for its
    // first item, and a stale drain request would discard that response.
    g_flash.playback_abort_requested = true;
    g_flash.amp_idle_armed = false;
    SetStreamAudioAmp(false);

    StackType_t* stack = EnsurePsramTaskStack(
        &g_stream_task_stack, kStreamTaskStackBytes, "flash_stream");
    g_flash.stream_task = stack == nullptr ? nullptr
        : xTaskCreateStaticPinnedToCore(
            &AudioStreamingTask, "flash_stream", kStreamTaskStackBytes,
            nullptr, 10, stack, &g_stream_task_tcb, 1);
    if (g_flash.stream_task == nullptr) {
        g_flash.stream_task = nullptr;
        g_flash.capture_started = false;
        SetErrorLocked("录音任务启动失败");
        ESP_LOGE(kTag, "audio streaming task create failed");
        RequestFlashTerminalStop(FlashTerminalReason::kTransportError);
    }
    xSemaphoreGive(g_flash.mutex);
}

void StopAudioStreaming()
{
    if (g_flash.mutex == nullptr) {
        return;
    }
    // Signal the task to stop by clearing the flag. Do NOT externally call
    // vTaskDelete() here — let the task delete itself after it reads the flag
    // and cleans up. Otherwise we risk a double-cleanup (task deleting itself
    // while we also try to delete it) and a double-free of hardware resources.
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    g_flash.capture_started = false;
    const TaskHandle_t task = g_flash.stream_task;
    xSemaphoreGive(g_flash.mutex);
    if (task == nullptr) {
        return;
    }
    // [panic-fix] Wait for the task to self-delete. The task can block in
    // esp_websocket_client_send_bin (1 s timeout) or i2s_channel_read (200 ms),
    // so it needs up to ~1.2 s after capture_started is cleared to exit. 3.0 s
    // covers two send_bin timeouts + jitter. If it still doesn't exit, leave it
    // alive (see below) - never vTaskDelete (corrupts FreeRTOS lists -> reboot).
    for (int i = 0; i < 300; ++i) {  // 3.0s (was 2.0s)
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        const bool stopped = g_flash.stream_task == nullptr;
        xSemaphoreGive(g_flash.mutex);
        if (stopped) {
            // Task self-deleted successfully
            return;
        }
    }
    // Task still alive — force cleanup (rare fallback for hung task)
    ESP_LOGE(kTag, "streaming task did not exit within 3s; leaving alive (will exit on WS destroy)");
    CleanupStreamHardware();
    // Do not query or delete `task` here: it may self-delete between the poll
    // and this point, making even eTaskGetState(task) a stale-handle access.
    // WebSocket destruction will unblock a still-live sender so it can exit.
    (void)task;
}

// RAII guard so each early-return path automatically frees the cJSON tree.
struct CJsonGuard {
    cJSON* p;
    ~CJsonGuard() { if (p) cJSON_Delete(p); }
};

// Convenience: read a string field, return nullptr if absent or wrong type.
const char* JsonStr(cJSON* obj, const char* key)
{
    cJSON* node = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(node) && node->valuestring != nullptr) ? node->valuestring : nullptr;
}

void ParseAndHandleEvent(const char* data, size_t len)
{
    // [flash-fix] Was: handcrafted substring matching of "type":"..." +
    // "delta":"..." etc. That guarantees miscounting on any embedded quote or
    // backslash, and realtime protocols embed user-typed text in delta fields
    // where this WILL happen. Switched to cJSON_ParseWithLength which handles
    // JSON string escapes correctly and is what every other parser in this
    // project already uses (wqn_api.cpp, word_pack.cpp).
    if (g_flash.mutex == nullptr || data == nullptr || len == 0) {
        return;
    }

    CJsonGuard g{cJSON_ParseWithLength(data, len)};
    if (g.p == nullptr) {
        ESP_LOGW(kTag, "WS event JSON parse failed (len=%u)", static_cast<unsigned>(len));
        return;
    }

    const char* type = JsonStr(g.p, "type");
    if (type == nullptr) {
        return;
    }
#if CONFIG_WQN_FLASH_PROTOCOL_V2
    const char* stage = JsonStr(g.p, "stage");
#else
    const char* stage = nullptr;
#endif

    // ============================================================
    // v2 control-frame events (text frames containing JSON).
    // ============================================================
#if CONFIG_WQN_FLASH_PROTOCOL_V2
    if (std::strcmp(type, "error") == 0) {
        // [error-fix] Surface StepFun/proxy error messages instead of relying solely
        // on the subsequent WS close. Event shape per StepFun docs:
        // {"type":"error","error":{"type":..,"code":..,"message":..}}
        std::string msg = "服务错误";
        const cJSON* err_obj = cJSON_GetObjectItem(g.p, "error");
        if (err_obj != nullptr) {
            const cJSON* m_item = cJSON_GetObjectItem(err_obj, "message");
            if (cJSON_IsString(m_item) && m_item->valuestring != nullptr && m_item->valuestring[0] != '\0') {
                msg = m_item->valuestring;
            }
        }
        ESP_LOGW(kTag, "flash session error event: %s", msg.c_str());
        // [barge-tolerate] Two StepFun errors are expected timing artifacts of
        // barge-in / fast PTT, not real failures:
        //  - "ongoing response already exists": response.create reached StepFun
        //    before the prior response.cancel was applied (cancel/create race).
        //  - "no ongoing response to cancel": response.cancel sent when no
        //    response is currently in-flight.
        // The Realtime session stays usable after both, so downgrade to a
        // warning instead of SetErrorLocked (which would flip the session to
        // kError and block further interaction until a reconnect).
        if (msg.find("ongoing response already exists") != std::string::npos ||
            msg.find("no ongoing response to cancel") != std::string::npos) {
            return;
        }
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked(msg);
        xSemaphoreGive(g_flash.mutex);
        return;
    }
    if (std::strcmp(type, "session.ready") == 0 ||
        std::strcmp(type, "session.created") == 0 ||
        std::strcmp(type, "session.updated") == 0) {
        ESP_LOGI(kTag, "session event: %s", type);
        bool should_auto_record = false;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        if (g_flash.status == InternalStatus::kConnecting ||
            g_flash.status == InternalStatus::kSessionUpdating) {
            g_flash.status = InternalStatus::kStreaming;
            g_flash.pending_text = stage ? stage : "开始对话";
            g_flash.status_since_ms = esp_timer_get_time() / 1000;
            if (g_flash.button_pressed && !g_flash.capture_started) {
                g_flash.capture_started = true;
                g_flash.pending_text = "正在录音...";
                should_auto_record = true;
            }
            MarkChanged();
        }
        xSemaphoreGive(g_flash.mutex);
        if (should_auto_record) {
            StartAudioStreaming();
        }
        return;
    }

    if (std::strcmp(type, "asr.delta") == 0 ||
        std::strcmp(type, "conversation.item.input_audio_transcription.delta") == 0) {
        const char* delta = JsonStr(g.p, "delta");
        if (delta != nullptr && delta[0] != '\0') {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.user_transcript += delta;
            g_flash.status_since_ms = esp_timer_get_time() / 1000;
            MarkChanged();
            xSemaphoreGive(g_flash.mutex);
        }
        return;
    }

    if (std::strcmp(type, "asr.complete") == 0 ||
        std::strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        const char* tr = JsonStr(g.p, "transcript");
        if (tr != nullptr) {
            const int64_t now_ms = esp_timer_get_time() / 1000;
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.user_transcript = tr;
            g_flash.pending_text.clear();
            if (!g_flash.history_user_committed && tr[0] != '\0') {
                g_flash.history_user_committed =
                    wqn::GetAiHistory(wqn::AiHistoryChannel::kFlash)
                        .AppendUser(tr, now_ms) != wqn::kInvalidChatMessageId;
            }
            // ASR establishes the ordering boundary. Any Thinking accumulated
            // before ASR now appears once, immediately after User. Do not clear
            // the accumulator; thinking.done/turn.done will replace this row.
            EnsureFlashThinkingHistoryLocked(now_ms);
            if (g_flash.turn_done_received) {
                EnsureFlashThinkingHistoryLocked(now_ms);
                FinalizeFlashAssistantLocked(now_ms);
            }
            g_flash.status_since_ms = now_ms;
            MarkChanged();
            xSemaphoreGive(g_flash.mutex);
            ESP_LOGI(kTag, "transcription: %s", tr);
        }
        return;
    }

    if (std::strcmp(type, "response.thinking.delta") == 0 ||
        std::strcmp(type, "response.reasoning.delta") == 0 ||
        std::strcmp(type, "response.reasoning_summary_text.delta") == 0) {
        const char* delta = JsonStr(g.p, "delta");
        if (delta != nullptr && delta[0] != '\0') {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            const bool first_visible =
                g_flash.history_user_committed &&
                g_flash.thinking_message_id == wqn::kInvalidChatMessageId;
            g_flash.response_started = true;
            if (!g_flash.thinking_done) {
                g_flash.thinking_text += delta;
            }
            if (first_visible) {
                // Show the first available prefix once. Later deltas only
                // accumulate; thinking.done replaces this row with full text.
                EnsureFlashThinkingHistoryLocked(esp_timer_get_time() / 1000);
                MarkChanged();
            }
            xSemaphoreGive(g_flash.mutex);
        }
        return;
    }

    if (std::strcmp(type, "response.thinking.done") == 0 ||
        std::strcmp(type, "response.reasoning.done") == 0 ||
        std::strcmp(type, "response.reasoning_summary_text.done") == 0) {
        const char* full = JsonStr(g.p, "full_text");
        if (full == nullptr || full[0] == '\0') full = JsonStr(g.p, "text");
        if (full == nullptr || full[0] == '\0') full = JsonStr(g.p, "thinking");
        if (full == nullptr || full[0] == '\0') full = JsonStr(g.p, "content");
        const int64_t now_ms = esp_timer_get_time() / 1000;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        if (full != nullptr && full[0] != '\0') {
            g_flash.thinking_text = full;
        }
        g_flash.thinking_done = true;
        EnsureFlashThinkingHistoryLocked(now_ms);
        const size_t thinking_chars = g_flash.thinking_text.size();
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);
        ESP_LOGI(kTag, "thinking done: chars=%u",
                 static_cast<unsigned>(thinking_chars));
        return;
    }

    if (std::strcmp(type, "text.delta") == 0 ||
        std::strcmp(type, "response.audio_transcript.delta") == 0) {
        const char* delta = JsonStr(g.p, "delta");
        if (delta != nullptr && delta[0] != '\0') {
            // [thinking-display] Replace the "思考中..." placeholder with the
            // final摘要 once the actual reply starts streaming. Snapshot +
            // clear thinking_text inside the lock; do the AppendThinking
            // outside the lock (matches the AppendUser/AppendAssistant pattern
            // - PMR alloc off the streaming critical section).
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.response_started = true;  // [phase-fix] current turn entered generation
            // [order-fix] Do NOT clear thinking_text or AppendThinking here.
            // thinking_text is appended as a summary when the transcription
            // arrives (asr.complete), so it lands after the user message. We
            // only accumulate assistant_text here.
            g_flash.assistant_text += delta;
            g_flash.status_since_ms = esp_timer_get_time() / 1000;
            MarkChanged();
            xSemaphoreGive(g_flash.mutex);
        }
        return;
    }

    if (std::strcmp(type, "tool.start") == 0) {
        const char* name = JsonStr(g.p, "name");
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.tool_label = std::string("🔧 ") + (name ? name : "tool") + "…";
        g_flash.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);
        return;
    }

    if (std::strcmp(type, "tool.result") == 0 || std::strcmp(type, "tool.error") == 0) {
        const char* display = JsonStr(g.p, "display");
        const char* name = JsonStr(g.p, "name");
        const bool ok = std::strcmp(type, "tool.result") == 0;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.tool_label = std::string(ok ? "✅ " : "❌ ") +
                             (display && display[0] ? display : (name ? name : "tool"));
        g_flash.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);
        return;
    }

    if (std::strcmp(type, "state") == 0) {
        const char* s = JsonStr(g.p, "stage");
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        if (s != nullptr) g_flash.pending_text = s;
        g_flash.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);
        return;
    }

    if (std::strcmp(type, "turn.done") == 0 || std::strcmp(type, "response.done") == 0) {
        ESP_LOGI(kTag, "turn done");
        const int64_t now_ms = esp_timer_get_time() / 1000;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.turn_done_received = true;
        // Finalize in strict order only after ASR established User. If ASR is
        // late, keep both accumulators; the ASR branch completes this sequence.
        if (g_flash.history_user_committed) {
            EnsureFlashThinkingHistoryLocked(now_ms);
            FinalizeFlashAssistantLocked(now_ms);
        }
        g_flash.response_in_flight = false;
        g_flash.response_started = false;
        g_flash.status_since_ms = now_ms;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);
        return;
    }
#endif  // CONFIG_WQN_FLASH_PROTOCOL_V2

    // ============================================================
    // Legacy v1 StepFun-shaped events (compile-only fallback).
    // ============================================================
#if !CONFIG_WQN_FLASH_PROTOCOL_V2
    if (std::strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        const char* tr = JsonStr(g.p, "transcript");
        if (tr != nullptr) {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.user_transcript = tr;
            g_flash.pending_text.clear();
            g_flash.status_since_ms = esp_timer_get_time() / 1000;
            MarkChanged();
            xSemaphoreGive(g_flash.mutex);
            ESP_LOGI(kTag, "transcription: %s", tr);
        }
        return;
    }

    if (std::strcmp(type, "response.audio_transcript.delta") == 0) {
        const char* delta = JsonStr(g.p, "delta");
        if (delta != nullptr && delta[0] != '\0') {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.assistant_text += delta;
            g_flash.status_since_ms = esp_timer_get_time() / 1000;
            MarkChanged();
            xSemaphoreGive(g_flash.mutex);
        }
        return;
    }

    if (std::strcmp(type, "response.audio.delta") == 0) {
        const char* delta = JsonStr(g.p, "delta");
        if (delta != nullptr && delta[0] != '\0') {
            std::vector<uint8_t> pcm = DecodeBase64Chunk(delta, std::strlen(delta));
            const size_t safe_bytes = pcm.size() & ~static_cast<size_t>(1);
            if (safe_bytes >= 2) {
                // [playback-fix] Hand decoded PCM off to the dedicated
                // playback task via ringbuffer; do NOT call i2s_channel_write
                // here — that blocked the WS event loop on slow I2S writes.
                EnqueuePlaybackPcm(pcm.data(), safe_bytes);
            }
        }
        return;
    }

    if (std::strcmp(type, "response.done") == 0) {
        ESP_LOGI(kTag, "response done");
        // Generation completion can precede local paced playback by seconds.
        // Leave GPIO46 under FlashPlaybackTask's queue-drain idle-tail control.
        return;
    }
#endif  // !CONFIG_WQN_FLASH_PROTOCOL_V2

    // ============================================================
    // Common: error events.
    // ============================================================
    if (std::strcmp(type, "error") == 0) {
        std::string err_msg;
        cJSON* err_node = cJSON_GetObjectItemCaseSensitive(g.p, "error");
        if (cJSON_IsObject(err_node)) {
            const char* nested = JsonStr(err_node, "message");
            if (nested != nullptr) err_msg = nested;
        }
        if (err_msg.empty()) {
            const char* top = JsonStr(g.p, "message");
            if (top != nullptr) err_msg = top;
        }
        if (err_msg.empty()) err_msg = "未知错误";

        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("WS 错误: " + err_msg);
        xSemaphoreGive(g_flash.mutex);
        ESP_LOGW(kTag, "WS error: %s", err_msg.c_str());
        RequestFlashTerminalStop(FlashTerminalReason::kServerError);
        return;
    }

    // [thinking-diag] Surface thinking/reasoning events at INFO so we can
    // confirm the exact event name StepFun uses at runtime (docs say
    // response.thinking.delta, but the two investigation agents disagreed on
    // thinking vs reasoning - verify at runtime). If this fires, the event
    // name needs to be added to the thinking.delta branch above. Other
    // unhandled events stay at DEBUG to avoid log spam.
    if (std::strstr(type, "thinking") != nullptr ||
        std::strstr(type, "reasoning") != nullptr) {
        ESP_LOGI(kTag, "unhandled thinking/reasoning event: %s", type);
    } else {
        ESP_LOGD(kTag, "unhandled WS event: %s", type);
    }
}

void WebsocketEventHandler(void* handler_args, esp_event_base_t, int32_t event_id, void* event_data)
{
    const auto* event = static_cast<const esp_websocket_event_data_t*>(event_data);

    switch (static_cast<esp_websocket_event_id_t>(event_id)) {
        case WEBSOCKET_EVENT_CONNECTED: {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.ws_connected = true;
            g_flash.status = InternalStatus::kSessionUpdating;
            g_flash.pending_text = "会话初始化...";
            g_flash.status_since_ms = esp_timer_get_time() / 1000;
            g_flash.uplink_seq = 0;
            MarkChanged();
            esp_websocket_client_handle_t client = g_flash.ws_client;
            xSemaphoreGive(g_flash.mutex);
            if (client == nullptr) break;

            // PCM frames arrive every 15 ms and are smaller than one TCP MSS.
            // Disable Nagle on the device-facing socket so a delayed ACK does
            // not let four small TLS records fill lwIP's 5,760-byte send
            // window before the fifth append. The relay already applies the
            // same policy to its upstream realtime socket.
            const esp_err_t nodelay_result =
                esp_websocket_client_set_tcp_nodelay(client, true);
            ESP_LOGI(kTag,
                     "WebSocket connected: tcp_nodelay=%s dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u",
                     esp_err_to_name(nodelay_result),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

#if CONFIG_WQN_FLASH_PROTOCOL_V2
            // [api-fix] session.update aligned to StepFun official Realtime API
            // docs (https://platform.stepfun.com/docs/llms.txt). Previous
            // version had three fatal errors that caused StepFun to silently
            // ignore the session config and never trigger TTS:
            //   1. Missing "modalities" (required field)
            //   2. "vad" -> should be "turn_detection"
            //   3. "mode" -> should be "type"
            // Also removed input_sample_rate / output_sample_rate (not in spec).
            // model is set by the relay's rewriteSessionUpdate, not here.
            // [tts-speed-fix] Mirror the official demo's session.update fields
            // exactly. Missing max_response_output_tokens / temperature /
            // tools / tool_choice / input_audio_transcription caused StepFun
            // to generate TTS at 0.51x with 47 x 700-1300ms stalls (vs demo's
            // 5x continuous). Hypothesis: default max_response_output_tokens
            // is small -> StepFun segments TTS into many small chunks, each
            // with a generation pause. Setting 4096 enables whole-response TTS.
            std::string session_update = std::string(R"({"type":"session.update","session":)") +
                R"({"modalities":["text","audio"],)" +
                R"("instructions":")" + std::string(kDefaultInstructions) +
                R"(","voice":")" + kDefaultVoice +
                R"(","input_audio_format":"pcm16")" +
                R"(,"output_audio_format":"pcm16","input_audio_transcription":null)" +
                R"(,"turn_detection":null,"tools":[],"tool_choice":"auto")" +
                R"(,"temperature":0.8,"max_response_output_tokens":4096}})";
#else
            std::string session_update = std::string(R"({"type":"session.update","session":)") +
                R"({"modalities":["text","audio"],"instructions":")" +
                "\u4f60\u662f\u4e2a\u4eba\u52a9\u7406\u5c0f\u4e91\uff0c\u8bf7\u7528\u53ef\u7231\u98ce\u8da3\u7684\u65b9\u5f0f\u56de\u7b54\u7528\u6237\u7684\u95ee\u9898\u3002" +
                R"(","voice":")" + kDefaultVoice +
                R"(","input_audio_format":"pcm16","output_audio_format":"pcm16")" +
                R"(,"turn_detection":{"type":"server_vad","prefix_padding_ms":500,"silence_duration_ms":200}}})";
#endif
            esp_websocket_client_send_text(client, session_update.c_str(),
                                           session_update.size(), pdMS_TO_TICKS(5000));
            break;
        }

        case WEBSOCKET_EVENT_DISCONNECTED: {
            ESP_LOGI(kTag, "WebSocket disconnected");
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.ws_connected = false;
            if (g_flash.status != InternalStatus::kError) {
                SetErrorLocked("连接断开");
            }
            xSemaphoreGive(g_flash.mutex);
            RequestFlashTerminalStop(FlashTerminalReason::kDisconnected);
            break;
        }

        case WEBSOCKET_EVENT_DATA:
            if (event->data_ptr != nullptr && event->data_len > 0) {
#if CONFIG_WQN_FLASH_PROTOCOL_V2
                // v2: text frames = control JSON, binary frames = audio.
                if (event->op_code == 0x02) {
                    HandleV2DownlinkAudio(reinterpret_cast<const uint8_t*>(event->data_ptr),
                                          static_cast<size_t>(event->data_len),
                                          static_cast<uint64_t>(event->payload_offset),
                                          static_cast<uint64_t>(event->payload_len));
                    break;
                }
                if (event->op_code != 0x01 && event->op_code != 0x00) {
                    break;
                }
#else
                if (event->op_code != 0x01 && event->op_code != 0x00) {
                    break;
                }
#endif
                // [frag-fix] Handle WebSocket frame fragmentation. When the
                // payload exceeds buffer_size, esp_websocket_client delivers
                // it across multiple WEBSOCKET_EVENT_DATA callbacks with
                // payload_offset tracking progress through the full frame.
                if (event->payload_offset == 0 &&
                    event->data_len == event->payload_len) {
                    // Complete frame in a single callback (fast path)
                    ParseAndHandleEvent(static_cast<const char*>(event->data_ptr),
                                        static_cast<size_t>(event->data_len));
                } else {
                    // Fragmented frame — accumulate into reassembly buffer
                    if (event->payload_offset == 0) {
                        // [oom-guard] Cap control-frame reassembly at 128 KB.
                        constexpr size_t kMaxWsFrameBytes = 128 * 1024;
                        if (event->payload_len > kMaxWsFrameBytes) {
                            ESP_LOGW(kTag, "WS control frame payload_len=%llu exceeds cap, dropping",
                                     static_cast<unsigned long long>(event->payload_len));
                            break;
                        }
                        g_flash.ws_reassembly_buf.clear();
                        g_flash.ws_reassembly_buf.reserve(
                            static_cast<size_t>(event->payload_len));
                    }
                    g_flash.ws_reassembly_buf.append(
                        static_cast<const char*>(event->data_ptr),
                        static_cast<size_t>(event->data_len));
                    // All fragments received — parse complete frame
                    if (event->payload_len > 0 &&
                        event->payload_offset + event->data_len >=
                        event->payload_len) {
                        ParseAndHandleEvent(g_flash.ws_reassembly_buf.data(),
                                            g_flash.ws_reassembly_buf.size());
                        g_flash.ws_reassembly_buf.clear();
                    }
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR: {
            ESP_LOGW(kTag, "WebSocket error");
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            SetErrorLocked("WebSocket 错误");
            xSemaphoreGive(g_flash.mutex);
            RequestFlashTerminalStop(FlashTerminalReason::kTransportError);
            break;
        }

        case WEBSOCKET_EVENT_CLOSED: {
            ESP_LOGI(kTag, "WebSocket closed");
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.ws_connected = false;
            if (g_flash.status != InternalStatus::kError) {
                SetErrorLocked("连接已关闭");
            }
            xSemaphoreGive(g_flash.mutex);
            RequestFlashTerminalStop(FlashTerminalReason::kDisconnected);
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// v2 binary downlink audio handler.
//
// Proxy contract: each binary frame is a 24-byte little-endian header identical
// to the uplink header but with magic 'WFLV' (we share the AudioFrameHeader
// struct) followed by PCM s16le mono samples at output_sample_rate negotiated
// in the session.update.  We decode, mix mono → stereo, and write to the
// already-running I2S TX channel from the streaming task.
//
// TODO(opus): StepAudio Realtime ships Opus-encoded audio. The proxy is
// expected to do the decode and forward PCM frames; if a future deployment
// forwards raw Opus we'll need to plug in libopus here.
// ============================================================================
#if CONFIG_WQN_FLASH_PROTOCOL_V2
void HandleV2DownlinkAudio(const uint8_t* data, size_t len,
                           uint64_t payload_offset, uint64_t payload_len)
{
    if (data == nullptr || len == 0) {
        return;
    }
    if (payload_offset == 0) {
        // [oom-guard] Cap the reassembly buffer at 64 KB. TTS audio chunks are
        // typically 2-6 KB; a malicious or buggy proxy claiming a huge
        // payload_len would otherwise exhaust PSRAM.
        constexpr size_t kMaxAudioFrameBytes = 64 * 1024;
        if (payload_len > kMaxAudioFrameBytes) {
            ESP_LOGW(kTag, "audio frame payload_len=%llu exceeds cap %u, dropping",
                     static_cast<unsigned long long>(payload_len),
                     static_cast<unsigned>(kMaxAudioFrameBytes));
            g_flash.audio_reassembly_buf.clear();  // [oom-guard] drop stale buf so later fragments don't pollute
            return;
        }
        g_flash.audio_reassembly_buf.clear();
        g_flash.audio_reassembly_buf.reserve(static_cast<size_t>(payload_len));
    }
    g_flash.audio_reassembly_buf.append(reinterpret_cast<const char*>(data), len);
    if (g_flash.audio_reassembly_buf.size() < payload_len) {
        return;  // wait for full frame
    }
    if (g_flash.audio_reassembly_buf.size() < sizeof(AudioFrameHeader)) {
        return;  // corrupt or malformed
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(g_flash.audio_reassembly_buf.data());
    AudioFrameHeader hdr{};
    std::memcpy(&hdr.magic,       p + 0,  4);
    std::memcpy(&hdr.version,     p + 4,  2);
    std::memcpy(&hdr.flags,       p + 6,  2);
    std::memcpy(&hdr.seq,         p + 8,  4);
    std::memcpy(&hdr.sample_rate, p + 12, 4);
    std::memcpy(&hdr.channels,    p + 16, 4);
    std::memcpy(&hdr.reserved,    p + 20, 4);

    if (hdr.magic != kAudioFrameMagic || hdr.version != kFrameVersion) {
        g_flash.audio_reassembly_buf.clear();
        return;
    }

    const size_t pcm_bytes = g_flash.audio_reassembly_buf.size() - sizeof(AudioFrameHeader);
    const size_t safe_bytes = pcm_bytes & ~static_cast<size_t>(1);  // align to int16
    if (safe_bytes < 2) {
        ESP_LOGD(kTag, "audio frame seq=%u has %u PCM bytes (< 1 sample), skipping",
                 static_cast<unsigned>(hdr.seq), static_cast<unsigned>(safe_bytes));
        g_flash.audio_reassembly_buf.clear();
        return;
    }

    // [playback-fix] Enqueue the post-header PCM bytes into the playback
    // ringbuffer instead of writing to I2S from the WS event task. The
    // dedicated FlashPlaybackTask drains it and writes to stream_tx.
    EnqueuePlaybackPcm(reinterpret_cast<const uint8_t*>(
                           g_flash.audio_reassembly_buf.data() + sizeof(AudioFrameHeader)),
                       safe_bytes);
    // [tts-diag] Confirm TTS audio frames reach the device. If this prints but
    // no sound, the ES8311 DAC path is the culprit (see dac-fix on 0x37); if it
    // never prints, the proxy isn't forwarding response.audio.delta as binary.
    static int downlink_frame_count = 0;
    if (++downlink_frame_count % 50 == 1) {
        ESP_LOGI(kTag, "downlink audio frame #%d seq=%u pcm_bytes=%u",
                 downlink_frame_count, (unsigned)hdr.seq, (unsigned)safe_bytes);
    }
    g_flash.audio_reassembly_buf.clear();
}
#endif  // CONFIG_WQN_FLASH_PROTOCOL_V2

}  // namespace

namespace wqn {

esp_err_t InitFlashSession()
{
    if (g_flash.mutex == nullptr) {
        g_flash.mutex = xSemaphoreCreateMutex();
    }
    if (g_flash.mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(
        EnsureFlashLifecycleTask(), kTag, "create Flash lifecycle task");
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    g_flash.status = InternalStatus::kIdle;
    g_flash.status_since_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(g_flash.mutex);
    // Flash's DMA/I2S reservation must precede dynamic playback/WebSocket task
    // allocation. StartFlashSessionNow creates the playback path after audio
    // hardware warm-up and before WebSocket start.
    return ESP_OK;
}

esp_err_t StartFlashSession()
{
    LogFlashHeapPoint("A-before-flash-session-request");
    if (g_flash.mutex == nullptr) {
        const esp_err_t init_result = InitFlashSession();
        if (init_result != ESP_OK) {
            return init_result;
        }
    }
    if (g_teardown_pending.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "Flash start deferred while prior teardown is pending");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    if (g_flash.ws_client != nullptr) {
        xSemaphoreGive(g_flash.mutex);
        return ESP_OK;
    }
    if (g_flash.status == InternalStatus::kConnecting || g_flash.status == InternalStatus::kSessionUpdating ||
        g_flash.status == InternalStatus::kStreaming) {
        xSemaphoreGive(g_flash.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation =
        g_session_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (generation == 0) {
        generation = 1;
        g_session_generation.store(generation, std::memory_order_release);
    }
    g_flash.status = InternalStatus::kConnecting;
    g_flash.pending_text = "正在连接...";
    g_flash.error_message.clear();
    g_flash.user_transcript.clear();
    g_flash.assistant_text.clear();
    g_flash.thinking_text.clear();
    g_flash.response_in_flight = false;
    g_flash.response_started = false;
    g_flash.status_since_ms = esp_timer_get_time() / 1000;
    MarkChanged();
    g_start_generation.store(generation, std::memory_order_relaxed);
    g_start_pending.store(true, std::memory_order_release);
    xSemaphoreGive(g_flash.mutex);
    xTaskNotifyGive(g_lifecycle_task);
    return ESP_OK;
}

esp_err_t StartFlashSessionNow(uint32_t generation)
{
    if (generation == 0 ||
        generation != g_session_generation.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    std::string access_token;
    esp_err_t tok_err = wqn::LoadAccessToken(&access_token);
    if (tok_err != ESP_OK || access_token.empty()) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("未登录，请先完成账号配对");
        g_flash_connectivity_demand.Reset();
        xSemaphoreGive(g_flash.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    services::ConnectivityDemand connectivity_demand =
        services::AcquireConnectivityDemand(
            services::ConnectivityDemandReason::kAiInteractive,
            "flash-session",
            __FILE__,
            __LINE__);
    if (!connectivity_demand) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("网络任务繁忙，请稍后重试");
        xSemaphoreGive(g_flash.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    services::ConnectivityDemandTicket ticket = connectivity_demand.ticket();
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    const bool admit_connect =
        generation == g_session_generation.load(std::memory_order_acquire) &&
        g_flash.status == InternalStatus::kConnecting &&
        !g_teardown_pending.load(std::memory_order_acquire);
    if (admit_connect) {
        g_flash_connectivity_demand = std::move(connectivity_demand);
        ticket = g_flash_connectivity_demand.ticket();
    }
    xSemaphoreGive(g_flash.mutex);
    if (!admit_connect) {
        return ESP_ERR_INVALID_STATE;
    }
    LogFlashHeapPoint("B-after-wifi-demand-acquire");

    const services::ConnectivityWaitResult wait_result =
        services::WaitForConnectivity(ticket, kWifiReadyWait);
    if (wait_result != services::ConnectivityWaitResult::kOnline) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        if (generation == g_session_generation.load(std::memory_order_acquire) &&
            !g_teardown_pending.load(std::memory_order_acquire)) {
            switch (wait_result) {
                case services::ConnectivityWaitResult::kNeedsProvisioning:
                    SetErrorLocked("未配置 WiFi，请先在设置中配网");
                    break;
                case services::ConnectivityWaitResult::kAuthFailed:
                    SetErrorLocked("WiFi 密码错误，请重新配网");
                    break;
                case services::ConnectivityWaitResult::kTimedOut:
                    SetErrorLocked("WiFi 连接超时，请稍后重试");
                    break;
                case services::ConnectivityWaitResult::kCancelled:
                    break;
                default:
                    SetErrorLocked("WiFi 暂时不可用，请稍后重试");
                    break;
            }
        }
        g_flash_connectivity_demand.Reset();
        xSemaphoreGive(g_flash.mutex);
        ESP_LOGW(
            kTag,
            "Flash WiFi wait failed: result=%s generation=%lu",
            services::ConnectivityWaitResultName(wait_result),
            static_cast<unsigned long>(generation));
        return services::ConnectivityWaitResultToEspErr(wait_result);
    }

    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    const bool still_current =
        generation == g_session_generation.load(std::memory_order_acquire) &&
        g_flash.status == InternalStatus::kConnecting &&
        g_flash_connectivity_demand.ticket().id == ticket.id &&
        !g_teardown_pending.load(std::memory_order_acquire);
    xSemaphoreGive(g_flash.mutex);
    if (!still_current) {
        return ESP_ERR_INVALID_STATE;
    }

    // Network association happens before I2S ownership. This keeps an absent
    // AP from pinning AudioActivity for the entire connection budget and the
    // lifecycle task, not the UI task, owns every blocking step.
    const esp_err_t playback_stop_result = wqn::StopAudioPlayback();
    if (playback_stop_result != ESP_OK) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("音频资源释放失败");
        g_flash_connectivity_demand.Reset();
        xSemaphoreGive(g_flash.mutex);
        return playback_stop_result;
    }
    const esp_err_t audio_result = wqn::services::BeginAudioActivity(
        wqn::services::AudioActivity::kFlash, &g_flash_audio_session);
    if (audio_result != ESP_OK) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("音频资源繁忙");
        g_flash_connectivity_demand.Reset();
        xSemaphoreGive(g_flash.mutex);
        return audio_result;
    }

    auto fail_after_audio_begin = [&](const char* message,
                                      esp_err_t cause) -> esp_err_t {
        SetStreamAudioAmp(false);
        esp_err_t cleanup_result = TearDownStreamChannels();
        g_flash.stream_i2c_bus = nullptr;
        g_flash.stream_audio_powered = false;
        if (cleanup_result == ESP_OK) {
            cleanup_result =
                services::EndAudioActivity(&g_flash_audio_session);
        }
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked(message);
        g_flash_connectivity_demand.Reset();
        xSemaphoreGive(g_flash.mutex);
        if (cleanup_result != ESP_OK) {
            ESP_LOGE(kTag,
                     "Flash start rollback incomplete: cause=%s cleanup=%s",
                     esp_err_to_name(cause),
                     esp_err_to_name(cleanup_result));
            return cleanup_result;
        }
        return cause;
    };

    if (g_teardown_pending.load(std::memory_order_acquire) ||
        generation != g_session_generation.load(std::memory_order_acquire)) {
        return fail_after_audio_begin(
            "Flash 启动已取消", ESP_ERR_INVALID_STATE);
    }

    // Reserve every DMA/I2S/codec resource before WebSocket/TLS can consume
    // or fragment internal memory. This also guarantees MCLK is running before
    // the ES8311 register program selects the external clock domain.
    esp_err_t result = PrepareStreamHardware();
    if (result != ESP_OK) {
        return fail_after_audio_begin("音频硬件初始化失败", result);
    }
    result = EnsurePlaybackRingbuf();
    if (result != ESP_OK) {
        return fail_after_audio_begin("音频播放任务初始化失败", result);
    }
    if (g_teardown_pending.load(std::memory_order_acquire) ||
        generation != g_session_generation.load(std::memory_order_acquire)) {
        return fail_after_audio_begin(
            "Flash 启动已取消", ESP_ERR_INVALID_STATE);
    }

    esp_websocket_client_config_t cfg = {};
#if CONFIG_WQN_FLASH_PROTOCOL_V2
    // [port-fix] ESP-IDF's host+path mode sometimes ignores cfg.port and
    // defaults to 80, causing "connecting to host ...:80" + HTTP 301
    // redirect instead of a WS upgrade on 443. Use the full URI form
    // (wss://host/path) which makes esp_websocket_client respect 443
    // implicitly from the "wss://" scheme.
    cfg.uri = "wss://" WQN_API_BASE_HOST WQN_FLASH_WS_PATH;
    cfg.subprotocol = kWsSubprotocol;
#else
    cfg.uri = kWsUri;
#endif
    cfg.network_timeout_ms = static_cast<int>(kWsConnectTimeout * portTICK_PERIOD_MS);
    // [stack-fix] Default WS task stack is 4 KB which is too small for the
    // cJSON parsing + Base64 decoding + I2S operations done inside the event
    // callback. Increase to 8 KB to prevent stack overflow panics.
    cfg.task_stack = 8192;
    // [frag-fix] Increase buffer from 4 KB to 8 KB so most response.audio.delta
    // events (typically 2-6 KB of base64) arrive in a single callback without
    // needing reassembly.
    cfg.buffer_size = 8192;
    // [tls-fix] ESP-IDF 5.x mbedtls refuses to set up SSL when the cfg has
    // no server verification source (error 0x8017 SSL_SETUP_FAILED). Attach
    // the bundled root CA store — same pattern as wqn_api.cpp's HTTP client.
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // [keepalive-fix] Reconnect fast (2s, not default 10s) so PTT retry
    // feels responsive after a momentary drop. Ping every 20s to keep the
    // connection alive through ALB/SLB idle timeouts (default 50s).
    cfg.reconnect_timeout_ms = 2000;
    cfg.ping_interval_sec = 20;

    g_flash.ws_client = esp_websocket_client_init(&cfg);
    if (g_flash.ws_client == nullptr) {
        return fail_after_audio_begin(
            "WS 客户端初始化失败", ESP_ERR_NO_MEM);
    }

    std::string bearer = "Bearer " + access_token;
    esp_websocket_client_append_header(g_flash.ws_client, "Authorization", bearer.c_str());
    esp_websocket_client_append_header(g_flash.ws_client, "Accept", "application/json");
    esp_websocket_client_append_header(g_flash.ws_client, "Content-Type", "application/json");
#if CONFIG_WQN_FLASH_PROTOCOL_V2
    esp_websocket_client_append_header(g_flash.ws_client, "X-WQN-Client-Version",
                                       WQN_FIRMWARE_NAME "@" WQN_FIRMWARE_VERSION);
    esp_websocket_client_append_header(g_flash.ws_client, "X-WQN-Protocol", WQN_FLASH_WS_SUBPROTOCOL);
#else
    // Legacy v1 StepFun-shaped deployment still gates the WS upgrade behind a
    // shared secret (nginx `if ($http_x_ws_secret != ...) return 401;`).
    esp_websocket_client_append_header(g_flash.ws_client, "X-WS-Secret", "WQN_Flash_Secret_2026");
#endif

    esp_err_t reg_err = esp_websocket_register_events(
        g_flash.ws_client, WEBSOCKET_EVENT_ANY, WebsocketEventHandler, nullptr);
    if (reg_err != ESP_OK) {
        esp_websocket_client_destroy(g_flash.ws_client);
        g_flash.ws_client = nullptr;
        return fail_after_audio_begin("WS 事件注册失败", reg_err);
    }

    result = esp_websocket_client_start(g_flash.ws_client);
    if (result != ESP_OK) {
        esp_websocket_client_destroy(g_flash.ws_client);
        g_flash.ws_client = nullptr;
        return fail_after_audio_begin("WS 连接失败", result);
    }

    return ESP_OK;
}

esp_err_t StopFlashSessionNow()
{
    if (g_flash.mutex == nullptr) {
        return ESP_OK;
    }

    // Signal streaming task to stop (outside mutex so task can read it)
    {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.capture_started = false;
        g_flash.button_pressed = false;
        bool had_stream_task = (g_flash.stream_task != nullptr);
        xSemaphoreGive(g_flash.mutex);
        if (had_stream_task) {
            StopAudioStreaming();
        }
    }

    // [playback-fix] Drop any PCM already enqueued for the playback task so
    // the user does not hear the tail of the previous response after a stop.
    DrainPlaybackRingbuf();
    SetStreamAudioAmp(false);

    // Disconnect WebSocket OUTSIDE the mutex to avoid deadlock with its event handler
    esp_websocket_client_handle_t client_to_destroy = nullptr;
    {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        if (g_flash.ws_client != nullptr) {
            client_to_destroy = g_flash.ws_client;
            g_flash.ws_client = nullptr;
            g_flash.ws_connected = false;
        }
        g_flash.status = InternalStatus::kIdle;
        g_flash.pending_text.clear();
        g_flash.tool_label.clear();
        g_flash.error_message.clear();
        g_flash.audio_reassembly_buf.clear();
        g_flash.audio_reassembly_buf.shrink_to_fit();
        g_flash.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);
    }

    // Now destroy the client (no mutex held, so WebSocket event handler can complete)
    if (client_to_destroy != nullptr) {
        esp_websocket_client_stop(client_to_destroy);
        esp_websocket_client_destroy(client_to_destroy);
    }

    // [i2s-handoff] WS destroy unblocks any AudioStreamingTask stuck in
    // esp_websocket_client_send_bin (the only thing StopAudioStreaming's 3 s
    // wait couldn't break). Wait briefly for it to self-exit, then tear down
    // the duplex I2S channels so STD/Pro can claim I2S_NUM_0. TearDownStream
    // Channels also stops FlashPlaybackTask and verifies stream_task is gone
    // before deleting stream_rx (UAF-safe).
    {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        TaskHandle_t stuck = g_flash.stream_task;
        xSemaphoreGive(g_flash.mutex);
        for (int i = 0; i < 150 && stuck != nullptr; ++i) {  // up to 1.5 s
            vTaskDelay(pdMS_TO_TICKS(10));
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            stuck = g_flash.stream_task;
            xSemaphoreGive(g_flash.mutex);
        }
    }
    esp_err_t teardown_result = TearDownStreamChannels();

    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    if (teardown_result == ESP_OK) {
        teardown_result =
            services::EndAudioActivity(&g_flash_audio_session);
        if (teardown_result != ESP_OK) {
            ESP_LOGE(kTag, "Flash audio session end failed; lease retained: %s",
                     esp_err_to_name(teardown_result));
        }
    } else {
        ESP_LOGE(kTag, "Flash audio teardown incomplete; retaining session lease");
    }
    if (teardown_result == ESP_OK) {
        g_flash_connectivity_demand.Reset();
    }
    xSemaphoreGive(g_flash.mutex);

    ESP_LOGI(kTag, "flash session stopped");
    return teardown_result;
}

esp_err_t StopFlashSession()
{
    if (g_flash.mutex == nullptr) {
        return ESP_OK;
    }
    if (g_lifecycle_task == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!g_teardown_pending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return ESP_OK;
    }
    g_start_pending.store(false, std::memory_order_release);
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    // Cancels a lifecycle-task WiFi wait without blocking the UI task. The
    // radio remains alive for the bounded idle tail while teardown completes.
    g_flash_connectivity_demand.Reset();
    xSemaphoreGive(g_flash.mutex);
    g_intentional_stop.store(true, std::memory_order_release);
    g_terminal_generation.store(
        g_session_generation.load(std::memory_order_acquire),
        std::memory_order_relaxed);
    g_terminal_reason.store(
        FlashTerminalReason::kIntentional, std::memory_order_release);
    xTaskNotifyGive(g_lifecycle_task);
    return ESP_OK;
}

FlashStatus GetFlashStatus()
{
    if (g_flash.mutex == nullptr) {
        return FlashStatus::kIdle;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    InternalStatus s = g_flash.status;
    xSemaphoreGive(g_flash.mutex);

    switch (s) {
        case InternalStatus::kIdle:
            return FlashStatus::kIdle;
        case InternalStatus::kConnecting:
        case InternalStatus::kSessionUpdating:
            return FlashStatus::kConnecting;
        case InternalStatus::kStreaming:
            return FlashStatus::kStreaming;
        case InternalStatus::kError:
            return FlashStatus::kError;
    }
    return FlashStatus::kIdle;
}

bool IsFlashConnected()
{
    if (g_flash.mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    const bool connected = g_flash.ws_connected;
    xSemaphoreGive(g_flash.mutex);
    return connected;
}

bool CopyFlashStateToUi(FlashUiState* state)
{
    if (state == nullptr || g_flash.mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    if (!g_flash.changed) {
        xSemaphoreGive(g_flash.mutex);
        return false;
    }
    // Map InternalStatus to FlashStatus directly (no nested mutex acquisition)
    switch (g_flash.status) {
        case InternalStatus::kIdle:
            state->status = FlashStatus::kIdle;
            break;
        case InternalStatus::kConnecting:
            state->status = FlashStatus::kConnecting;
            break;
        case InternalStatus::kSessionUpdating:
            state->status = FlashStatus::kConnecting;
            break;
        case InternalStatus::kStreaming:
            state->status = FlashStatus::kStreaming;
            break;
        case InternalStatus::kError:
            state->status = FlashStatus::kError;
            break;
    }
    state->user_transcript = g_flash.user_transcript;
    state->assistant_text = g_flash.assistant_text;
    state->pending_text = g_flash.pending_text;
    state->tool_label = g_flash.tool_label;
    state->error_message = g_flash.error_message;
    state->status_since_ms = g_flash.status_since_ms;
    // [phase-fix] Snapshot actual turn state. InternalStatus::kStreaming alone
    // only says the WebSocket session is connected.
    state->connected = g_flash.ws_connected;
    state->capture_started = g_flash.capture_started;
    state->response_in_flight = g_flash.response_in_flight;
    state->response_started = g_flash.response_started;
    state->playback_active =
        g_flash.playback_write_active ||
        g_flash.playback_queued_bytes.load(std::memory_order_acquire) > 0;
    g_flash.changed = false;
    xSemaphoreGive(g_flash.mutex);
    return true;
}

bool IsFlashTranscribing()
{
    if (g_flash.mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    const bool transcribing = g_flash.capture_started && g_flash.ws_connected;
    xSemaphoreGive(g_flash.mutex);
    return transcribing;
}

bool IsFlashSessionActive()
{
    if (g_flash.mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    const bool active =
        g_flash.ws_client != nullptr || g_flash.status == InternalStatus::kConnecting ||
        g_flash.status == InternalStatus::kSessionUpdating ||
        g_flash.status == InternalStatus::kStreaming || g_flash.capture_started ||
        g_flash.response_in_flight || g_flash.playback_write_active ||
        g_flash.playback_queued_bytes.load(std::memory_order_acquire) > 0;
    xSemaphoreGive(g_flash.mutex);
    return active;
}

void PollFlashAmpIdle()
{
    CheckAmpIdleTail();
}

void OnFlashButtonPressed()
{
    if (g_flash.mutex == nullptr) {
        InitFlashSession();
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);

    if (g_flash.status == InternalStatus::kIdle || g_flash.status == InternalStatus::kError) {
        // [error-recovery-fix] When in kError, the old WS client is still
        // alive (connection not closed, just got an error event). StartFlashSession
        // sees ws_client != nullptr and returns early without reconnecting,
        // permanently locking the device in kError. Force-stop the old session
        // first so StartFlashSession creates a fresh client.
        if (g_flash.status == InternalStatus::kError) {
            g_restart_after_teardown.store(true, std::memory_order_release);
            xSemaphoreGive(g_flash.mutex);
            StopFlashSession();
            return;
        }
        xSemaphoreGive(g_flash.mutex);
        StartFlashSession();
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    }

    g_flash.button_pressed = true;

    // [barge-in-fix] User pressed PTT while the server is mid-playback.
    // Clear the local playback buffer immediately (no audio tail) AND tell
    // the proxy to cancel any in-flight response generation. This is the
    // device half of the CLEAR_PLAYER double-action; the proxy then sends
    // response.cancelled / conversation.item.truncate to StepFun Realtime.
    bool need_barge_in = false;
    if (g_flash.ws_connected && g_flash.response_in_flight) {
        // [inflight-fix] Only cancel when a response is actually in-flight
        // (tracked via response_in_flight: set on turn-end, cleared on
        // response.done/error). The old check (playback_queued || tool_label)
        // fired even after response.done -> "no ongoing response to cancel".
        need_barge_in = true;
    }

    if (g_flash.ws_connected &&
        (g_flash.status == InternalStatus::kStreaming || g_flash.status == InternalStatus::kSessionUpdating)) {
        // [turn-lifecycle] This function is only reached after the 200ms hold
        // threshold, so it is a real PTT turn (short/double taps never get here).
        ResetFlashTurnAssemblyLocked();
        g_flash.capture_started = true;
        g_flash.capture_base_seq = g_flash.uplink_seq;  // [barge-fix] detect empty capture on release
        g_flash.pending_text = "正在录音...";
        g_flash.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);

        if (need_barge_in) {
            // Local side: drop everything still waiting in the ringbuffer so
            // the speaker goes silent the instant the user lets go of PTT.
            DrainPlaybackRingbuf();
            SetStreamAudioAmp(false);
            // [inflight-fix] Flush I2S TX DMA: disable+enable clears any residual
            // TTS samples the DMA already fetched (DrainPlaybackRingbuf only
            // clears the ringbuffer, not the DMA buffer). Mirrors xiaozhi
            // ResetDecoder clearing audio_playback_queue.
            if (g_flash.stream_tx != nullptr) {
                wqn::services::ResetAudioTxChannel(
                    g_flash_audio_session, g_flash.stream_tx,
                    kStreamTxDmaBytes);
            }
            // Remote side: ask the proxy to cancel the in-progress response.
            const char* cancel = "{\"type\":\"response.cancel\"}";
            esp_websocket_client_send_text(g_flash.ws_client, cancel,
                                           std::strlen(cancel),
                                           pdMS_TO_TICKS(1000));
            ESP_LOGI(kTag, "barge-in: cancelled in-flight response");
        }

        StartAudioStreaming();
    } else if (g_flash.status == InternalStatus::kConnecting) {
        // [cold-start] Session still connecting - the press is recorded and
        // recording auto-starts on session.ready (see session.ready handler).
        // Tell the user to hold so they don't release early and lose the turn.
        g_flash.pending_text = "正在连接...请按住";
        g_flash.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);
    } else {
        xSemaphoreGive(g_flash.mutex);
    }
}

void OnFlashButtonReleased(bool submit)
{
    if (g_flash.mutex == nullptr) {
        return;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    // Always stop streaming if it was started, regardless of WS connection state.
    // If WS disconnected mid-recording, we still need to release the I2S hardware.
    bool was_capturing = g_flash.capture_started;
    const bool cancel_pending_connect =
        !was_capturing && !g_flash.ws_connected &&
        g_flash.status == InternalStatus::kConnecting;
    g_flash.capture_started = false;
    g_flash.button_pressed = false;
    g_flash.status_since_ms = esp_timer_get_time() / 1000;
    MarkChanged();
    if (was_capturing) {
        g_flash.pending_text = g_flash.ws_connected ? "正在处理..." : "录音中断";
    }
    bool ws_connected = g_flash.ws_connected;
    xSemaphoreGive(g_flash.mutex);

    if (cancel_pending_connect) {
        ESP_LOGI(kTag, "Flash PTT released before online; cancelling connect");
        ESP_ERROR_CHECK_WITHOUT_ABORT(StopFlashSession());
        return;
    }

    if (was_capturing) {
        StopAudioStreaming();
    }

    // Force the PA off when capture ends. Mark any playback write racing with
    // this transition as aborted so it cannot re-arm the idle tail afterward.
    if (g_flash.mutex != nullptr) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.playback_abort_requested = true;
        g_flash.amp_idle_armed = false;
        xSemaphoreGive(g_flash.mutex);
    }
    SetStreamAudioAmp(false);

    if (was_capturing) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.playback_abort_requested = false;
        xSemaphoreGive(g_flash.mutex);
    }

    // [deadlock-fix] Send turn-end WITHOUT holding g_flash.mutex. The WS
    // client's internal lock is acquired by esp_websocket_client_send_*,
    // and the WS event handler (which holds that lock) may try to take
    // g_flash.mutex in ParseAndHandleEvent. Nesting g_flash.mutex ->
    // client->lock while the WS task holds client->lock -> g_flash.mutex
    // is a classic AB-BA deadlock. Copy handle + seq under our lock,
    // release, then send.
    bool should_send = false;
    esp_websocket_client_handle_t client = nullptr;
    uint32_t seq = 0;

    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    const bool captured_audio = (g_flash.uplink_seq != g_flash.capture_base_seq);
    const bool prev_in_flight = g_flash.response_in_flight;
    if (submit && ws_connected && g_flash.ws_client != nullptr && was_capturing && captured_audio && !prev_in_flight) {
        // [barge-fix] Only send the final end-of-turn frame if real audio was
        // captured (uplink_seq advanced past capture_base_seq). A fast barge-in
        // tap leaves uplink_seq unchanged; sending an empty final frame would
        // trigger commit + response.create that collides with the just-sent
        // response.cancel -> interleaved replies ("串台") + "append is not
        // called" error.
        // [inflight-fix] Also require !prev_in_flight: sending commit+create
        // while the prior response is still generating -> "ongoing response
        // already exists". If prev_in_flight, skip (audio lost, user must
        // re-press after the prior response finishes).
        should_send = true;
        client = g_flash.ws_client;
        ++g_flash.uplink_seq;
        seq = g_flash.uplink_seq;
        g_flash.response_in_flight = true;  // in-flight until response.done/error
        g_flash.response_started = false;   // [phase-fix] no delta for this turn yet
    } else if (was_capturing && captured_audio && prev_in_flight) {
        ESP_LOGW(kTag, "skip turn-end: prior response still in-flight");
    }
    xSemaphoreGive(g_flash.mutex);

    if (should_send) {
#if CONFIG_WQN_FLASH_PROTOCOL_V2
        std::vector<uint8_t> end;
        BuildV2AudioFrame(&end, nullptr, 0, seq, /*final=*/true);
        esp_websocket_client_send_bin(client,
                                      reinterpret_cast<const char*>(end.data()),
                                      end.size(), pdMS_TO_TICKS(1000));
#else
        std::string commit = R"({"type":"input_audio_buffer.commit"})";
        esp_websocket_client_send_text(client, commit.c_str(), commit.size(), pdMS_TO_TICKS(1000));
        std::string resp_create = R"({"type":"response.create"})";
        esp_websocket_client_send_text(client, resp_create.c_str(), resp_create.size(), pdMS_TO_TICKS(1000));
#endif
    }
}

void AbortFlashPlayback()
{
    if (g_flash.mutex == nullptr) {
        return;
    }
    // [barge-in] Local silence: drain queued PCM + flush I2S TX DMA + amp off.
    // Mirrors the local half of OnFlashButtonPressed's barge-in (without
    // starting a new turn). FlashPlaybackTask keeps running (idle, blocked on
    // its ringbuf receive); it is NOT stopped here - that is session-level, in
    // TearDownStreamChannels.
    DrainPlaybackRingbuf();
    SetStreamAudioAmp(false);
    if (g_flash.stream_tx != nullptr) {
        wqn::services::ResetAudioTxChannel(
            g_flash_audio_session, g_flash.stream_tx, kStreamTxDmaBytes);
    }
    // Remote: ask the proxy to cancel the in-flight response so the server
    // stops sending more TTS audio. Without this, audio.delta frames already
    // in flight would refill the ringbuf and playback would resume.
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    const bool cancel = g_flash.ws_connected && g_flash.response_in_flight;
    esp_websocket_client_handle_t client = g_flash.ws_client;
    xSemaphoreGive(g_flash.mutex);
    if (cancel && client != nullptr) {
        const char* msg = "{\"type\":\"response.cancel\"}";
        esp_websocket_client_send_text(client, msg, std::strlen(msg), pdMS_TO_TICKS(1000));
        ESP_LOGI(kTag, "flash playback aborted (response.cancel sent)");
    } else {
        ESP_LOGI(kTag, "flash playback aborted (local only)");
    }
}

}  // namespace wqn

#else

namespace wqn {
esp_err_t InitFlashSession() { return ESP_OK; }
esp_err_t StartFlashSession() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t StopFlashSession() { return ESP_ERR_NOT_SUPPORTED; }
FlashStatus GetFlashStatus() { return FlashStatus::kIdle; }
bool IsFlashConnected() { return false; }
bool CopyFlashStateToUi(FlashUiState*) { return false; }
bool IsFlashTranscribing() { return false; }
void PollFlashAmpIdle() {}
void OnFlashButtonPressed() {}
void OnFlashButtonReleased(bool) {}
void AbortFlashPlayback() {}
}  // namespace wqn

#endif
