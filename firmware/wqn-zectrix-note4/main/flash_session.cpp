#include "flash_session.h"

#if CONFIG_WQN_AI_ENABLE

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "audio_player.h"
#include "audio_capture.h"
#include "cJSON.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_manager.h"
#include "storage.h"
#include "wifi_manager.h"

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
//                  [u32 sample_rate= 16000]
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

constexpr int kSampleRate = 16000;
constexpr int kChunkFrames = 240;
constexpr int kChunkBytes = kChunkFrames * 2;
constexpr int kChunkIntervalMs = 15;
constexpr uint32_t kOutputSampleRate = WQN_FLASH_OUTPUT_SAMPLE_RATE_HZ;

constexpr int kMaxReconnectAttempts = 3;
constexpr TickType_t kWifiReadyWait = pdMS_TO_TICKS(15000);
constexpr TickType_t kWsConnectTimeout = pdMS_TO_TICKS(20000);

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
    i2s_chan_handle_t stream_rx = nullptr;
    i2s_chan_handle_t stream_tx = nullptr;  // duplex TX for audio playback
    i2c_master_bus_handle_t stream_i2c_bus = nullptr;
    bool stream_audio_powered = false;
    // [amp-fix] amp is opened lazily on the first playback write and closed via
    // an idle-tail timer (or synchronously on release / stop / cleanup).
    int64_t amp_idle_due_ms = 0;
    bool amp_idle_armed = false;
    // v2 uplink frame counter
    uint32_t uplink_seq = 0;
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
    size_t playback_queued_bytes = 0;
    size_t playback_dropped_bytes = 0;
};

FlashState g_flash;

void MarkChanged()
{
    g_flash.changed = true;
}

void SetErrorLocked(const std::string& message)
{
    g_flash.status = InternalStatus::kError;
    g_flash.pending_text.clear();
    g_flash.error_message = message;
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
    uint32_t sample_rate;  // 16000
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

constexpr gpio_num_t kStreamAudioPower = GPIO_NUM_42;
constexpr gpio_num_t kStreamAudioAmp = GPIO_NUM_46;
constexpr gpio_num_t kStreamI2sMclk = GPIO_NUM_14;
constexpr gpio_num_t kStreamI2sBclk = GPIO_NUM_15;
constexpr gpio_num_t kStreamI2sWs = GPIO_NUM_38;
constexpr gpio_num_t kStreamI2sDin = GPIO_NUM_16;
constexpr gpio_num_t kStreamI2sDout = GPIO_NUM_45;
constexpr gpio_num_t kStreamCodecSda = GPIO_NUM_47;
constexpr gpio_num_t kStreamCodecScl = GPIO_NUM_48;
constexpr i2c_port_num_t kStreamCodecI2cPort = I2C_NUM_0;
constexpr uint8_t kStreamEs8311Address = 0x18;
constexpr uint32_t kStreamDmaFrameNum = 256;
constexpr int kStreamChunkFrames = 240;    // 15 ms at 16 kHz
constexpr int kStreamChunkBytes = kStreamChunkFrames * 2;  // 16-bit mono
constexpr int64_t kAmpIdleTailMs = 600;    // turn amp off this long after last audio-delta write

// [playback-fix] FreeRTOS ringbuffer for decoded downlink PCM. Sized for
// ~1.5 s of 24 kHz mono audio = 72000 bytes. Byte-mode ringbuffer is required
// so we can carry an arbitrary byte slice (the v2 downlink frames arrive in
// 24-byte-header + variable-length PCM chunks).
constexpr size_t kPlaybackRingbufBytes = 72000;
constexpr size_t kPlaybackRingbufItemMax = 4096;  // matches an average downlink chunk

constexpr uint8_t ES8311_REG_RESET = 0x00;
constexpr uint8_t ES8311_REG_CLK_MAN1 = 0x01;
constexpr uint8_t ES8311_REG_CLK_MAN2 = 0x02;
constexpr uint8_t ES8311_REG_CLK_MAN3 = 0x03;
constexpr uint8_t ES8311_REG_RESERVED1 = 0x04;
constexpr uint8_t ES8311_REG_RESERVED2 = 0x05;
constexpr uint8_t ES8311_REG_RESERVED3 = 0x06;
constexpr uint8_t ES8311_REG_RESERVED4 = 0x07;
constexpr uint8_t ES8311_REG_SDPOUT = 0x0A;
constexpr uint8_t ES8311_REG_SDPIN = 0x09;
constexpr uint8_t ES8311_REG_SYSTEM1 = 0x0B;
constexpr uint8_t ES8311_REG_SYSTEM2 = 0x0C;
constexpr uint8_t ES8311_REG_ADC_CTRL1 = 0x10;
constexpr uint8_t ES8311_REG_ADC_CTRL2 = 0x11;
constexpr uint8_t ES8311_REG_ADC_DGAIN1 = 0x13;
constexpr uint8_t ES8311_REG_ADC_DGAIN2 = 0x14;
constexpr uint8_t ES8311_REG_ADC_DGAIN3 = 0x15;
constexpr uint8_t ES8311_REG_ADC_DGAIN4 = 0x16;
constexpr uint8_t ES8311_REG_ADC_DGAIN5 = 0x17;
constexpr uint8_t ES8311_REG_ADC_DGAIN6 = 0x1B;
constexpr uint8_t ES8311_REG_ADC_DGAIN7 = 0x1C;
constexpr uint8_t ES8311_REG_GPIO = 0x44;
constexpr uint8_t ES8311_REG_GP = 0x45;

esp_err_t WriteEs8311Reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(dev, data, 2, pdMS_TO_TICKS(100));
}

