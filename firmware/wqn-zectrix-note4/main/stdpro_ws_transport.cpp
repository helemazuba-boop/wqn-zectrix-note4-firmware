#include "stdpro_ws_transport.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include "cJSON.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sse_chunk.h"
#include "wflv_frame.h"

namespace {

constexpr char kTag[] = "stdpro_ws";
constexpr size_t kPcmFramesPerBlock = 240; // 15ms @ 16kHz mono s16le
// One WebSocket send may legally block for 1.5 s. Keep 1.92 s of capture
// jitter in explicitly allocated PSRAM so a transient TCP stall cannot turn
// into pre-FINAL loss, without reclaiming the ~62 KiB from DMA/internal RAM.
constexpr size_t kPcmQueueLength = 128;
constexpr size_t kCtrlQueueLength = 8;
constexpr size_t kCtrlCompletionQueueLength = 4;
constexpr UBaseType_t kTransportTaskPriority = 7;
// [stream-depth] Device-observed failure: an SSE burst of 1287 bytes arrived
// with only 512 B free in an 8 KiB buffer -> overflow -> ResetRequired killed
// a healthy connection mid-turn. Consumer (this same owner task) can stall
// for up to a PCM send timeout while the socket keeps delivering, so size for
// multi-frame bursts. The backing store is explicitly allocated from PSRAM;
// FreeRTOS dynamic object allocation is internal-only in this IDF build.
constexpr size_t kTextStreamBufferSize = 16384;

enum class ConnectionState : uint8_t {
    kDisconnected,
    kConnecting,
    kReady,
};

enum class TurnState : uint8_t {
    kIdle,
    kRecording,
    kFinalQueued,
    kFinalInFlight,
    kRemoteHandoff,
    kFailedPreFinal,
    kComplete,
};

struct PcmBlock {
    uint32_t seq;
    uint16_t flags; // kWflvFlagStream or kWflvFlagFinal
    uint16_t sample_count;
    int16_t samples[kPcmFramesPerBlock];
};

enum class CtrlCmdType : uint8_t {
    kConnect,
    kStartTurn,
    kAbort,
    kDisconnect,
    kResetRequired,
};

enum class CmdDisposition : uint8_t {
    kExecuted,
    kCancelled,
    kExpired,
    kRejected,
};

struct CtrlCmd {
    CtrlCmdType type;
    uint32_t gen;          // Immutable generation assigned at command creation
    uint32_t target_turn_gen; // 0 = match request_id alone or unassigned
    int64_t deadline_ms;   // Absolute deadline in ms (0 = no deadline)
    char token[128];
    char request_id[64];
    char tier[16];
    char conversation_id[64];
    char reasoning_effort[16];
    bool enable_thinking;
};

struct CtrlCompletion {
    uint32_t op_gen;
    esp_err_t result;
    CmdDisposition disposition;
};

struct TurnTerminalRecord {
    uint32_t turn_gen = 0;
    char request_id[64] = {};
    wqn::stdpro_ws::TurnReleaseState release_state = wqn::stdpro_ws::TurnReleaseState::kNone;
    wqn::stdpro_ws::TurnStreamObservation stream_observation = wqn::stdpro_ws::TurnStreamObservation::kNone;
};

// Event Notification Bits for VoiceWsTransportTask
constexpr uint32_t kEvtBitWsConnected         = (1 << 0);
constexpr uint32_t kEvtBitWsDisconnected      = (1 << 1);
constexpr uint32_t kEvtBitWsError             = (1 << 2);
constexpr uint32_t kEvtBitTextData            = (1 << 3);
constexpr uint32_t kEvtBitCtrlPending         = (1 << 4);
constexpr uint32_t kEvtBitResetRequired       = (1 << 5);
constexpr uint32_t kEvtBitEmergencyAbort      = (1 << 6);
constexpr uint32_t kEvtBitEmergencyDisconnect = (1 << 7);
constexpr uint32_t kEvtBitPcmPending           = (1 << 8);

// Global synchronization and queue primitives
QueueHandle_t g_pcm_queue = nullptr;
QueueHandle_t g_ctrl_queue = nullptr;
QueueHandle_t g_ctrl_completion_queue = nullptr;
StreamBufferHandle_t g_text_stream = nullptr;
TaskHandle_t g_transport_task = nullptr;

SemaphoreHandle_t g_transport_mutex = nullptr;
SemaphoreHandle_t g_sync_op_sem = nullptr;
SemaphoreHandle_t g_final_sem = nullptr;
SemaphoreHandle_t g_turn_complete_sem = nullptr;

// Init is transactional and serialized independently from the transport
// mutex, which does not exist until the transaction commits. Keeping the
// stream control block static lets its 16 KiB backing store live in PSRAM
// without relying on FreeRTOS' internal-only pvPortMalloc path.
StaticSemaphore_t g_init_mutex_storage = {};
SemaphoreHandle_t g_init_mutex = nullptr;
portMUX_TYPE g_init_mutex_lock = portMUX_INITIALIZER_UNLOCKED;
StaticQueue_t g_pcm_queue_control = {};
uint8_t* g_pcm_queue_storage = nullptr;
StaticStreamBuffer_t g_text_stream_control = {};
uint8_t* g_text_stream_storage = nullptr;

std::atomic<uint32_t> g_sync_op_gen{0};
std::atomic<uint32_t> g_turn_gen{0};

std::atomic<ConnectionState> g_conn_state{ConnectionState::kDisconnected};
std::atomic<TurnState> g_turn_state{TurnState::kIdle};
std::atomic<bool> g_is_connected{false};
std::atomic<bool> g_session_ready{false};
std::atomic<uint32_t> g_seq{0};
std::atomic<bool> g_prefinal_pcm_lost{false};

wqn::stdpro_ws::FinalHandoffResult g_final_result =
    wqn::stdpro_ws::FinalHandoffResult::kDefinitelyNotSent;

wqn::WqnAiSseCallback g_sse_callback = nullptr;
void* g_sse_user_ctx = nullptr;

// Owned strictly inside VoiceWsTransportTask
esp_websocket_client_handle_t g_ws_client = nullptr;
uint32_t g_active_conn_op_gen = 0;
int64_t g_conn_deadline_ms = 0;

uint32_t g_active_turn_gen = 0;
char g_active_turn_req_id[64] = {};
wqn::stdpro_ws::TurnStreamObservation g_active_turn_stream_obs =
    wqn::stdpro_ws::TurnStreamObservation::kNone;

TurnTerminalRecord g_terminal_turn_record = {};

std::string g_current_token;
std::string g_current_req_id;
std::string g_control_reassembly_buf;
wqn::SseFrameBuffer g_sse_parser;

void PublishCompletion(uint32_t op_gen, esp_err_t result, CmdDisposition disposition)
{
    if (op_gen == 0) {
        return;
    }
    CtrlCompletion comp = { op_gen, result, disposition };
    if (g_ctrl_completion_queue != nullptr) {
        if (xQueueSend(g_ctrl_completion_queue, &comp, 0) != pdTRUE) {
            // Queue full: unconsumed completion from earlier timed-out operation; purge oldest
            CtrlCompletion stale;
            xQueueReceive(g_ctrl_completion_queue, &stale, 0);
            xQueueSend(g_ctrl_completion_queue, &comp, 0);
        }
    }
    if (g_sync_op_sem != nullptr) {
        xSemaphoreGive(g_sync_op_sem);
    }
}

void PublishTurnTerminalRecord(uint32_t turn_gen,
                               const char* req_id,
                               wqn::stdpro_ws::TurnReleaseState release_state,
                               wqn::stdpro_ws::TurnStreamObservation stream_obs)
{
    if (turn_gen == 0 || req_id == nullptr || req_id[0] == '\0') {
        return;
    }
    g_terminal_turn_record.turn_gen = turn_gen;
    std::strncpy(g_terminal_turn_record.request_id, req_id,
                 sizeof(g_terminal_turn_record.request_id) - 1);
    g_terminal_turn_record.request_id[sizeof(g_terminal_turn_record.request_id) - 1] = '\0';
    g_terminal_turn_record.release_state = release_state;
    g_terminal_turn_record.stream_observation = stream_obs;

    if (g_turn_complete_sem != nullptr) {
        xSemaphoreGive(g_turn_complete_sem);
    }
}

void DoDestroyWs()
{
    bool destroyed = false;
    if (g_ws_client != nullptr) {
        esp_websocket_client_stop(g_ws_client);
        esp_websocket_client_destroy(g_ws_client);
        g_ws_client = nullptr;
        destroyed = true;
    }
    g_conn_state.store(ConnectionState::kDisconnected, std::memory_order_release);
    g_is_connected.store(false, std::memory_order_release);
    g_session_ready.store(false, std::memory_order_release);
    g_control_reassembly_buf.clear();
    g_sse_parser.clear();

    if (g_active_conn_op_gen != 0) {
        PublishCompletion(g_active_conn_op_gen, ESP_FAIL, CmdDisposition::kCancelled);
        g_active_conn_op_gen = 0;
        g_conn_deadline_ms = 0;
    }

    if (g_text_stream != nullptr) {
        xStreamBufferReset(g_text_stream);
    }

    if (g_pcm_queue != nullptr) {
        PcmBlock dummy;
        while (xQueueReceive(g_pcm_queue, &dummy, 0) == pdTRUE) {
            // drop
        }
    }

    if (destroyed) {
        // [dma-attrib] Attribute the per-cycle DMA-capable decay: snapshot right
        // after a full client stop+destroy so connect/teardown deltas are visible.
        ESP_LOGW(kTag,
                 "[dma-attrib] after-do-destroy-ws dma_free=%u dma_largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    }
}

enum class TeardownReason : uint8_t {
    kTransportFault,
    kLocalDisconnect,
    kLocalAbort,
};

struct EmergencyAbortTarget {
    uint32_t target_turn_gen = 0;
    char request_id[64] = {};
};

portMUX_TYPE g_emergency_abort_mux = portMUX_INITIALIZER_UNLOCKED;
EmergencyAbortTarget g_emergency_abort_target = {};

void TeardownFailedTurn(TeardownReason reason = TeardownReason::kTransportFault)
{
    const TurnState state = g_turn_state.load(std::memory_order_acquire);
    if (state == TurnState::kFinalInFlight) {
        g_final_result = wqn::stdpro_ws::FinalHandoffResult::kAmbiguous;
        if (g_final_sem != nullptr) {
            xSemaphoreGive(g_final_sem);
        }
    } else if (state == TurnState::kRecording) {
        g_final_result = wqn::stdpro_ws::FinalHandoffResult::kDefinitelyNotSent;
        if (g_final_sem != nullptr) {
            xSemaphoreGive(g_final_sem);
        }
    }
    if (g_active_turn_gen != 0) {
        wqn::stdpro_ws::TurnReleaseState rel =
            wqn::stdpro_ws::TurnReleaseState::kPreFinalFailed;
        wqn::stdpro_ws::TurnStreamObservation obs = g_active_turn_stream_obs;

        if (state == TurnState::kRemoteHandoff) {
            if (reason == TeardownReason::kLocalDisconnect) {
                rel = wqn::stdpro_ws::TurnReleaseState::kLocallyAbandoned;
                // Preserve last actually observed stream observation (do NOT overwrite with kTransportLost)
            } else if (reason == TeardownReason::kLocalAbort) {
                rel = wqn::stdpro_ws::TurnReleaseState::kAborted;
                obs = wqn::stdpro_ws::TurnStreamObservation::kAborted;
            } else {
                rel = wqn::stdpro_ws::TurnReleaseState::kTransportLost;
                obs = wqn::stdpro_ws::TurnStreamObservation::kTransportLost;
            }
        } else if (state == TurnState::kFinalInFlight) {
            rel = wqn::stdpro_ws::TurnReleaseState::kHandoffAmbiguous;
        } else {
            if (reason == TeardownReason::kLocalAbort) {
                rel = wqn::stdpro_ws::TurnReleaseState::kAborted;
                obs = wqn::stdpro_ws::TurnStreamObservation::kAborted;
            }
        }
        PublishTurnTerminalRecord(g_active_turn_gen, g_active_turn_req_id, rel, obs);
        g_active_turn_gen = 0;
        g_active_turn_req_id[0] = '\0';
        g_active_turn_stream_obs = wqn::stdpro_ws::TurnStreamObservation::kNone;
    }
    DoDestroyWs();
    g_current_req_id.clear();
    g_turn_state.store(TurnState::kIdle, std::memory_order_release);
}

void WebsocketEventHandler(void*, esp_event_base_t, int32_t event_id,
                           void* event_data)
{
    if (g_transport_task == nullptr) {
        return;
    }
    const auto* event =
        static_cast<const esp_websocket_event_data_t*>(event_data);

    switch (static_cast<esp_websocket_event_id_t>(event_id)) {
        case WEBSOCKET_EVENT_CONNECTED: {
            xTaskNotify(g_transport_task, kEvtBitWsConnected, eSetBits);
            break;
        }

        case WEBSOCKET_EVENT_DISCONNECTED: {
            xTaskNotify(g_transport_task, kEvtBitWsDisconnected, eSetBits);
            break;
        }

        case WEBSOCKET_EVENT_ERROR: {
            xTaskNotify(g_transport_task, kEvtBitWsError, eSetBits);
            break;
        }

        case WEBSOCKET_EVENT_DATA: {
            if (event->op_code == 0x01 || event->op_code == 0x00) {
                if (event->data_ptr != nullptr && event->data_len > 0 &&
                    g_text_stream != nullptr) {
                    const size_t sent = xStreamBufferSend(
                        g_text_stream, event->data_ptr, event->data_len, 0);
                    if (sent != static_cast<size_t>(event->data_len)) {
                        ESP_LOGE(kTag,
                                 "StreamBuffer overflow (sent %u of %d) — "
                                 "fatal stream loss",
                                 static_cast<unsigned>(sent), event->data_len);
                        xTaskNotify(g_transport_task, kEvtBitResetRequired,
                                    eSetBits);
                    } else {
                        xTaskNotify(g_transport_task, kEvtBitTextData,
                                    eSetBits);
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

void ProcessTextStream()
{
    if (g_text_stream == nullptr) {
        return;
    }
    char buf[512];
    size_t bytes_read = 0;

    while ((bytes_read = xStreamBufferReceive(g_text_stream, buf, sizeof(buf),
                                             0)) > 0) {
        if (!g_session_ready.load(std::memory_order_acquire)) {
            g_control_reassembly_buf.append(buf, bytes_read);
            cJSON* root = cJSON_ParseWithLength(
                g_control_reassembly_buf.data(),
                g_control_reassembly_buf.size());
            if (root != nullptr) {
                cJSON* type_item =
                    cJSON_GetObjectItemCaseSensitive(root, "type");
                if (type_item != nullptr && cJSON_IsString(type_item) &&
                    std::strcmp(type_item->valuestring, "session.ready") == 0) {
                    ESP_LOGI(kTag, "session.ready control frame validated");
                    g_session_ready.store(true, std::memory_order_release);
                    if (g_conn_state.load(std::memory_order_acquire) ==
                        ConnectionState::kConnecting) {
                        g_conn_state.store(ConnectionState::kReady,
                                           std::memory_order_release);
                        if (g_active_conn_op_gen != 0) {
                            PublishCompletion(g_active_conn_op_gen, ESP_OK,
                                              CmdDisposition::kExecuted);
                            g_active_conn_op_gen = 0;
                            g_conn_deadline_ms = 0;
                        }
                    }
                }
                cJSON_Delete(root);
                g_control_reassembly_buf.clear();
            }
            continue;
        }

        // Feed into SSE parser
        g_sse_parser.feed(buf, bytes_read);
        std::string ev_name;
        uint64_t ev_id = 0;
        std::string ev_data;

        while (g_sse_parser.extract(&ev_name, &ev_id, &ev_data) ==
               wqn::SseFrameBuffer::FrameState::kComplete) {
            if (ev_data.empty()) {
                continue;
            }
            wqn::WqnAiSseEvent ev;
            if (wqn::DecodeSseEvent(ev_name, ev_id, ev_data, &ev)) {
                const TurnState state =
                    g_turn_state.load(std::memory_order_acquire);

                if (ev.kind == wqn::WqnAiSseEvent::Kind::kError) {
                    if (g_active_turn_gen != 0 &&
                        (ev.request_id.empty() || ev.request_id == g_active_turn_req_id)) {
                        g_active_turn_stream_obs =
                            wqn::stdpro_ws::TurnStreamObservation::kRemoteErrorObserved;
                    }
                    if (state == TurnState::kRecording) {
                        ESP_LOGW(kTag, "Pre-FINAL server error (%s): %s",
                                 ev.error_code.c_str(),
                                 ev.error_message.c_str());
                        TeardownFailedTurn();
                        break;
                    }
                    if (state == TurnState::kFinalInFlight) {
                        ESP_LOGW(kTag,
                                 "Error during FINAL_IN_FLIGHT: marking "
                                 "ambiguous");
                        TeardownFailedTurn();
                        break;
                    }
                }

                if (ev.kind == wqn::WqnAiSseEvent::Kind::kTurnDone) {
                    if (g_active_turn_gen != 0 &&
                        (ev.request_id.empty() || ev.request_id == g_active_turn_req_id) &&
                        g_active_turn_stream_obs !=
                            wqn::stdpro_ws::TurnStreamObservation::kRemoteErrorObserved) {
                        g_active_turn_stream_obs =
                            wqn::stdpro_ws::TurnStreamObservation::kTurnDoneObserved;
                    }
                }

                // Deliver business SSE events to UI immediately
                if (g_sse_callback != nullptr) {
                    g_sse_callback(ev, g_sse_user_ctx);
                }

                // Transport-level release ONLY occurs on matching turn.released in RemoteHandoff
                if (ev_name == "turn.released") {
                    if (g_active_turn_gen != 0 && !ev.request_id.empty() &&
                        std::strcmp(ev.request_id.c_str(), g_active_turn_req_id) == 0 &&
                        state == TurnState::kRemoteHandoff) {
                        ESP_LOGI(kTag, "turn.released matched: req=%s gen=%lu obs=%d",
                                 g_active_turn_req_id,
                                 (unsigned long)g_active_turn_gen,
                                 (int)g_active_turn_stream_obs);
                        PublishTurnTerminalRecord(
                            g_active_turn_gen, g_active_turn_req_id,
                            wqn::stdpro_ws::TurnReleaseState::kReleased,
                            g_active_turn_stream_obs);
                        g_active_turn_gen = 0;
                        g_active_turn_req_id[0] = '\0';
                        g_active_turn_stream_obs =
                            wqn::stdpro_ws::TurnStreamObservation::kNone;
                        g_current_req_id.clear();
                        g_turn_state.store(TurnState::kIdle,
                                           std::memory_order_release);
                    } else {
                        ESP_LOGW(kTag,
                                 "Stale or mismatched turn.released ignored: "
                                 "ev_req=%s active_req=%s active_gen=%lu "
                                 "state=%d",
                                 ev.request_id.c_str(), g_active_turn_req_id,
                                 (unsigned long)g_active_turn_gen,
                                 (int)state);
                    }
                }
            }
        }
    }
}

void ExecuteCtrlCmdSingleStep(const CtrlCmd& cmd)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (cmd.deadline_ms > 0 && now_ms >= cmd.deadline_ms) {
        if (cmd.type == CtrlCmdType::kDisconnect) {
            // Expiry of caller wait does not cancel local transport recovery side-effect
            TeardownFailedTurn(TeardownReason::kLocalDisconnect);
        }
        PublishCompletion(cmd.gen, ESP_ERR_TIMEOUT, CmdDisposition::kExpired);
        return;
    }

    switch (cmd.type) {
        case CtrlCmdType::kConnect: {
            if (g_ws_client != nullptr &&
                esp_websocket_client_is_connected(g_ws_client) &&
                g_session_ready.load(std::memory_order_acquire) &&
                g_current_token == cmd.token) {
                g_conn_state.store(ConnectionState::kReady,
                                   std::memory_order_release);
                PublishCompletion(cmd.gen, ESP_OK, CmdDisposition::kExecuted);
                return;
            }

            DoDestroyWs();
            g_current_token = cmd.token;

            std::string auth_header =
                "Authorization: Bearer " + g_current_token + "\r\n";

            esp_websocket_client_config_t cfg = {};
            cfg.uri = "wss://" WQN_API_BASE_HOST WQN_FLASH_WS_PATH;
            cfg.subprotocol = WQN_VOICE_WS_SUBPROTOCOL;
            cfg.headers = auth_header.c_str();
            cfg.network_timeout_ms = 2500;
            cfg.task_stack = 8192;
            cfg.buffer_size = 4096;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            cfg.reconnect_timeout_ms = 2000;
            cfg.ping_interval_sec = 20;

            g_ws_client = esp_websocket_client_init(&cfg);
            if (g_ws_client == nullptr) {
                PublishCompletion(cmd.gen, ESP_ERR_NO_MEM, CmdDisposition::kExecuted);
                return;
            }

            esp_websocket_register_events(
                g_ws_client, WEBSOCKET_EVENT_ANY,
                WebsocketEventHandler, nullptr);

            const esp_err_t start_err =
                esp_websocket_client_start(g_ws_client);
            if (start_err != ESP_OK) {
                DoDestroyWs();
                PublishCompletion(cmd.gen, start_err, CmdDisposition::kExecuted);
                return;
            }

            // Asynchronous connecting state: owner task records active operation and returns to root loop
            g_active_conn_op_gen = cmd.gen;
            g_conn_deadline_ms = cmd.deadline_ms;
            g_conn_state.store(ConnectionState::kConnecting,
                               std::memory_order_release);
            break;
        }

        case CtrlCmdType::kStartTurn: {
            if (g_conn_state.load(std::memory_order_acquire) != ConnectionState::kReady ||
                g_turn_state.load(std::memory_order_acquire) != TurnState::kIdle ||
                g_ws_client == nullptr ||
                !esp_websocket_client_is_connected(g_ws_client) ||
                !g_session_ready.load(std::memory_order_acquire)) {
                PublishCompletion(cmd.gen, ESP_ERR_INVALID_STATE, CmdDisposition::kRejected);
                return;
            }

            // Build JSON safely and validate all item allocations prior to wire transmission
            cJSON* root = cJSON_CreateObject();
            bool json_ok = (root != nullptr);
            json_ok = json_ok && (cJSON_AddStringToObject(root, "type", "voice.turn.start") != nullptr);
            json_ok = json_ok && (cJSON_AddStringToObject(root, "request_id", cmd.request_id) != nullptr);
            json_ok = json_ok && (cJSON_AddStringToObject(root, "tier", cmd.tier) != nullptr);
            if (cmd.conversation_id[0] != '\0') {
                json_ok = json_ok && (cJSON_AddStringToObject(root, "conversation_id",
                                                            cmd.conversation_id) != nullptr);
            }
            json_ok = json_ok && (cJSON_AddBoolToObject(root, "enable_thinking",
                                                      cmd.enable_thinking) != nullptr);
            if (cmd.reasoning_effort[0] != '\0') {
                json_ok = json_ok && (cJSON_AddStringToObject(root, "reasoning_effort",
                                                            cmd.reasoning_effort) != nullptr);
            }

            if (!json_ok) {
                if (root != nullptr) {
                    cJSON_Delete(root);
                }
                PublishCompletion(cmd.gen, ESP_ERR_NO_MEM, CmdDisposition::kExecuted);
                return;
            }

            char* json_str = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);

            if (json_str == nullptr) {
                PublishCompletion(cmd.gen, ESP_ERR_NO_MEM, CmdDisposition::kExecuted);
                return;
            }

            const size_t json_len = std::strlen(json_str);
            const int sent = esp_websocket_client_send_text(
                g_ws_client, json_str, json_len,
                pdMS_TO_TICKS(1000));
            std::free(json_str);

            if (sent != static_cast<int>(json_len)) {
                ESP_LOGW(kTag,
                         "failed to send voice.turn.start: sent=%d "
                         "expected=%u",
                         sent, static_cast<unsigned>(json_len));
                DoDestroyWs();
                PublishCompletion(cmd.gen, ESP_FAIL, CmdDisposition::kExecuted);
                return;
            }

            // Drain any old PCM blocks prior to commit
            if (g_pcm_queue != nullptr) {
                PcmBlock dummy;
                while (xQueueReceive(g_pcm_queue, &dummy, 0) == pdTRUE) {}
            }

            // ATOMIC COMMIT UPON EXACT FULL SEND CONFIRMATION:
            g_current_req_id = cmd.request_id;
            g_active_turn_gen = ++g_turn_gen;
            std::strncpy(g_active_turn_req_id, cmd.request_id,
                         sizeof(g_active_turn_req_id) - 1);
            g_active_turn_req_id[sizeof(g_active_turn_req_id) - 1] = '\0';
            g_active_turn_stream_obs =
                wqn::stdpro_ws::TurnStreamObservation::kNone;

            g_seq.store(0, std::memory_order_release);
            g_turn_state.store(TurnState::kRecording,
                               std::memory_order_release);
            g_prefinal_pcm_lost.store(false, std::memory_order_release);
            g_sse_parser.clear();

            PublishCompletion(cmd.gen, ESP_OK, CmdDisposition::kExecuted);
            break;
        }

        case CtrlCmdType::kAbort: {
            const bool req_match = (cmd.request_id[0] != '\0' &&
                                    std::strcmp(cmd.request_id, g_active_turn_req_id) == 0);
            const bool gen_match = (cmd.target_turn_gen == 0 ||
                                    cmd.target_turn_gen == g_active_turn_gen);
            if (g_active_turn_gen != 0 && req_match && gen_match) {
                if (g_ws_client != nullptr &&
                    esp_websocket_client_is_connected(g_ws_client)) {
                    std::string msg =
                        "{\"type\":\"voice.turn.abort\",\"request_id\":\"" +
                        std::string(cmd.request_id) + "\"}";
                    esp_websocket_client_send_text(
                        g_ws_client, msg.c_str(), msg.size(),
                        pdMS_TO_TICKS(500));
                }
                TeardownFailedTurn(TeardownReason::kLocalAbort);
            } else {
                ESP_LOGW(kTag,
                         "Stale Abort ignored: cmd_req=%s cmd_gen=%lu "
                         "active_req=%s active_gen=%lu",
                         cmd.request_id,
                         (unsigned long)cmd.target_turn_gen,
                         g_active_turn_req_id,
                         (unsigned long)g_active_turn_gen);
            }
            break;
        }

        case CtrlCmdType::kDisconnect: {
            TeardownFailedTurn(TeardownReason::kLocalDisconnect);
            PublishCompletion(cmd.gen, ESP_OK, CmdDisposition::kExecuted);
            break;
        }

        case CtrlCmdType::kResetRequired: {
            TeardownFailedTurn();
            break;
        }
    }
}

void HandleAllNotificationBits(uint32_t bits)
{
    if (bits & kEvtBitWsConnected) {
        g_is_connected.store(true, std::memory_order_release);
    }

    // Process buffered TextData BEFORE fault teardown for active, session-ready turns
    const bool is_active_turn =
        (g_conn_state.load(std::memory_order_acquire) == ConnectionState::kReady &&
         g_session_ready.load(std::memory_order_acquire) &&
         g_active_turn_gen != 0);

    if (is_active_turn && (bits & kEvtBitTextData)) {
        ProcessTextStream();
    }

    if (bits & (kEvtBitWsDisconnected | kEvtBitWsError | kEvtBitResetRequired)) {
        if (g_conn_state.load(std::memory_order_acquire) == ConnectionState::kConnecting) {
            DoDestroyWs();
        } else {
            TeardownFailedTurn(TeardownReason::kTransportFault);
        }
    }

    if (bits & kEvtBitEmergencyDisconnect) {
        if (g_conn_state.load(std::memory_order_acquire) == ConnectionState::kConnecting) {
            DoDestroyWs();
        } else {
            TeardownFailedTurn(TeardownReason::kLocalDisconnect);
        }
    }

    if (bits & kEvtBitEmergencyAbort) {
        EmergencyAbortTarget target;
        portENTER_CRITICAL(&g_emergency_abort_mux);
        target = g_emergency_abort_target;
        g_emergency_abort_target = {};
        portEXIT_CRITICAL(&g_emergency_abort_mux);

        const bool req_match = (target.request_id[0] != '\0' &&
                                std::strcmp(target.request_id, g_active_turn_req_id) == 0);
        const bool gen_match = (target.target_turn_gen == 0 ||
                                target.target_turn_gen == g_active_turn_gen);
        if (g_active_turn_gen != 0 && req_match && gen_match) {
            if (g_ws_client != nullptr &&
                esp_websocket_client_is_connected(g_ws_client)) {
                std::string msg =
                    "{\"type\":\"voice.turn.abort\",\"request_id\":\"" +
                    std::string(target.request_id) + "\"}";
                esp_websocket_client_send_text(
                    g_ws_client, msg.c_str(), msg.size(),
                    pdMS_TO_TICKS(500));
            }
            TeardownFailedTurn(TeardownReason::kLocalAbort);
        } else {
            ESP_LOGW(kTag,
                     "Emergency Abort ignored: target_req=%s target_gen=%lu "
                     "active_req=%s active_gen=%lu",
                     target.request_id,
                     (unsigned long)target.target_turn_gen,
                     g_active_turn_req_id,
                     (unsigned long)g_active_turn_gen);
        }
    }

    // Process TextData for other cases (e.g. session.ready during connection handshake, or non-active turns)
    if (!is_active_turn && (bits & kEvtBitTextData)) {
        ProcessTextStream();
    }

    if (bits & kEvtBitCtrlPending) {
        // kEvtBitCtrlPending wakes the root loop; g_ctrl_queue is drained exclusively in the root loop below
    }
    if (bits & kEvtBitPcmPending) {
        // Wake-only bit. The root loop drains exactly one PCM block below and
        // then immediately iterates while the queue remains non-empty.
    }
}

void VoiceWsTransportTask(void*)
{
    // InitPrimitives creates this task before publishing the primitive set.
    // Stay behind the commit gate until every handle is globally visible.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    uint32_t notified_bits = 0;
    uint8_t frame[wqn::kWflvHeaderBytes + kPcmFramesPerBlock * 2];

    while (true) {
        // Compute poll timeout: if PCM is pending during recording, non-blocking check; otherwise 50ms
        TickType_t wait_ticks = pdMS_TO_TICKS(50);
        if (g_turn_state.load(std::memory_order_acquire) == TurnState::kRecording &&
            g_pcm_queue != nullptr && uxQueueMessagesWaiting(g_pcm_queue) > 0) {
            wait_ticks = 0;
        }

        if (xTaskNotifyWait(0, 0xFFFFFFFF, &notified_bits, wait_ticks) == pdTRUE) {
            HandleAllNotificationBits(notified_bits);
        }

        // Drain any control commands that arrived without separate notification (single consumer site)
        if (g_ctrl_queue != nullptr) {
            CtrlCmd cmd;
            while (xQueueReceive(g_ctrl_queue, &cmd, 0) == pdTRUE) {
                ExecuteCtrlCmdSingleStep(cmd);
            }
        }

        // Evaluate asynchronous connecting state deadline
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (g_conn_state.load(std::memory_order_acquire) == ConnectionState::kConnecting &&
            g_conn_deadline_ms > 0 && now_ms >= g_conn_deadline_ms) {
            ESP_LOGW(kTag, "Connection handshake timed out at deadline");
            const uint32_t expired_gen = g_active_conn_op_gen;
            g_active_conn_op_gen = 0;
            g_conn_deadline_ms = 0;
            DoDestroyWs();
            PublishCompletion(expired_gen, ESP_ERR_TIMEOUT, CmdDisposition::kExpired);
        }

        // Single PCM / FINAL frame step: process at most ONE block per root loop iteration
        if (g_turn_state.load(std::memory_order_acquire) == TurnState::kRecording &&
            g_pcm_queue != nullptr) {
            PcmBlock block;
            if (xQueueReceive(g_pcm_queue, &block, 0) == pdTRUE) {
                if (block.flags & wqn::kWflvFlagFinal) {
                    g_turn_state.store(TurnState::kFinalInFlight,
                                       std::memory_order_release);

                    wqn::EncodeWflvHeader(frame, wqn::kWflvFlagFinal, block.seq, 16000, 1);
                    const size_t total_len = wqn::kWflvHeaderBytes;

                    const int sent = esp_websocket_client_send_bin(
                        g_ws_client, reinterpret_cast<const char*>(frame),
                        total_len, pdMS_TO_TICKS(2000));

                    if (sent == static_cast<int>(total_len)) {
                        ESP_LOGI(kTag, "final_sent confirmed: req=%s seq=%lu",
                                 g_current_req_id.c_str(),
                                 static_cast<unsigned long>(block.seq));
                        g_turn_state.store(TurnState::kRemoteHandoff,
                                           std::memory_order_release);
                        g_final_result =
                            wqn::stdpro_ws::FinalHandoffResult::kFinalSent;
                        if (g_final_sem != nullptr) {
                            xSemaphoreGive(g_final_sem);
                        }
                    } else {
                        ESP_LOGW(kTag,
                                 "FINAL send failed (sent=%d): marking "
                                 "ambiguous",
                                 sent);
                        TeardownFailedTurn();
                    }
                } else {
                    // Regular streaming PCM chunk
                    wqn::EncodeWflvHeader(frame, block.flags, block.seq, 16000, 1);
                    const size_t payload_bytes =
                        block.sample_count * sizeof(int16_t);
                    if (payload_bytes > 0) {
                        std::memcpy(frame + wqn::kWflvHeaderBytes, block.samples,
                                    payload_bytes);
                    }
                    const size_t total_len = wqn::kWflvHeaderBytes + payload_bytes;

                    const int sent = esp_websocket_client_send_bin(
                        g_ws_client, reinterpret_cast<const char*>(frame),
                        total_len, pdMS_TO_TICKS(1500));

                    if (sent != static_cast<int>(total_len)) {
                        ESP_LOGW(kTag,
                                 "PCM send failed: sent=%d expected=%u seq=%lu",
                                 sent, static_cast<unsigned>(total_len),
                                 static_cast<unsigned long>(block.seq));
                        TeardownFailedTurn();
                    } else if (block.seq == 0) {
                        ESP_LOGI(kTag, "first_ws_pcm_sent: seq=0");
                    }
                }
            }
        }
    }
}

SemaphoreHandle_t GetInitMutex()
{
    portENTER_CRITICAL(&g_init_mutex_lock);
    if (g_init_mutex == nullptr) {
        g_init_mutex = xSemaphoreCreateMutexStatic(&g_init_mutex_storage);
    }
    SemaphoreHandle_t mutex = g_init_mutex;
    portEXIT_CRITICAL(&g_init_mutex_lock);
    return mutex;
}

bool PrimitivesReady()
{
    return g_transport_mutex != nullptr && g_sync_op_sem != nullptr &&
        g_final_sem != nullptr && g_turn_complete_sem != nullptr &&
        g_pcm_queue != nullptr && g_ctrl_queue != nullptr &&
        g_ctrl_completion_queue != nullptr && g_text_stream != nullptr &&
        g_pcm_queue_storage != nullptr && g_text_stream_storage != nullptr &&
        g_transport_task != nullptr;
}

void DeleteUncommittedPrimitives(
    SemaphoreHandle_t transport_mutex,
    SemaphoreHandle_t sync_op_sem,
    SemaphoreHandle_t final_sem,
    SemaphoreHandle_t turn_complete_sem,
    QueueHandle_t pcm_queue,
    uint8_t* pcm_queue_storage,
    QueueHandle_t ctrl_queue,
    QueueHandle_t ctrl_completion_queue,
    StreamBufferHandle_t text_stream,
    uint8_t* text_stream_storage)
{
    if (text_stream != nullptr) {
        vStreamBufferDelete(text_stream);
    }
    if (text_stream_storage != nullptr) {
        heap_caps_free(text_stream_storage);
    }
    if (ctrl_completion_queue != nullptr) {
        vQueueDelete(ctrl_completion_queue);
    }
    if (ctrl_queue != nullptr) {
        vQueueDelete(ctrl_queue);
    }
    if (pcm_queue != nullptr) {
        vQueueDelete(pcm_queue);
    }
    if (pcm_queue_storage != nullptr) {
        heap_caps_free(pcm_queue_storage);
    }
    if (turn_complete_sem != nullptr) {
        vSemaphoreDelete(turn_complete_sem);
    }
    if (final_sem != nullptr) {
        vSemaphoreDelete(final_sem);
    }
    if (sync_op_sem != nullptr) {
        vSemaphoreDelete(sync_op_sem);
    }
    if (transport_mutex != nullptr) {
        vSemaphoreDelete(transport_mutex);
    }
}

esp_err_t InitPrimitives()
{
    SemaphoreHandle_t init_mutex = GetInitMutex();
    if (init_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(init_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (PrimitivesReady()) {
        xSemaphoreGive(init_mutex);
        return ESP_OK;
    }

    // Allocate the complete set locally. The task blocks on its commit gate,
    // so no global handle needs to be published until task creation succeeds.
    SemaphoreHandle_t transport_mutex = xSemaphoreCreateMutex();
    SemaphoreHandle_t sync_op_sem = xSemaphoreCreateBinary();
    SemaphoreHandle_t final_sem = xSemaphoreCreateBinary();
    SemaphoreHandle_t turn_complete_sem = xSemaphoreCreateBinary();
    uint8_t* pcm_queue_storage = static_cast<uint8_t*>(heap_caps_malloc(
        kPcmQueueLength * sizeof(PcmBlock),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    QueueHandle_t pcm_queue = pcm_queue_storage == nullptr
        ? nullptr
        : xQueueCreateStatic(
            kPcmQueueLength, sizeof(PcmBlock), pcm_queue_storage,
            &g_pcm_queue_control);
    QueueHandle_t ctrl_queue =
        xQueueCreate(kCtrlQueueLength, sizeof(CtrlCmd));
    QueueHandle_t ctrl_completion_queue =
        xQueueCreate(kCtrlCompletionQueueLength, sizeof(CtrlCompletion));
    uint8_t* text_stream_storage = static_cast<uint8_t*>(heap_caps_malloc(
        kTextStreamBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    StreamBufferHandle_t text_stream = text_stream_storage == nullptr
        ? nullptr
        : xStreamBufferCreateStatic(
            kTextStreamBufferSize, 1, text_stream_storage,
            &g_text_stream_control);

    const bool allocations_ok = transport_mutex != nullptr &&
        sync_op_sem != nullptr && final_sem != nullptr &&
        turn_complete_sem != nullptr && pcm_queue != nullptr &&
        ctrl_queue != nullptr && ctrl_completion_queue != nullptr &&
        text_stream != nullptr;
    if (!allocations_ok) {
        DeleteUncommittedPrimitives(
            transport_mutex, sync_op_sem, final_sem, turn_complete_sem,
            pcm_queue, pcm_queue_storage, ctrl_queue,
            ctrl_completion_queue, text_stream,
            text_stream_storage);
        xSemaphoreGive(init_mutex);
        ESP_LOGE(kTag, "transport primitive allocation failed; rolled back");
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t transport_task = nullptr;
    const BaseType_t created = xTaskCreate(
        VoiceWsTransportTask, "wqn_vws_ctrl", 8192, nullptr,
        kTransportTaskPriority,
        &transport_task);
    if (created != pdPASS) {
        DeleteUncommittedPrimitives(
            transport_mutex, sync_op_sem, final_sem, turn_complete_sem,
            pcm_queue, pcm_queue_storage, ctrl_queue,
            ctrl_completion_queue, text_stream,
            text_stream_storage);
        xSemaphoreGive(init_mutex);
        ESP_LOGE(kTag, "transport task allocation failed; primitives rolled back");
        return ESP_ERR_NO_MEM;
    }

    g_transport_mutex = transport_mutex;
    g_sync_op_sem = sync_op_sem;
    g_final_sem = final_sem;
    g_turn_complete_sem = turn_complete_sem;
    g_pcm_queue = pcm_queue;
    g_pcm_queue_storage = pcm_queue_storage;
    g_ctrl_queue = ctrl_queue;
    g_ctrl_completion_queue = ctrl_completion_queue;
    g_text_stream = text_stream;
    g_text_stream_storage = text_stream_storage;
    g_transport_task = transport_task;
    xTaskNotifyGive(transport_task);
    xSemaphoreGive(init_mutex);
    ESP_LOGI(kTag,
             "transport primitives ready: pcm_psram=%p blocks=%u bytes=%u text_psram=%p bytes=%u",
             g_pcm_queue_storage,
             static_cast<unsigned>(kPcmQueueLength),
             static_cast<unsigned>(kPcmQueueLength * sizeof(PcmBlock)),
             g_text_stream_storage,
             static_cast<unsigned>(kTextStreamBufferSize));
    return ESP_OK;
}

}  // namespace

namespace wqn::stdpro_ws {

esp_err_t EnsureConnected(const std::string& token, uint32_t timeout_ms)
{
    const esp_err_t init_result = InitPrimitives();
    if (init_result != ESP_OK) {
        return init_result;
    }

    CtrlCmd cmd = {};
    if (token.size() >= sizeof(cmd.token)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (xSemaphoreTake(g_transport_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint32_t my_gen = ++g_sync_op_gen;
    const int64_t deadline_ms = (timeout_ms > 0)
        ? ((esp_timer_get_time() / 1000) + timeout_ms)
        : 0;

    cmd.type = CtrlCmdType::kConnect;
    cmd.gen = my_gen;
    cmd.deadline_ms = deadline_ms;
    std::strncpy(cmd.token, token.c_str(), sizeof(cmd.token) - 1);

    if (xQueueSend(g_ctrl_queue, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) {
        xSemaphoreGive(g_transport_mutex);
        return ESP_ERR_TIMEOUT;
    }
    xTaskNotify(g_transport_task, kEvtBitCtrlPending, eSetBits);

    esp_err_t ret = ESP_ERR_TIMEOUT;

    while (true) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (deadline_ms > 0 && now_ms >= deadline_ms) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        const uint32_t wait_ticks = (deadline_ms > 0)
            ? pdMS_TO_TICKS(deadline_ms - now_ms + 100)
            : portMAX_DELAY;

        CtrlCompletion comp;
        if (xQueueReceive(g_ctrl_completion_queue, &comp, wait_ticks) == pdTRUE) {
            if (comp.op_gen == my_gen) {
                ret = comp.result;
                break;
            }
            // Stale completion from an earlier operation; ignore and continue waiting
        } else {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
    }

    xSemaphoreGive(g_transport_mutex);
    return ret;
}

bool IsConnected()
{
    return g_is_connected.load(std::memory_order_acquire) &&
           g_session_ready.load(std::memory_order_acquire);
}

void Disconnect()
{
    if (g_transport_mutex == nullptr || g_sync_op_sem == nullptr ||
        g_ctrl_queue == nullptr || g_ctrl_completion_queue == nullptr ||
        g_transport_task == nullptr) {
        return;
    }

    if (xSemaphoreTake(g_transport_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        xTaskNotify(g_transport_task, kEvtBitEmergencyDisconnect, eSetBits);
        return;
    }

    const uint32_t my_gen = ++g_sync_op_gen;
    const int64_t deadline_ms = (esp_timer_get_time() / 1000) + 1000;

    CtrlCmd cmd = {};
    cmd.type = CtrlCmdType::kDisconnect;
    cmd.gen = my_gen;
    cmd.target_turn_gen = 0;
    cmd.deadline_ms = deadline_ms;

    if (xQueueSend(g_ctrl_queue, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
        xTaskNotify(g_transport_task, kEvtBitCtrlPending, eSetBits);

        while (true) {
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms >= deadline_ms) {
                break;
            }
            CtrlCompletion comp;
            if (xQueueReceive(g_ctrl_completion_queue, &comp,
                              pdMS_TO_TICKS(deadline_ms - now_ms)) == pdTRUE) {
                if (comp.op_gen == my_gen) {
                    break;
                }
            } else {
                break;
            }
        }
    } else {
        // Queue send failed despite mutex: deliver coupled emergency notification
        xTaskNotify(g_transport_task, kEvtBitEmergencyDisconnect, eSetBits);
    }

    xSemaphoreGive(g_transport_mutex);
}

esp_err_t StartTurn(const std::string& request_id,
                    const std::string& tier,
                    const std::string& conversation_id,
                    bool enable_thinking,
                    const char* reasoning_effort,
                    uint32_t* out_turn_gen)
{
    const esp_err_t init_result = InitPrimitives();
    if (init_result != ESP_OK) {
        return init_result;
    }
    if (g_transport_mutex == nullptr || g_sync_op_sem == nullptr ||
        g_ctrl_queue == nullptr || g_ctrl_completion_queue == nullptr ||
        g_transport_task == nullptr ||
        !g_session_ready.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    CtrlCmd cmd = {};
    if (request_id.size() >= sizeof(cmd.request_id) ||
        tier.size() >= sizeof(cmd.tier) ||
        conversation_id.size() >= sizeof(cmd.conversation_id) ||
        (reasoning_effort != nullptr &&
         std::strlen(reasoning_effort) >= sizeof(cmd.reasoning_effort))) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Transport must be IDLE before starting a new turn
    const TurnState turn_state = g_turn_state.load(std::memory_order_acquire);
    if (turn_state != TurnState::kIdle) {
        ESP_LOGW(kTag, "StartTurn rejected: transport state is not IDLE (state=%d)",
                 static_cast<int>(turn_state));
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_transport_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint32_t my_gen = ++g_sync_op_gen;
    const int64_t deadline_ms = (esp_timer_get_time() / 1000) + 2000;

    xSemaphoreTake(g_final_sem, 0);
    xSemaphoreTake(g_turn_complete_sem, 0);

    cmd.type = CtrlCmdType::kStartTurn;
    cmd.gen = my_gen;
    cmd.deadline_ms = deadline_ms;
    std::strncpy(cmd.request_id, request_id.c_str(), sizeof(cmd.request_id) - 1);
    std::strncpy(cmd.tier, tier.c_str(), sizeof(cmd.tier) - 1);
    if (!conversation_id.empty()) {
        std::strncpy(cmd.conversation_id, conversation_id.c_str(),
                     sizeof(cmd.conversation_id) - 1);
    }
    cmd.enable_thinking = enable_thinking;
    if (reasoning_effort != nullptr && reasoning_effort[0] != '\0') {
        std::strncpy(cmd.reasoning_effort, reasoning_effort,
                     sizeof(cmd.reasoning_effort) - 1);
    }

    if (xQueueSend(g_ctrl_queue, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) {
        xSemaphoreGive(g_transport_mutex);
        return ESP_ERR_TIMEOUT;
    }
    xTaskNotify(g_transport_task, kEvtBitCtrlPending, eSetBits);

    esp_err_t ret = ESP_ERR_TIMEOUT;

    while (true) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= deadline_ms) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        const uint32_t wait_ticks = pdMS_TO_TICKS(deadline_ms - now_ms);
        CtrlCompletion comp;
        if (xQueueReceive(g_ctrl_completion_queue, &comp, wait_ticks) == pdTRUE) {
            if (comp.op_gen == my_gen) {
                ret = comp.result;
                if (ret == ESP_OK && out_turn_gen != nullptr) {
                    *out_turn_gen = g_turn_gen.load(std::memory_order_acquire);
                }
                break;
            }
        } else {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
    }

    xSemaphoreGive(g_transport_mutex);
    return ret;
}

void PushPcm(const int16_t* samples, size_t count)
{
    if (g_turn_state.load(std::memory_order_acquire) != TurnState::kRecording ||
        g_pcm_queue == nullptr || samples == nullptr || count == 0 ||
        g_prefinal_pcm_lost.load(std::memory_order_acquire)) {
        return;
    }

    while (count > 0) {
        const size_t chunk_count = std::min(count, kPcmFramesPerBlock);

        PcmBlock block = {};
        block.seq = g_seq.fetch_add(1, std::memory_order_relaxed);
        block.flags = wqn::kWflvFlagStream;
        block.sample_count = static_cast<uint16_t>(chunk_count);
        std::memcpy(block.samples, samples, chunk_count * sizeof(int16_t));

        if (xQueueSend(g_pcm_queue, &block, 0) != pdTRUE) {
            bool expected = false;
            if (g_prefinal_pcm_lost.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                ESP_LOGW(kTag,
                         "PCM queue overflow: queued=%u capacity=%u; latching pre-FINAL failure",
                         static_cast<unsigned>(uxQueueMessagesWaiting(g_pcm_queue)),
                         static_cast<unsigned>(kPcmQueueLength));
                if (g_transport_task != nullptr) {
                    xTaskNotify(
                        g_transport_task, kEvtBitResetRequired, eSetBits);
                }
            }
            break;
        }
        if (g_transport_task != nullptr) {
            xTaskNotify(g_transport_task, kEvtBitPcmPending, eSetBits);
        }

        samples += chunk_count;
        count -= chunk_count;
    }
}

FinalHandoffResult SendFinalAndWait(const std::string& /*request_id*/,
                                   int /*duration_ms*/,
                                   uint32_t timeout_ms)
{
    const TurnState cur = g_turn_state.load(std::memory_order_acquire);
    if (cur != TurnState::kRecording || g_pcm_queue == nullptr ||
        !g_session_ready.load(std::memory_order_acquire) ||
        g_prefinal_pcm_lost.load(std::memory_order_acquire)) {
        return FinalHandoffResult::kDefinitelyNotSent;
    }

    xSemaphoreTake(g_final_sem, 0);

    // Enqueue FINAL into the exact same FIFO queue as PCM chunks
    PcmBlock final_block = {};
    final_block.seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    final_block.flags = wqn::kWflvFlagFinal;
    final_block.sample_count = 0;

    if (xQueueSend(g_pcm_queue, &final_block, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(kTag, "failed to enqueue FINAL block — requesting transport reset");
        if (g_transport_task != nullptr) {
            xTaskNotify(g_transport_task, kEvtBitResetRequired, eSetBits);
        }
        return FinalHandoffResult::kDefinitelyNotSent;
    }

    if (xSemaphoreTake(g_final_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(kTag, "timeout waiting for FINAL send confirmation: returning ambiguous");
        return FinalHandoffResult::kAmbiguous;
    }

    return g_final_result;
}

void AbortTurn(const std::string& request_id, uint32_t target_turn_gen)
{
    if (request_id.empty() || g_transport_task == nullptr) {
        return;
    }
    CtrlCmd cmd = {};
    cmd.type = CtrlCmdType::kAbort;
    cmd.gen = 0;
    cmd.target_turn_gen = target_turn_gen;
    cmd.deadline_ms = 0;
    std::strncpy(cmd.request_id, request_id.c_str(), sizeof(cmd.request_id) - 1);

    if (g_ctrl_queue != nullptr && xQueueSend(g_ctrl_queue, &cmd, 0) == pdTRUE) {
        xTaskNotify(g_transport_task, kEvtBitCtrlPending, eSetBits);
    } else {
        // Queue full: record target identity and deliver coupled emergency abort notification
        portENTER_CRITICAL(&g_emergency_abort_mux);
        g_emergency_abort_target.target_turn_gen = target_turn_gen;
        std::strncpy(g_emergency_abort_target.request_id, request_id.c_str(),
                     sizeof(g_emergency_abort_target.request_id) - 1);
        g_emergency_abort_target.request_id[sizeof(g_emergency_abort_target.request_id) - 1] = '\0';
        portEXIT_CRITICAL(&g_emergency_abort_mux);

        if (g_transport_task != nullptr) {
            xTaskNotify(g_transport_task, kEvtBitEmergencyAbort, eSetBits);
        }
    }
}

void SetSseCallback(WqnAiSseCallback cb, void* user_ctx)
{
    g_sse_callback = cb;
    g_sse_user_ctx = user_ctx;
}

TurnWaitResult WaitForTurnRelease(uint32_t expected_turn_gen,
                                 const std::string& expected_request_id,
                                 uint32_t timeout_ms)
{
    TurnWaitResult result;
    result.turn_gen = expected_turn_gen;
    result.wait_status = TurnWaitStatus::kTimedOut;

    if (g_turn_complete_sem == nullptr || expected_turn_gen == 0) {
        return result;
    }

    const int64_t deadline_ms = (timeout_ms > 0)
        ? ((esp_timer_get_time() / 1000) + timeout_ms)
        : 0;

    while (true) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (deadline_ms > 0 && now_ms >= deadline_ms) {
            result.wait_status = TurnWaitStatus::kTimedOut;
            break;
        }
        const uint32_t wait_ticks = (deadline_ms > 0)
            ? pdMS_TO_TICKS(deadline_ms - now_ms + 10)
            : portMAX_DELAY;

        if (xSemaphoreTake(g_turn_complete_sem, wait_ticks) == pdTRUE) {
            if (g_terminal_turn_record.turn_gen == expected_turn_gen &&
                std::strcmp(g_terminal_turn_record.request_id, expected_request_id.c_str()) == 0) {
                result.wait_status = TurnWaitStatus::kTerminalObserved;
                result.release_state = g_terminal_turn_record.release_state;
                result.stream_observation = g_terminal_turn_record.stream_observation;
                break;
            }
            // Stale terminal record from a previous turn; ignore and continue waiting
        } else {
            result.wait_status = TurnWaitStatus::kTimedOut;
            break;
        }
    }

    if (result.wait_status == TurnWaitStatus::kTimedOut) {
        ESP_LOGW(kTag, "WaitForTurnRelease timed out for req=%s gen=%lu — tearing down",
                 expected_request_id.c_str(), (unsigned long)expected_turn_gen);
        Disconnect();
    }

    return result;
}

bool WaitForTurnComplete(uint32_t timeout_ms)
{
    TurnWaitResult res = WaitForTurnRelease(g_turn_gen.load(std::memory_order_acquire),
                                           g_current_req_id, timeout_ms);
    return (res.wait_status == TurnWaitStatus::kTerminalObserved &&
            res.release_state == TurnReleaseState::kReleased);
}

void SignalTurnComplete()
{
    if (g_turn_complete_sem != nullptr) {
        xSemaphoreGive(g_turn_complete_sem);
    }
}

}  // namespace wqn::stdpro_ws
