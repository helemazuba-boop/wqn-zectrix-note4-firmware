#!/usr/bin/env python3
"""Real Flash parser/history on host ports; excludes scheduling, I2S and HIL."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_audio_runtime import function

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5')) / 'components/json/cJSON'
source = (ROOT / 'main/flash_session.cpp').read_text()
source = source[:source.rindex('\n#else')]
header = (ROOT / 'main/flash_session.h').read_text()
pre = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "ai_history.h"
#include "flash_frame_assembly.h"
#include "device_protocol/json_depth_guard.h"
#include "cJSON.h"
#include "esp_log.h"
#define CONFIG_WQN_FLASH_PROTOCOL_V2 1
using TaskHandle_t=void*;
using RingbufHandle_t=void*;
using esp_websocket_client_handle_t=void*;
namespace wqn {
enum class AiTier { kStd,kFlash,kAgent };
namespace services { using AudioChannelHandle=void*;using AudioBusHandle=void*; }
int InitFlashSession(){return ESP_OK;}
}
constexpr char kTag[]="flash-host";
int64_t now_us=1000000;
int64_t esp_timer_get_time(){return now_us;}
void* g_lifecycle_task=reinterpret_cast<void*>(3);
void xTaskNotifyGive(void*){}
void SetFlashCaptureIndicatorLocked(bool){}
void LogFlashHeapPoint(const char*){}
std::atomic<bool> g_teardown_pending{false},g_start_pending{false};
std::atomic<uint32_t> g_session_generation{1},g_start_generation{0};
'''
body = function(source, 'enum class InternalStatus {') + ';\n'
body += function(source, 'enum class FlashTerminalReason : uint8_t {') + ';\n'
body += function(source, 'struct FlashToolCard {') + ';\n'
body += function(source, 'struct FlashState {') + ';\nFlashState g_flash;\n'
body += '''int terminal_requests=0;
void RequestFlashTerminalStop(FlashTerminalReason){++terminal_requests;}
void DrainPlaybackRingbuf(){
 g_flash.playback_generation.fetch_add(1);g_flash.playback_epoch_valid=false;
 g_flash.playback_accepting=false;g_flash.playback_abort_requested=true;
}
void SetStreamAudioAmp(bool){}
'''
for signature in [
    'std::string BoundedFlashText(', 'bool EnsureFlashUserHistoryLocked(',
    'bool MirrorFlashMessageLocked(', 'bool EnsureFlashThinkingHistoryLocked(',
    'bool FinalizeFlashAssistantLocked(', 'void ResetFlashTurnAssemblyLocked()',
    'void MarkChanged()', 'void SetErrorLocked(const std::string& message)\n{',
    'struct CJsonGuard {', 'const char* JsonStr(',
    'void ParseAndHandleEvent(const char* data, size_t len)\n{', 'void CheckAmpIdleTail()\n{',
]:
    body += function(source, signature) + (';\n' if signature.startswith('struct ') else '\n')
body += '\nnamespace wqn {\n'
body += function(header, 'enum class FlashStatus {') + ';\n'
body += function(header, 'struct FlashUiState {') + ';\n'
body += function(source, 'esp_err_t StartFlashSession()\n{') + '\n'
body += function(source, 'bool CopyFlashStateToUi(') + '\n}\n'
main = r'''
int checks=0;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);std::abort();}++checks;}while(0)
void event(const std::string& text){ParseAndHandleEvent(text.data(),text.size());}
auto snapshot(){return wqn::GetAiHistorySnapshot(wqn::AiHistoryChannel::kFlash);}
void reset(){
 g_flash.mutex=reinterpret_cast<void*>(1);
 wqn::GetAiHistory(wqn::AiHistoryChannel::kFlash).Clear();ResetFlashTurnAssemblyLocked();
 g_flash.status=InternalStatus::kStreaming;g_flash.ws_connected=true;g_flash.tool_cards.clear();
 g_flash.response_in_flight=true;g_flash.awaiting_server_turn=true;g_flash.changed=false;
 g_flash.error_message.clear();terminal_requests=0;
 event(R"({"type":"turn.started","turn_id":7})");
}
int main(){
 reset();
 event(R"({"type":"asr.delta","turn_id":7,"delta":"partial"})");
 CHECK(snapshot()->messages.size()==1&&snapshot()->messages[0].text=="partial");
 const auto user_id=snapshot()->messages[0].id;
 event(R"({"type":"asr.complete","turn_id":7,"transcript":"question"})");
 CHECK(snapshot()->messages[0].id==user_id&&snapshot()->messages[0].text=="question");
 event(R"({"type":"response.created","turn_id":7,"response":{"id":"r1"}})");
 event(R"({"type":"text.delta","turn_id":7,"delta":"live answer"})");
 CHECK(snapshot()->messages.size()==2&&snapshot()->messages[1].text=="live answer");
 event(R"({"type":"response.audio_transcript.delta","turn_id":7,"delta":"live answer"})");
 CHECK(snapshot()->messages.size()==2&&snapshot()->messages[1].text=="live answer");
 event(R"({"type":"response.done","turn_id":7,"response":{"status":"completed"}})");
 CHECK(!g_flash.response_in_flight&&g_flash.status==InternalStatus::kStreaming);
 CHECK(snapshot()->messages.size()==2);
 const auto old_answer=snapshot()->messages[1].id;
 g_flash.status=InternalStatus::kIdle;g_flash.ws_client=nullptr;
 CHECK(wqn::StartFlashSession()==ESP_OK);
 CHECK(g_flash.user_message_id==0&&g_flash.assistant_message_id==0&&!g_flash.history_user_committed);
 event(R"({"type":"text.delta","turn_id":7,"delta":"stale"})");
 CHECK(snapshot()->messages[1].text=="live answer");
 g_flash.response_in_flight=true;g_flash.awaiting_server_turn=true;
 event(R"({"type":"turn.started","turn_id":8})");
 event(R"({"type":"response.created","turn_id":8,"response":{"id":"r2"}})");
 event(R"({"type":"response.text.delta","turn_id":8,"delta":"new answer"})");
 event(R"({"type":"response.done","turn_id":8,"response":{"status":"completed"}})");
 CHECK(snapshot()->messages.size()==4&&snapshot()->messages[1].id==old_answer);
 CHECK(snapshot()->messages[2].kind==wqn::ChatMessageKind::kUser);
 CHECK(snapshot()->messages[2].text=="语音提问（未识别到文本）");
 CHECK(snapshot()->messages[3].text=="new answer");
 reset();
 event(R"({"type":"tool.start","turn_id":7,"call_id":"c1","name":"create_todo","args":"{}"})");
 CHECK(snapshot()->messages.size()==2&&snapshot()->messages[1].kind==wqn::ChatMessageKind::kToolStart);
 const auto tool_id=snapshot()->messages[1].id;
 ResetFlashTurnAssemblyLocked();g_flash.response_in_flight=true;g_flash.awaiting_server_turn=true;
 event(R"({"type":"turn.started","turn_id":8})");
 event(R"({"type":"text.delta","turn_id":8,"delta":"current answer"})");
 event(R"({"type":"tool.receipt","turn_id":7,"call_id":"c1","ok":true,"display":"Saved","retired":true})");
 CHECK(snapshot()->messages.size()==4);
 CHECK(snapshot()->messages[1].id==tool_id&&snapshot()->messages[1].kind==wqn::ChatMessageKind::kToolResult);
 CHECK(snapshot()->messages[1].tool_ok&&snapshot()->messages[1].tool_result_json=="Saved");
 CHECK(snapshot()->messages[3].text=="current answer");
 event(R"({"type":"tool.receipt","turn_id":7,"call_id":"unknown","ok":true,"display":"wrong"})");
 CHECK(snapshot()->messages.size()==4);
 reset();
 event(R"({"type":"text.delta","turn_id":7,"delta":"partial answer"})");
 event(R"({"type":"response.done","turn_id":7,"response":{"status":"failed"}})");
 CHECK(g_flash.status==InternalStatus::kError&&!g_flash.response_in_flight&&terminal_requests==1);
 CHECK(snapshot()->messages[1].text=="partial answer"&&snapshot()->messages.size()==3);
 reset();event(R"({"type":"error","error_code":"internal","message":"precise relay failure"})");
 CHECK(g_flash.error_message=="precise relay failure"&&terminal_requests==1);
 CHECK(snapshot()->messages.back().text=="提示：precise relay failure");
 reset();
 event(R"({"type":"text.complete","turn_id":7,"text":"complete only"})");
 CHECK(snapshot()->messages.size()==2&&snapshot()->messages[1].text=="complete only");
 const auto complete_id=snapshot()->messages[1].id;
 event(R"({"type":"text.complete","turn_id":7,"text":"complete only"})");
 CHECK(snapshot()->messages.size()==2&&snapshot()->messages[1].id==complete_id);
 reset();
 event("{\"type\":\"text.delta\",\"turn_id\":7,\"delta\":\""+std::string(100000,'a')+"\"}");
 CHECK(g_flash.assistant_text.size()<=32768);
 CHECK(g_flash.assistant_text.find("显示已截断")!=std::string::npos);
 wqn::GetAiHistory(wqn::AiHistoryChannel::kFlash).Clear();
 event(R"({"type":"text.delta","turn_id":7,"delta":"tail"})");
 CHECK(snapshot()->messages.size()==2&&snapshot()->messages[1].kind==wqn::ChatMessageKind::kAssistant);
 reset();g_flash.amp_idle_armed=true;g_flash.amp_idle_due_ms=0;g_flash.changed=false;
 CheckAmpIdleTail();CHECK(!g_flash.amp_idle_armed&&g_flash.changed);
 wqn::FlashUiState ui;CHECK(wqn::CopyFlashStateToUi(&ui)&&!ui.playback_active);
 CHECK(!wqn::CopyFlashStateToUi(&ui));
 std::printf("Flash parser/history: %d checks PASS (host ports, no HIL)\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='flash-events-') as directory:
    folder = Path(directory)
    (folder/'freertos').mkdir()
    stubs = {
        'esp_err.h': '#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=257,ESP_ERR_INVALID_STATE=259;\n',
        'freertos/FreeRTOS.h': '#pragma once\nstruct StaticSemaphore_t{};using SemaphoreHandle_t=void*;constexpr int portMAX_DELAY=0;\n',
        'freertos/semphr.h': '#pragma once\n#include "FreeRTOS.h"\ninline void* xSemaphoreCreateMutexStatic(StaticSemaphore_t*){return reinterpret_cast<void*>(1);}\ninline int xSemaphoreTake(void*,int){return 1;}\ninline int xSemaphoreGive(void*){return 1;}\n',
        'esp_log.h': '#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n',
        'esp_heap_caps.h': '#pragma once\n#include <cstdlib>\n#include <algorithm>\nconstexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_INTERNAL=4;\ninline size_t heap_caps_get_total_size(int){return 1024*1024;}\ninline void* heap_caps_aligned_alloc(size_t a,size_t b,int){void* p=nullptr;return posix_memalign(&p,std::max(a,sizeof(void*)),b)==0?p:nullptr;}\ninline void heap_caps_free(void* p){std::free(p);}\n',
    }
    for name, text in stubs.items(): (folder/name).write_text(text)
    unit = folder/'events.cpp'
    unit.write_text(pre+body+main)
    flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie']
    subprocess.run(['gcc', '-std=c99', *flags, '-I', str(SDK), '-c', str(SDK/'cJSON.c'), '-o', str(folder/'cjson.o')], check=True, timeout=60)
    subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-variable', '-Wno-unused-parameter', *flags, '-no-pie',
                    '-I', str(folder), '-I', str(ROOT/'main'), '-I', str(ROOT/'components/device_protocol/include'), '-I', str(SDK),
                    str(unit), str(ROOT/'main/ai_history.cpp'), str(folder/'cjson.o'), '-o', str(folder/'events')], check=True, timeout=60)
    subprocess.run([str(folder/'events')], check=True, timeout=30)