esp_err_t ReadEs8311Reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* value)
{
    return i2c_master_transmit_receive(dev, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

void SetStreamAudioPower(bool enabled)
{
    gpio_hold_dis(kStreamAudioPower);
    gpio_set_level(kStreamAudioPower, enabled ? 1 : 0);
    gpio_hold_en(kStreamAudioPower);
}

// [amp-fix] Was: amp was hard-tied to power and forced off, so response.audio.delta
// PCM data went into I2S TX but the user heard nothing (ES8311 line-out disabled).
// Now amp is independent: off during capture (avoid feedback), on while the server
// is streaming audio out, off after a short idle tail so consecutive spoken turns
// don't accumulate click/pop artifacts. Direct write to GPIO46 is OK only because
// flash mode and audio_player (TTS) are mutually exclusive on this UI.
void SetStreamAudioAmp(bool enabled)
{
    gpio_hold_dis(kStreamAudioAmp);
    gpio_set_level(kStreamAudioAmp, enabled ? 1 : 0);
    gpio_hold_en(kStreamAudioAmp);
}

// Check whether the idle tail has expired and turn the amp off if so. Cheap
// enough to call from the UI loop on every poll. Safe to call when flash is
// idle (no-op).
void CheckAmpIdleTail()
{
    if (g_flash.mutex == nullptr) {
        return;
    }
    bool should_off = false;
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    if (g_flash.amp_idle_armed) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= g_flash.amp_idle_due_ms) {
            g_flash.amp_idle_armed = false;
            should_off = true;
        }
    }
    xSemaphoreGive(g_flash.mutex);
    if (should_off) {
        SetStreamAudioAmp(false);
    }
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
    if (g_flash.playback_ringbuf == nullptr || bytes == 0) {
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
    g_flash.playback_queued_bytes += aligned;
    return true;
}

// [playback-fix] Reset the playback ringbuffer. Used when the user barge-ins
// or when the session transitions to idle/error, so queued audio is dropped
// immediately and the next response starts from silence.
void DrainPlaybackRingbuf()
{
    if (g_flash.playback_ringbuf == nullptr) {
        return;
    }
    size_t item_size = 0;
    char* item = nullptr;
    while ((item = static_cast<char*>(xRingbufferReceive(
                   g_flash.playback_ringbuf, &item_size, 0))) != nullptr) {
        vRingbufferReturnItem(g_flash.playback_ringbuf, item);
    }
    // ESP-IDF 5.x exposes only xRingbufferReset() (FreeRTOS-Kconfig-aware) as
    // vRingbufferReset(); the API isn't available in stock FreeRTOS. We rely on
    // having drained all items above, so no reset call is needed.
    g_flash.playback_queued_bytes = 0;
    g_flash.playback_dropped_bytes = 0;
}

// [playback-fix] Independent FreeRTOS task that drains the ringbuffer and
// writes to I2S. Runs on core 1 alongside the streaming (capture) task so
// neither path competes with the UI loop on core 0.
void FlashPlaybackTask(void* /*param*/)
{
    ESP_LOGI(kTag, "flash playback task started");
    std::vector<int16_t> stereo;  // reused scratch buffer
    while (true) {
        size_t item_size = 0;
        char* item = static_cast<char*>(
            xRingbufferReceive(g_flash.playback_ringbuf, &item_size, portMAX_DELAY));
        if (item == nullptr) {
            continue;
        }
        // Silence-fill small items so I2S DMA does not underrun; this also
        // preserves mono->stereo alignment if the proxy delivered a sub-frame.
        const size_t sample_count = item_size / 2;
        const int16_t* mono = reinterpret_cast<const int16_t*>(item);

        if (g_flash.stream_tx == nullptr) {
            vRingbufferReturnItem(g_flash.playback_ringbuf, item);
            continue;
        }

        // Cheap piggyback: extend the amp idle tail so consecutive chunks
        // within the same turn don't toggle the speaker off between frames.
        {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.amp_idle_armed = true;
            g_flash.amp_idle_due_ms = esp_timer_get_time() / 1000 + kAmpIdleTailMs;
            xSemaphoreGive(g_flash.mutex);
        }
        SetStreamAudioAmp(true);

        stereo.resize(sample_count * 2);
        for (size_t i = 0; i < sample_count; ++i) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        size_t written = 0;
        // pdMS_TO_TICKS(50) — same grace window as before. With the ringbuffer
        // decoupled, a slow i2s write here no longer stalls the WS event task;
        // it just queues more PCM in the ringbuffer (or drops newest bytes).
        i2s_channel_write(g_flash.stream_tx, stereo.data(),
                          stereo.size() * sizeof(int16_t), &written,
                          pdMS_TO_TICKS(50));
        vRingbufferReturnItem(g_flash.playback_ringbuf, item);

        if (g_flash.playback_queued_bytes >= item_size) {
            g_flash.playback_queued_bytes -= item_size;
        } else {
            g_flash.playback_queued_bytes = 0;
        }
    }
}

void EnsurePlaybackRingbuf()
{
    if (g_flash.playback_ringbuf == nullptr) {
        g_flash.playback_ringbuf = xRingbufferCreate(
            kPlaybackRingbufBytes, RINGBUF_TYPE_BYTEBUF);
        if (g_flash.playback_ringbuf == nullptr) {
            ESP_LOGE(kTag, "playback ringbuf alloc failed (%u B)",
                     static_cast<unsigned>(kPlaybackRingbufBytes));
        }
    }
    if (g_flash.playback_ringbuf != nullptr && g_flash.playback_task == nullptr) {
        const BaseType_t created = xTaskCreatePinnedToCore(
            FlashPlaybackTask, "flash_playback", 4096, nullptr, 5,
            &g_flash.playback_task, 1);
        if (created != pdPASS) {
            ESP_LOGE(kTag, "flash playback task create failed");
            g_flash.playback_task = nullptr;
        }
    }
}

esp_err_t InitStreamEs8311Adc(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t dev = nullptr;
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kStreamEs8311Address;
    dev_cfg.scl_speed_hz = 100000;
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bus, &dev_cfg, &dev),
        kTag, "add ES8311 device");

    auto write = [&](uint8_t r, uint8_t v) { return WriteEs8311Reg(dev, r, v); };
    auto read = [&](uint8_t r, uint8_t* v) { return ReadEs8311Reg(dev, r, v); };
    esp_err_t ret = ESP_OK;
    // NOTE: ES8311 is shared with audio_player (DAC). Do NOT perform a full chip
    // reset (write ES8311_REG_RESET = 0x80) here — it wipes all DAC/PGA settings
    // and breaks subsequent playback. Only configure ADC-specific registers and
    // assume the codec has been powered up by audio_player or by SetStreamAudioPower.

    // Configure ADC digital gain, volume, and clock for capture path
    ret |= write(ES8311_REG_ADC_CTRL1, 0x1F);    // ADC PGA volume
    ret |= write(ES8311_REG_ADC_CTRL2, 0x7F);    // ADC max analog gain
    ret |= write(ES8311_REG_ADC_DGAIN1, 0x10);   // ADC digital gain
    ret |= write(ES8311_REG_ADC_DGAIN6, 0x0A);   // ADC ALC target level
    ret |= write(ES8311_REG_ADC_DGAIN7, 0x6A);   // ADC ALC settings

    // Enable ADC path (SDPIN is the ADC digital interface register)
    uint8_t reg = 0;
    if (read(ES8311_REG_SDPIN, &reg) == ESP_OK) {
        ret |= write(ES8311_REG_SDPIN, reg & ~0x40);
    } else {
        ret = ESP_FAIL;
    }

    i2c_master_bus_rm_device(dev);
    return ret;
}

