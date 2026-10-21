#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=1, ESP_ERR_TIMEOUT=2;
int checks=0;
#define CHECK(c) do { if(!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort(); } ++checks; } while(0)
void Log(const char*,const char*,...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(c) ((void)(c))
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
constexpr char kTag[]="host";
constexpr int64_t kCallTimeoutUs=100;
int64_t esp_timer_get_time() { return 1000; }
const char* esp_err_to_name(int) { return "host-result"; }
namespace wqn::services {
@@AUDIO_TYPES@@
const char* AudioActivityName(AudioActivity) { return "activity"; }
const char* AudioStateName(AudioState) { return "state"; }
}
using wqn::services::AudioActivity;
using wqn::services::AudioState;
using wqn::services::AudioSession;
std::atomic<uint32_t> g_session_id{0}, g_inflight_operations{0};
std::atomic<AudioActivity> g_activity{AudioActivity::kCapture};
std::atomic<AudioState> g_state{AudioState::kIdle};
std::atomic<bool> g_accept_operations{false};
int g_resource_lock=0;
struct ResourceSlot { void* handle=nullptr;uint32_t owner_session_id=0;bool closing=false;unsigned inflight=0; };
constexpr size_t kMaxCodecHandles=2,kMaxChannelHandles=2;
ResourceSlot g_codec_slots[2],g_channel_slots[2];
AudioState StateForActivity(AudioActivity activity) {
    if(activity==AudioActivity::kCapture)return AudioState::kCapturing;
    if(activity==AudioActivity::kFlash)return AudioState::kFlash;
    if(activity==AudioActivity::kPlayback)return AudioState::kPlaying;
    return AudioState::kSelfTest;
}
@@DRIVER_GUARD@@
int gpio_writes=0,led_level=1,gpio_result=ESP_OK;
constexpr int kLed=3;
int gpio_set_level(int pin,int level) { CHECK(pin==3);++gpio_writes;if(gpio_result==ESP_OK)led_level=level;return gpio_result; }
namespace wqn {
@@HAL@@
}
namespace wqn::services {
@@SET@@
}
struct AudioCommand { uint32_t session_id;AudioActivity activity; };
int DrainDriverOperations(int64_t) { return g_inflight_operations==0?ESP_OK:ESP_ERR_TIMEOUT; }
bool resources_live=false;
bool HasRegisteredResources(uint32_t) { return resources_live; }
int ApplyAmplifier(bool enabled) { CHECK(!enabled);return ESP_OK; }
void SetState(AudioState state) { g_state=state; }
struct Lease { int resets=0;void Reset() { ++resets; } } g_audio_lease;
@@END@@
namespace capture {
struct State { AudioSession indicator_session;bool running=false,stop_requested=false,recording_indicator_on=false; } g_audio;
@@CAPTURE@@
}
namespace flash {
enum class InternalStatus { kIdle,kConnecting,kStreaming,kError };
struct State {
    bool capture_started=false,ws_connected=false,response_in_flight=false,response_started=false;
    InternalStatus status=InternalStatus::kIdle;
    std::string pending_text,error_message;int64_t status_since_ms=0;
} g_flash;
AudioSession g_flash_indicator_session;
bool g_flash_indicator_on=false;
void MarkChanged() {}
@@FLASH@@
}
AudioSession Begin(uint32_t id,AudioActivity activity) {
    g_session_id=id;g_activity=activity;g_state=StateForActivity(activity);g_accept_operations=true;
    return {id,activity};
}
int main() {
    CHECK(led_level==1);
    auto cap=Begin(1,AudioActivity::kCapture);
    capture::g_audio.indicator_session=cap;capture::g_audio.running=true;
    CHECK(led_level==1); // Admission/setup is not readiness.
    capture::g_audio.stop_requested=true;capture::SetCaptureIndicatorLocked(true);
    CHECK(gpio_writes==0&&led_level==1); // Cancel while warming up.
    capture::g_audio.stop_requested=false;capture::SetCaptureIndicatorLocked(true);
    CHECK(led_level==0);CHECK(capture::g_audio.recording_indicator_on);
    const int writes=gpio_writes;capture::SetCaptureIndicatorLocked(true);CHECK(gpio_writes==writes);
    capture::g_audio.stop_requested=true;capture::SetCaptureIndicatorLocked(false);CHECK(led_level==1);
    capture::SetCaptureIndicatorLocked(true);CHECK(led_level==1&&gpio_writes==writes+1);
    // The actual service guard keeps a GPIO write in flight until it completes.
    {
        DriverOperation operation(cap);CHECK(static_cast<bool>(operation));CHECK(g_inflight_operations==1);
        CHECK(EndActivity({1,AudioActivity::kCapture})==ESP_ERR_TIMEOUT);CHECK(g_session_id==1);
    }
    CHECK(g_inflight_operations==0);
    CHECK(EndActivity({1,AudioActivity::kCapture})==ESP_OK);CHECK(led_level==1);CHECK(g_session_id==0);
    CHECK(wqn::services::SetAudioRecordingIndicator(cap,true)==ESP_ERR_INVALID_STATE);
    const auto newer=Begin(2,AudioActivity::kCapture);
    CHECK(wqn::services::SetAudioRecordingIndicator(newer,true)==ESP_OK);CHECK(led_level==0);
    CHECK(wqn::services::SetAudioRecordingIndicator(cap,false)==ESP_ERR_INVALID_STATE);CHECK(led_level==0);
    CHECK(wqn::services::SetAudioRecordingIndicator({2,AudioActivity::kFlash},false)==ESP_ERR_INVALID_STATE);
    CHECK(EndActivity({2,AudioActivity::kCapture})==ESP_OK);CHECK(led_level==1);
    const int before_invalid=gpio_writes;
    CHECK(wqn::services::SetAudioRecordingIndicator({},true)==ESP_ERR_INVALID_STATE);
    g_accept_operations=false;
    CHECK(wqn::services::SetAudioRecordingIndicator(newer,true)==ESP_ERR_INVALID_STATE);
    g_session_id=2;g_activity=AudioActivity::kCapture;g_state=AudioState::kQuiescing;g_accept_operations=true;
    CHECK(wqn::services::SetAudioRecordingIndicator(newer,true)==ESP_ERR_INVALID_STATE);
    CHECK(gpio_writes==before_invalid&&led_level==1);
    auto playback=Begin(3,AudioActivity::kPlayback);
    CHECK(wqn::services::SetAudioRecordingIndicator(playback,true)==ESP_ERR_INVALID_STATE);CHECK(led_level==1);
    CHECK(EndActivity({3,AudioActivity::kPlayback})==ESP_OK);
    auto realtime=Begin(4,AudioActivity::kFlash);flash::g_flash_indicator_session=realtime;
    flash::g_flash.status=flash::InternalStatus::kConnecting;
    flash::SetFlashCaptureIndicatorLocked(true);CHECK(led_level==1);
    flash::g_flash.capture_started=true;flash::g_flash.ws_connected=true;flash::g_flash.status=flash::InternalStatus::kStreaming;
    CHECK(led_level==1); // Network ready alone does not light the LED.
    flash::SetFlashCaptureIndicatorLocked(true);CHECK(led_level==0);
    flash::g_flash.capture_started=false;flash::SetFlashCaptureIndicatorLocked(false);CHECK(led_level==1);
    flash::SetFlashCaptureIndicatorLocked(true);CHECK(led_level==1); // Late read after release.
    flash::g_flash.capture_started=true;flash::SetFlashCaptureIndicatorLocked(true);CHECK(led_level==0);
    flash::SetFlashCaptureIndicatorLocked(false);CHECK(led_level==1); // Read failure.
    flash::SetFlashCaptureIndicatorLocked(true);CHECK(led_level==0); // First recovered PCM.
    flash::SetErrorLocked("transport failed");CHECK(led_level==1);
    flash::SetFlashCaptureIndicatorLocked(true);CHECK(led_level==1); // Late read after error.
    flash::g_flash.status=flash::InternalStatus::kStreaming;flash::g_flash.ws_connected=false;
    flash::SetFlashCaptureIndicatorLocked(true);CHECK(led_level==1);
    flash::g_flash.ws_connected=true;gpio_result=ESP_ERR_TIMEOUT;
    flash::SetFlashCaptureIndicatorLocked(true);CHECK(!flash::g_flash_indicator_on&&led_level==1);
    gpio_result=ESP_OK;flash::SetFlashCaptureIndicatorLocked(true);CHECK(led_level==0);
    CHECK(EndActivity({4,AudioActivity::kFlash})==ESP_OK);CHECK(led_level==1); // Service fallback even if a caller missed stop.
    CHECK(g_audio_lease.resets==4);CHECK(g_inflight_operations==0);
    std::printf("Recording indicator production gates: %d assertions passed (ASan/UBSan)\n",checks);
}
