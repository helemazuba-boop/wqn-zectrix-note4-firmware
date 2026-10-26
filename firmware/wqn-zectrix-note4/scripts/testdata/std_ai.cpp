#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "wqn_api.h"
#include "agent_round_policy.h"

static int checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
#define ESP_RETURN_ON_ERROR(call,tag,message) do { auto result=(call); if(result!=ESP_OK) return result; } while(0)
#define ESP_LOGW(...) do {} while(0)
constexpr int portMAX_DELAY=0;
constexpr esp_err_t ESP_ERR_INVALID_SIZE=0x104;
int64_t esp_timer_get_time() { return 1000000; }
void xSemaphoreTake(void*,int) {}
void xSemaphoreGive(void*) {}
int notifications=0;
void xTaskNotifyGive(void*) { ++notifications; }

namespace wqn {
@@MODEL@@
namespace runtime {
enum class SleepBlocker { kAiSession };
struct SleepLease {
    explicit operator bool() const { return true; }
    static SleepLease TryAcquire(SleepBlocker,const char*,const char*,int) { return {}; }
};
}
namespace services {
enum class ConnectivityDemandReason { kAiInteractive };
struct ConnectivityDemand { void Reset() {} };
ConnectivityDemand AcquireConnectivityDemand(ConnectivityDemandReason,const char*,const char*,int) { return {}; }
}
struct AudioCaptureChunk { int duration_ms=1500; size_t sample_count=24000; const int16_t* samples=nullptr; int peak=100,rms=10; };
AiSessionState g_state;
enum class AiWorkerCommand { kNone,kPrepareRecording,kSubmitSession,kSubmitPrompt,kCancelCapture };
AiWorkerCommand g_worker_command=AiWorkerCommand::kNone;
uint32_t g_worker_command_since_ms=0,g_prepare_generation=0;
void* g_lock=reinterpret_cast<void*>(1);
void* g_ai_worker=reinterpret_cast<void*>(1);
bool g_prepare_active=false,g_recording_requested=false,g_turn_ws_capable=false;
bool g_reply_pending=false,cancelled=false;
std::atomic<bool> g_prompt_cancel_requested{false};
std::string g_current_turn_req_id="clip_request",g_prompt_request_id;
runtime::SleepLease g_ai_sleep_lease;
services::ConnectivityDemand g_ai_connectivity_demand;
constexpr const char* kTag="std-fixture";
constexpr int kMinAudioDurationMs=1000,kMinAudioPeak=80,kMinAudioRms=8;
constexpr size_t kMinAudioSamples=16000;
int changes=0,asr_uploads=0,replies=0;
esp_err_t asr_result=ESP_OK,reply_result=ESP_OK;
std::string recognized="把这句话写入笔记",sent_id,sent_answer;
bool replace_ask_during_reply=false;
void MarkChanged() { ++changes; }
void ReleaseAiSleepLeaseIfIdleLocked() {}
void LogAiMemory(const char*, const char* = nullptr) {}
void LogAiTransport(const char*, const char*, uint32_t, const char*, const char*, esp_err_t, int64_t) {}
uint32_t g_current_turn_gen=1;
void ReportStuckWorkerIfAnyLocked(const char*) {}
void ArmAiFollowLocked() { g_state.follow_active=true;g_state.user_moved=false; }
void SetErrorLocked(const std::string& text) { g_state.status=AiSessionStatus::kError;g_state.assistant_text=text; }
void SetCancelledBeforeRecordingLocked() { g_state.status=AiSessionStatus::kIdle; }
esp_err_t InitAiSession() { return ESP_OK; }
std::string GenerateAiRequestId() { return "0123456789abcdef"; }
esp_err_t StopAudioCapture(AudioCaptureChunk*) { return ESP_OK; }
void ReleaseAudioCapturePower() {}
constexpr int kAudioCaptureSampleRate=16000;
constexpr int kMaxAudioDurationMs=20000;
void DumpCapturedPcmForAnalysis(const int16_t*,size_t,int) {}
void AgentVoiceTurnAbort() {}
bool AgentVoiceCancelRequested() { return cancelled; }
void AgentVoiceClearCancel() { cancelled=false; }
esp_err_t AgentVoiceTurnFinalize(int,uint32_t,std::string* transcript) { *transcript=recognized;return asr_result; }
esp_err_t LoadAccessTokenFromStorageService(std::string* token) { *token="fixture";return ESP_OK; }
esp_err_t UploadAiAudioChatStream(const WqnAiStreamRequest& request,WqnAiChatResponse* response) {
    CHECK(request.transcribe_only);CHECK(request.confirmed_text.empty());
    ++asr_uploads;response->transcript=recognized;return ESP_OK;
}
esp_err_t PostAiInteractionReply(const std::string&,const std::string& id,const std::string& answer,std::string* error) {
    ++replies;sent_id=id;sent_answer=answer;*error="fixture loss";
    if(replace_ask_during_reply) g_state.interaction.id="newer";
    return reply_result;
}
@@FUNCTIONS@@
}