esp_err_t InitStreamI2sDuplex(i2s_chan_handle_t* rx_handle)
{
    if (rx_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*rx_handle != nullptr) {
        ESP_RETURN_ON_ERROR(i2s_channel_enable(*rx_handle), kTag, "enable stream I2S RX");
        if (g_flash.stream_tx != nullptr) {
            ESP_RETURN_ON_ERROR(i2s_channel_enable(g_flash.stream_tx), kTag, "enable stream I2S TX");
        }
        return ESP_OK;
    }
    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = I2S_NUM_0;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = kStreamDmaFrameNum;
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = false;
    chan_cfg.intr_priority = 0;
    // [duplex-fix] Create BOTH RX and TX channels on I2S_NUM_0 together.
    // This mirrors the official firmware's CreateDuplexChannels pattern and
    // prevents the port-occupied conflict that occurs when audio_player.cpp
    // later tries to create a standalone TX on the same I2S port.
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &g_flash.stream_tx, rx_handle),
                        kTag, "create duplex I2S channels");

    // ---- RX config (microphone capture) ----
    i2s_std_config_t rx_cfg = {};
    rx_cfg.clk_cfg.sample_rate_hz = kSampleRate;
    rx_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    rx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    rx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    rx_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    rx_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    rx_cfg.slot_cfg.ws_pol = false;
    rx_cfg.slot_cfg.bit_shift = true;
    rx_cfg.slot_cfg.left_align = true;
    rx_cfg.slot_cfg.big_endian = false;
    rx_cfg.slot_cfg.bit_order_lsb = false;
    rx_cfg.gpio_cfg.mclk = kStreamI2sMclk;
    rx_cfg.gpio_cfg.bclk = kStreamI2sBclk;
    rx_cfg.gpio_cfg.ws = kStreamI2sWs;
    rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rx_cfg.gpio_cfg.din = kStreamI2sDin;
    rx_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    rx_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    rx_cfg.gpio_cfg.invert_flags.ws_inv = false;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*rx_handle, &rx_cfg), kTag, "init stream I2S RX std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*rx_handle), kTag, "enable stream I2S RX");

    // ---- TX config (speaker playback) ----
    i2s_std_config_t tx_cfg = {};
    tx_cfg.clk_cfg.sample_rate_hz = kSampleRate;
    tx_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    tx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tx_cfg.slot_cfg = rx_cfg.slot_cfg;  // same slot format
    tx_cfg.gpio_cfg = rx_cfg.gpio_cfg;
    tx_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    tx_cfg.gpio_cfg.dout = kStreamI2sDout;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_flash.stream_tx, &tx_cfg),
                        kTag, "init stream I2S TX std (duplex)");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(g_flash.stream_tx),
                        kTag, "enable stream I2S TX (duplex)");
    return ESP_OK;
}

