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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_manager.h"
#include "storage.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "wqn_flash";
// [model-fix] StepFun's /v1/realtime requires the model to be specified in
// the URL query string (same convention as OpenAI's realtime API). Without
// `?model=...` the server immediately returns HTTP/1.1 400 with
// {"error":{"message":"model is empty"}}, the WS upgrade never completes,
// and nginx logs a status=000 disconnect — which is exactly what we saw on
// the access_log side. Confirmed via direct curl against StepFun: this URL
// (with model) returns 101 Switching Protocols.
constexpr char kWsUri[] = "wss://wqn.helema.cn/v1/realtime?model=stepaudio-2.5-realtime";
constexpr char kDefaultVoice[] = "qingchunshaonv";

constexpr int kSampleRate = 16000;
constexpr int kChunkFrames = 240;
constexpr int kChunkBytes = kChunkFrames * 2;
constexpr int kChunkIntervalMs = 15;

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

std::string EncodeBase64(const uint8_t* data, size_t len)
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

std::vector<uint8_t> DecodeBase64Chunk(const char* encoded, size_t len)
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

    gpio_hold_dis(kStreamAudioAmp);
    gpio_set_level(kStreamAudioAmp, 0);
    gpio_hold_en(kStreamAudioAmp);
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

        // Send via WebSocket if connected
        bool ws_ok = false;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        ws_ok = g_flash.ws_connected && (g_flash.ws_client != nullptr);
        xSemaphoreGive(g_flash.mutex);
        if (ws_ok) {
            std::string b64 = EncodeBase64(mono_buf, mono_bytes);
            std::string msg = R"({"type":"input_audio_buffer.append","audio":"}" + b64 + R"("})";
            esp_websocket_client_send_text(g_flash.ws_client, msg.c_str(), msg.size(), pdMS_TO_TICKS(50));
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

    if (std::strcmp(type, "session.created") == 0 ||
        std::strcmp(type, "session.updated") == 0) {
        ESP_LOGI(kTag, "session event: %s", type);
        bool should_auto_record = false;
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        if (g_flash.status == InternalStatus::kConnecting ||
            g_flash.status == InternalStatus::kSessionUpdating) {
            g_flash.status = InternalStatus::kStreaming;
            g_flash.pending_text = "开始对话";
            g_flash.status_since_ms = esp_timer_get_time() / 1000;
            // If the user is still holding the button, auto-start recording now
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

    if (std::strcmp(type, "response.audio_transcript.done") == 0) {
        return;
    }

    if (std::strcmp(type, "response.audio.delta") == 0) {
        const char* delta = JsonStr(g.p, "delta");
        if (delta != nullptr && delta[0] != '\0') {
            std::vector<uint8_t> pcm = DecodeBase64Chunk(delta, std::strlen(delta));
            // [heap-fix] Align decoded byte count down to int16_t boundary.
            // Without this, an odd pcm.size() causes memcpy to write 1 byte
            // past the samples vector, corrupting the heap and triggering a
            // panic on the next allocation.
            const size_t safe_bytes = pcm.size() & ~static_cast<size_t>(1);
            if (safe_bytes >= 2 && g_flash.stream_tx != nullptr) {
                // Mono PCM from server → stereo interleaved for I2S TX
                const size_t sample_count = safe_bytes / 2;
                std::vector<int16_t> stereo(sample_count * 2);
                const int16_t* mono = reinterpret_cast<const int16_t*>(pcm.data());
                for (size_t i = 0; i < sample_count; ++i) {
                    stereo[i * 2] = mono[i];
                    stereo[i * 2 + 1] = mono[i];
                }
                size_t written = 0;
                i2s_channel_write(g_flash.stream_tx, stereo.data(),
                                  stereo.size() * sizeof(int16_t),
                                  &written, pdMS_TO_TICKS(50));
            }
        }
        return;
    }

    if (std::strcmp(type, "response.done") == 0) {
        ESP_LOGI(kTag, "response done");
        return;
    }

    if (std::strcmp(type, "error") == 0) {
        // StepFun realtime nests the error under {"error":{"message":"..."}};
        // accept either nesting or a top-level "message" so the path is robust.
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

    // Unhandled event type — log at debug level only so unknown future events
    // don't spam the warn channel.
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
            MarkChanged();

            std::string session_update = std::string(R"({"type":"session.update","session":)") +
                R"({"modalities":["text","audio"],"instructions":")" +
                "\u4f60\u662f\u4e2a\u4eba\u52a9\u7406\u5c0f\u4e91\uff0c\u8bf7\u7528\u53ef\u7231\u98ce\u8da3\u7684\u65b9\u5f0f\u56de\u7b54\u7528\u6237\u7684\u95ee\u9898\u3002" +
                R"(","voice":")" + kDefaultVoice +
                R"(","input_audio_format":"pcm16","output_audio_format":"pcm16")" +
                R"(,"turn_detection":{"type":"server_vad","prefix_padding_ms":500,"silence_duration_ms":200}}})";
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
                // Skip control frames (Ping: 0x9, Pong: 0xa, Close: 0x8) and binary data (0x2).
                // Only process text frames (0x1) and continuation frames (0x0).
                if (event->op_code != 0x01 && event->op_code != 0x00) {
                    break;
                }
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
                    if (event->payload_offset + event->data_len >=
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
    cfg.uri = kWsUri;
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
    // Without this, the WS handshake fails locally and nginx never sees the
    // request, which is why /v1/realtime had no access_log entries.
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // [flash-fix] Was: cfg.subprotocol = "io.vertx.core.eventbus.EventBus".
    // That's Vert.x EventBus bridge protocol and has nothing to do with the
    // StepFun realtime API — the upstream server can reject the handshake on
    // subprotocol mismatch. StepFun realtime doesn't require any subprotocol;
    // leave it unset (default).

    g_flash.ws_client = esp_websocket_client_init(&cfg);
    if (g_flash.ws_client == nullptr) {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        SetErrorLocked("WS 客户端初始化失败");
        xSemaphoreGive(g_flash.mutex);
        return ESP_ERR_NO_MEM;
    }

    std::string bearer = "Bearer " + access_token;
    esp_websocket_client_append_header(g_flash.ws_client, "Authorization", bearer.c_str());
    // [auth-fix] Shared secret for nginx `/v1/realtime` gate. Replaces the
    // auth_request subrequest approach, which broke WS upgrade responses
    // (handshake_status_code=0 even after 204 from auth-verify). nginx now
    // checks `if ($http_x_ws_secret != "WQN_Flash_Secret_2026") return 401;`
    // directly — no subrequest, no WS-response corruption.
    esp_websocket_client_append_header(g_flash.ws_client, "X-WS-Secret", "WQN_Flash_Secret_2026");
    esp_websocket_client_append_header(g_flash.ws_client, "Accept", "application/json");
    esp_websocket_client_append_header(g_flash.ws_client, "Content-Type", "application/json");

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

    // Disconnect WebSocket OUTSIDE the mutex to avoid deadlock with its event handler
    esp_websocket_client_handle_t client_to_destroy = nullptr;
    bool was_connected = false;
    {
        xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
        if (g_flash.ws_client != nullptr) {
            was_connected = g_flash.ws_connected;
            if (was_connected) {
                std::string commit = R"({"type":"input_audio_buffer.commit"})";
                esp_websocket_client_send_text(g_flash.ws_client, commit.c_str(), commit.size(), pdMS_TO_TICKS(1000));
                std::string resp_create = R"({"type":"response.create"})";
                esp_websocket_client_send_text(g_flash.ws_client, resp_create.c_str(), resp_create.size(), pdMS_TO_TICKS(1000));
            }
            client_to_destroy = g_flash.ws_client;
            g_flash.ws_client = nullptr;
            g_flash.ws_connected = false;
        }
        g_flash.status = InternalStatus::kIdle;
        g_flash.pending_text.clear();
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

    if (g_flash.ws_connected &&
        (g_flash.status == InternalStatus::kStreaming || g_flash.status == InternalStatus::kSessionUpdating)) {
        g_flash.capture_started = true;
        g_flash.pending_text = "正在录音...";
        g_flash.status_since_ms = esp_timer_get_time() / 1000;
        MarkChanged();
        xSemaphoreGive(g_flash.mutex);
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

    // Send commit/response only if WS was actually connected
    xSemaphoreTake(g_flash.mutex, portMAX_DELAY);
    if (ws_connected && g_flash.ws_client != nullptr) {
        std::string commit = R"({"type":"input_audio_buffer.commit"})";
        esp_websocket_client_send_text(g_flash.ws_client, commit.c_str(), commit.size(), pdMS_TO_TICKS(1000));
        std::string resp_create = R"({"type":"response.create"})";
        esp_websocket_client_send_text(g_flash.ws_client, resp_create.c_str(), resp_create.size(), pdMS_TO_TICKS(1000));
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
void OnFlashButtonPressed() {}
void OnFlashButtonReleased() {}
}  // namespace wqn

#endif