int main() {
    using namespace wqn;
    g_state.status=AiSessionStatus::kListening;
    CHECK(StopAiRecordingAndSubmit()==ESP_OK);
    CHECK(g_worker_command==AiWorkerCommand::kSubmitSession);
    CHECK(g_state.status==AiSessionStatus::kTranscribing);
    TranscribeInput();
    CHECK(g_state.status==AiSessionStatus::kAwaitingConfirmation);
    CHECK(g_state.pending_input==recognized&&g_state.user_text.empty());
    CHECK(g_worker_command==AiWorkerCommand::kNone);
    CHECK(ConfirmAiPrompt(999)==ESP_ERR_INVALID_STATE);
    CHECK(ConfirmAiPrompt(1000)==ESP_OK);
    CHECK(g_worker_command==AiWorkerCommand::kSubmitPrompt);
    CHECK(g_state.user_text==recognized&&g_state.pending_input.empty());
    const int dispatched=notifications;
    CHECK(ConfirmAiPrompt(1001)==ESP_ERR_INVALID_STATE&&notifications==dispatched);

    g_state={};g_state.status=AiSessionStatus::kAwaitingConfirmation;g_state.pending_input="unsent";
    g_worker_command=AiWorkerCommand::kNone;CancelAiPrompt();
    CHECK(g_state.pending_input.empty()&&g_state.status==AiSessionStatus::kIdle);
    CHECK(ConfirmAiPrompt(2000)==ESP_ERR_INVALID_STATE);

    g_state.status=AiSessionStatus::kTranscribing;g_worker_command=AiWorkerCommand::kSubmitSession;
    asr_result=ESP_ERR_INVALID_STATE;TranscribeInput();
    CHECK(asr_uploads==1&&g_state.status==AiSessionStatus::kAwaitingConfirmation);
    g_state.status=AiSessionStatus::kTranscribing;g_worker_command=AiWorkerCommand::kSubmitSession;
    asr_result=ESP_OK;cancelled=true;TranscribeInput();
    CHECK(g_state.pending_input.empty()&&g_state.status==AiSessionStatus::kIdle);

    g_state.status=AiSessionStatus::kAwaitingDecision;
    AiInteraction interaction;
    interaction.id="original";
    ReceiveInteractionLocked(interaction,2500);
    CHECK(g_state.interaction.armed_at_ms==2500);
    g_state.interaction.options={{"yes","是"},{"no","否"}};
    g_state.interaction.allow_custom=true;
    CHECK(ReplyAiInteraction(3)==ESP_ERR_INVALID_STATE);
    CHECK(ReplyAiInteraction(1)==ESP_OK);
    ReceiveInteractionLocked(interaction,2600);
    CHECK(g_state.interaction.armed_at_ms==2500&&g_reply_pending&&g_state.interaction.reply_value=="no");
    CHECK(ReplyAiInteraction(1)==ESP_ERR_INVALID_STATE);
    std::string token="fixture";reply_result=ESP_FAIL;PumpInteractionReply(&token);
    CHECK(sent_id=="original"&&sent_answer=="no");
    CHECK(!g_state.interaction.sending&&!g_state.interaction.error.empty());
    CHECK(ReplyAiInteraction(0)==ESP_ERR_INVALID_STATE);
    CHECK(ReplyAiInteraction(1)==ESP_OK);reply_result=ESP_OK;PumpInteractionReply(&token);
    CHECK(g_state.interaction.id.empty()&&g_state.status==AiSessionStatus::kStreaming);
    g_state.interaction.id="question";g_state.interaction.options={{"yes","是"}};g_state.interaction.allow_custom=true;
    CHECK(ReplyAiInteraction(1)==ESP_OK);PumpInteractionReply(&token);CHECK(sent_answer=="__custom__");
    g_state.interaction.id="old";g_state.interaction.options={{"yes","是"}};
    CHECK(ReplyAiInteraction(0)==ESP_OK);replace_ask_during_reply=true;PumpInteractionReply(&token);
    CHECK(g_state.interaction.id=="newer");
    std::printf("STD production session: %d assertions passed\n",checks);
}