void CleanupStreamHardware()
{
    // Release the duplex I2S channels (RX + TX) that this module created.
    // Do NOT delete any shared I2C bus or pull codec power — those are managed
    // by audio_player (which is also using the same ES8311 codec and audio amp).
    // Pulling the power here would cut off any audio that is still being played.
    if (g_flash.stream_tx != nullptr) {
        i2s_channel_disable(g_flash.stream_tx);
        i2s_del_channel(g_flash.stream_tx);
        g_flash.stream_tx = nullptr;
    }
    if (g_flash.stream_rx != nullptr) {
        i2s_channel_disable(g_flash.stream_rx);
        i2s_del_channel(g_flash.stream_rx);
        g_flash.stream_rx = nullptr;
    }
    if (g_flash.stream_i2c_bus != nullptr) {
        if (g_flash.stream_i2c_bus != wqn::GetSharedI2cBusHandle()) {
            i2c_del_master_bus(g_flash.stream_i2c_bus);
        }
        g_flash.stream_i2c_bus = nullptr;
    }
    g_flash.stream_audio_powered = false;
    // [amp-fix] Always force amp off on hardware teardown; the mutex may
    // already be taken here, so write GPIO directly and clear the timer flag.
    if (g_flash.mutex != nullptr) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.amp_idle_armed = false;
        xSemaphoreGive(g_flash.mutex);
    }
    SetStreamAudioAmp(false);
}

void AudioStreamingTask(void* param)
{
    (void)param;
    uint8_t i2s_buf[kStreamChunkBytes * 2];  // stereo, 2 bytes/sample

    // Init I2C bus (try shared, fallback to own)
    g_flash.stream_i2c_bus = wqn::GetSharedI2cBusHandle();
    if (g_flash.stream_i2c_bus == nullptr) {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = kStreamCodecI2cPort;
        bus_cfg.sda_io_num = kStreamCodecSda;
        bus_cfg.scl_io_num = kStreamCodecScl;
        if (i2c_new_master_bus(&bus_cfg, &g_flash.stream_i2c_bus) != ESP_OK) {
            ESP_LOGW(kTag, "stream I2C bus init failed");
            g_flash.stream_i2c_bus = nullptr;
            vTaskDelete(nullptr);
            return;
        }
    }

    // Init ES8311 ADC
    if (InitStreamEs8311Adc(g_flash.stream_i2c_bus) != ESP_OK) {
        ESP_LOGW(kTag, "stream ES8311 ADC init failed");
        CleanupStreamHardware();
        vTaskDelete(nullptr);
        return;
    }

    // Init I2S RX
    if (InitStreamI2sDuplex(&g_flash.stream_rx) != ESP_OK) {
        ESP_LOGW(kTag, "stream I2S RX init failed");
        CleanupStreamHardware();
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag, "audio streaming task started");

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
        esp_err_t read_err = i2s_channel_read(
            g_flash.stream_rx, i2s_buf, sizeof(i2s_buf), &bytes_read, pdMS_TO_TICKS(20));
        if (read_err != ESP_OK || bytes_read == 0) {
            if (read_err != ESP_ERR_TIMEOUT) {
                ESP_LOGW(kTag, "I2S read error: %s", esp_err_to_name(read_err));
            }
            continue;
        }

        // Convert stereo interleaved to mono (left channel only).
        // I2S stereo 16-bit layout: [L0_lo L0_hi R0_lo R0_hi L1_lo L1_hi ...]
        // Each stereo frame is 4 bytes (2 channels × 2 bytes/sample).
        const int stereo_frames = static_cast<int>(bytes_read / 4);
        const int frames = std::min(stereo_frames, kStreamChunkFrames);
        uint8_t mono_buf[kStreamChunkBytes];
        const int16_t* stereo_samples = reinterpret_cast<const int16_t*>(i2s_buf);
        int16_t* mono_samples = reinterpret_cast<int16_t*>(mono_buf);
        for (int i = 0; i < frames; ++i) {
            mono_samples[i] = stereo_samples[i * 2];  // left channel only
        }
        size_t mono_bytes = static_cast<size_t>(frames) * 2;

        // Send via WebSocket if connected.
        // v2 uses binary frames (24-byte header + PCM); v1 fallback sends
        // {"type":"input_audio_buffer.append","audio":"<base64>"}.
        bool ws_ok = false;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        ws_ok = g_flash.ws_connected && (g_flash.ws_client != nullptr);
        xSemaphoreGive(g_flash.mutex);
        if (ws_ok) {
#if CONFIG_WQN_FLASH_PROTOCOL_V2
            std::vector<uint8_t> frame;
            BuildV2AudioFrame(&frame, mono_buf, mono_bytes, ++g_flash.uplink_seq, /*final=*/false);
            esp_websocket_client_send_bin(g_flash.ws_client,
                                         reinterpret_cast<const char*>(frame.data()),
                                         frame.size(),
                                         pdMS_TO_TICKS(50));
#else
            std::string b64 = EncodeBase64(mono_buf, mono_bytes);
            std::string msg = R"({"type":"input_audio_buffer.append","audio":"}" + b64 + R"("})";
            esp_websocket_client_send_text(g_flash.ws_client, msg.c_str(), msg.size(), pdMS_TO_TICKS(50));
#endif
        }
    }

    ESP_LOGI(kTag, "audio streaming task exiting");
    CleanupStreamHardware();
    // Clear the global handle BEFORE self-deletion so StopAudioStreaming()
    // can detect task exit without touching already-freed memory.
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    g_flash.stream_task = nullptr;
    xSemaphoreGive(g_flash.mutex);
    vTaskDelete(nullptr);
}

void StartAudioStreaming()
{
    if (g_flash.stream_task != nullptr) {
        return;
    }
    // Power on audio hardware first
    SetStreamAudioPower(true);
    g_flash.stream_audio_powered = true;
    vTaskDelay(pdMS_TO_TICKS(250));  // warm-up delay for ES8311

    // [amp-fix] Always start in a closed-amp state. The amp is enabled lazily
    // by the response.audio.delta handler when the server actually starts
    // streaming playback, and closed by the idle tail or by release/stop.
    if (g_flash.mutex != nullptr) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.amp_idle_armed = false;
        xSemaphoreGive(g_flash.mutex);
    }
    SetStreamAudioAmp(false);

    xTaskCreatePinnedToCore(&AudioStreamingTask, "flash_stream", 4096, nullptr, 10,
                             &g_flash.stream_task, 1);
}

void StopAudioStreaming()
{
    if (g_flash.stream_task == nullptr) {
        return;
    }
    // Signal the task to stop by clearing the flag. Do NOT externally call
    // vTaskDelete() here — let the task delete itself after it reads the flag
    // and cleans up. Otherwise we risk a double-cleanup (task deleting itself
    // while we also try to delete it) and a double-free of hardware resources.
    g_flash.capture_started = false;
    TaskHandle_t task = g_flash.stream_task;
    // Wait for the task to notice the flag and self-delete (max ~50 ms).
    for (int i = 0; i < 5; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (g_flash.stream_task == nullptr) {
            // Task self-deleted successfully
            return;
        }
    }
    // Task still alive — force cleanup (rare fallback for hung task)
    ESP_LOGW(kTag, "streaming task did not exit cleanly, forcing cleanup");
    CleanupStreamHardware();
    if (eTaskGetState(task) != eDeleted) {
        vTaskDelete(task);
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    g_flash.stream_task = nullptr;
    xSemaphoreGive(g_flash.mutex);
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

    if (std::strcmp(type, "asr.complete") == 0) {
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

    if (std::strcmp(type, "text.delta") == 0 ||
        std::strcmp(type, "response.audio_transcript.delta") == 0) {
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
        if (g_flash.mutex != nullptr) {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.amp_idle_armed = false;
            g_flash.status_since_ms = esp_timer_get_time() / 1000;
            MarkChanged();
            xSemaphoreGive(g_flash.mutex);
        }
        SetStreamAudioAmp(false);
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
        if (g_flash.mutex != nullptr) {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.amp_idle_armed = false;
            xSemaphoreGive(g_flash.mutex);
        }
        SetStreamAudioAmp(false);
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
        return;
    }

    ESP_LOGD(kTag, "unhandled WS event: %s", type);
}

void WebsocketEventHandler(void* handler_args, esp_event_base_t, int32_t event_id, void* event_data)
{
    const auto* event = static_cast<const esp_websocket_event_data_t*>(event_data);

    switch (static_cast<esp_websocket_event_id_t>(event_id)) {
        case WEBSOCKET_EVENT_CONNECTED: {
            ESP_LOGI(kTag, "WebSocket connected");
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            g_flash.ws_connected = true;
            g_flash.status = InternalStatus::kSessionUpdating;
            g_flash.pending_text = "会话初始化...";
            g_flash.status_since_ms = esp_timer_get_time() / 1000;
            g_flash.uplink_seq = 0;
            MarkChanged();

#if CONFIG_WQN_FLASH_PROTOCOL_V2
            // wqn-flash-v2: every negotiation happens through the session.update
            // control JSON. We pin the voice + audio format + instructions here
            // and ask the proxy to call StepAudio 2.5 Realtime on our behalf.
            std::string session_update = std::string(R"({"type":"session.update","session":)") +
                R"({"model":")" + std::string(WQN_FLASH_WS_MODEL) +
                R"(","voice":")" + kDefaultVoice +
                R"(","input_audio_format":"pcm16","input_sample_rate":16000)" +
                R"(,"output_audio_format":"opus","output_sample_rate":)" +
                std::to_string(static_cast<long long>(kOutputSampleRate)) +
                R"(,"instructions":")" + std::string(kDefaultInstructions) +
                R"(","vad":{"mode":"server_vad","prefix_padding_ms":500,"silence_duration_ms":200}}})";
#else
            std::string session_update = std::string(R"({"type":"session.update","session":)") +
                R"({"modalities":["text","audio"],"instructions":")" +
                "\u4f60\u662f\u4e2a\u4eba\u52a9\u7406\u5c0f\u4e91\uff0c\u8bf7\u7528\u53ef\u7231\u98ce\u8da3\u7684\u65b9\u5f0f\u56de\u7b54\u7528\u6237\u7684\u95ee\u9898\u3002" +
                R"(","voice":")" + kDefaultVoice +
                R"(","input_audio_format":"pcm16","output_audio_format":"pcm16")" +
                R"(,"turn_detection":{"type":"server_vad","prefix_padding_ms":500,"silence_duration_ms":200}}})";
#endif
            esp_websocket_client_send_text(g_flash.ws_client, session_update.c_str(),
                                           session_update.size(), pdMS_TO_TICKS(5000));
            xSemaphoreGive(g_flash.mutex);
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
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    g_flash.status = InternalStatus::kIdle;
    g_flash.status_since_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(g_flash.mutex);
    // [playback-fix] Ringbuffer + playback task are allocated once, up front,
    // so the WS event path can never hit a null ringbuffer in steady state.
    EnsurePlaybackRingbuf();
    return ESP_OK;
}

esp_err_t StartFlashSession()
{
    if (g_flash.mutex == nullptr) {
        InitFlashSession();
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

    esp_err_t result = StartWifiStationIfEnabled();
    if (result != ESP_OK) {
        result = WaitForWifiStationConnected(kWifiReadyWait);
        if (result != ESP_OK) {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            SetErrorLocked("WiFi 未就绪");
            xSemaphoreGive(g_flash.mutex);
            return result;
        }
    } else if (!IsWifiStationConnected()) {
        result = WaitForWifiStationConnected(kWifiReadyWait);
        if (result != ESP_OK) {
            xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
            SetErrorLocked("WiFi 未连接");
            xSemaphoreGive(g_flash.mutex);
            return result;
        }
    }

    g_flash.status = InternalStatus::kConnecting;
    g_flash.pending_text = "正在连接...";
    g_flash.error_message.clear();
    g_flash.user_transcript.clear();
    g_flash.assistant_text.clear();
    g_flash.status_since_ms = esp_timer_get_time() / 1000;
    MarkChanged();
    xSemaphoreGive(g_flash.mutex);

    std::string access_token;
    esp_err_t tok_err = wqn::LoadAccessToken(&access_token);
    if (tok_err != ESP_OK || access_token.empty()) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("未登录，请先完成账号配对");
        xSemaphoreGive(g_flash.mutex);
        return ESP_ERR_INVALID_STATE;
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
    cfg.network_timeout_ms = static_cast<int>(kWsConnectTimeout / portTICK_PERIOD_MS);
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

    g_flash.ws_client = esp_websocket_client_init(&cfg);
    if (g_flash.ws_client == nullptr) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("WS 客户端初始化失败");
        xSemaphoreGive(g_flash.mutex);
        return ESP_ERR_NO_MEM;
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
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("WS 事件注册失败");
        xSemaphoreGive(g_flash.mutex);
        return reg_err;
    }

    result = esp_websocket_client_start(g_flash.ws_client);
    if (result != ESP_OK) {
        esp_websocket_client_destroy(g_flash.ws_client);
        g_flash.ws_client = nullptr;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("WS 连接失败");
        xSemaphoreGive(g_flash.mutex);
        return result;
    }

    return ESP_OK;
}

esp_err_t StopFlashSession()
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
        g_flash.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);
    }

    // Now destroy the client (no mutex held, so WebSocket event handler can complete)
    if (client_to_destroy != nullptr) {
        esp_websocket_client_stop(client_to_destroy);
        esp_websocket_client_destroy(client_to_destroy);
    }

    ESP_LOGI(kTag, "flash session stopped");
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
    if (g_flash.ws_connected && g_flash.status == InternalStatus::kStreaming &&
        (g_flash.playback_queued_bytes > 0 || !g_flash.tool_label.empty())) {
        need_barge_in = true;
    }

    if (g_flash.ws_connected &&
        (g_flash.status == InternalStatus::kStreaming || g_flash.status == InternalStatus::kSessionUpdating)) {
        g_flash.capture_started = true;
        g_flash.pending_text = "正在录音...";
        g_flash.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);

        if (need_barge_in) {
            // Local side: drop everything still waiting in the ringbuffer so
            // the speaker goes silent the instant the user lets go of PTT.
            DrainPlaybackRingbuf();
            SetStreamAudioAmp(false);
            // Remote side: ask the proxy to cancel the in-progress response.
            const char* cancel = "{\"type\":\"response.cancel\"}";
            esp_websocket_client_send_text(g_flash.ws_client, cancel,
                                           std::strlen(cancel),
                                           pdMS_TO_TICKS(1000));
            ESP_LOGI(kTag, "barge-in: cancelled in-flight response");
        }

        StartAudioStreaming();
    } else {
        xSemaphoreGive(g_flash.mutex);
    }
}

void OnFlashButtonReleased()
{
    if (g_flash.mutex == nullptr) {
        return;
    }
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    // Always stop streaming if it was started, regardless of WS connection state.
    // If WS disconnected mid-recording, we still need to release the I2S hardware.
    bool was_capturing = g_flash.capture_started;
    g_flash.capture_started = false;
    g_flash.button_pressed = false;
    g_flash.status_since_ms = esp_timer_get_time() / 1000;
    MarkChanged();
    if (was_capturing) {
        g_flash.pending_text = g_flash.ws_connected ? "正在处理..." : "录音中断";
    }
    bool ws_connected = g_flash.ws_connected;
    xSemaphoreGive(g_flash.mutex);

    if (was_capturing) {
        StopAudioStreaming();
    }

    // [amp-fix] Force the amp off the moment the user lets go of PTT, regardless
    // of whether audio was actually playing back. The idle-tail timer would also
    // do this but only after up to 600 ms of silence — too late for a clean handoff.
    if (g_flash.mutex != nullptr) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        g_flash.amp_idle_armed = false;
        xSemaphoreGive(g_flash.mutex);
    }
    SetStreamAudioAmp(false);

    // Send turn-end only if WS was actually connected.
    // v2: a binary "end-of-turn" frame with kFlagFinal signals the proxy that
    // no more audio will follow for this turn; the server flushes ASR and
    // asks the model to respond.  v1 fallback: send the OpenAI-style commit +
    // response.create pair that StepFun shaped servers understand.
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    if (ws_connected && g_flash.ws_client != nullptr) {
#if CONFIG_WQN_FLASH_PROTOCOL_V2
        std::vector<uint8_t> end;
        BuildV2AudioFrame(&end, nullptr, 0, ++g_flash.uplink_seq, /*final=*/true);
        esp_websocket_client_send_bin(g_flash.ws_client,
                                      reinterpret_cast<const char*>(end.data()),
                                      end.size(), pdMS_TO_TICKS(1000));
#else
        std::string commit = R"({"type":"input_audio_buffer.commit"})";
        esp_websocket_client_send_text(g_flash.ws_client, commit.c_str(), commit.size(), pdMS_TO_TICKS(1000));
        std::string resp_create = R"({"type":"response.create"})";
        esp_websocket_client_send_text(g_flash.ws_client, resp_create.c_str(), resp_create.size(), pdMS_TO_TICKS(1000));
#endif
    }
    xSemaphoreGive(g_flash.mutex);
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
void OnFlashButtonReleased() {}
}  // namespace wqn

#endif
