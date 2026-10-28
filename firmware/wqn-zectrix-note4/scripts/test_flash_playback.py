#!/usr/bin/env python3
"""Exercise real playback admission/owner with bounded ring and I2S host ports."""
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'main/flash_session.cpp').read_text()
pre = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>
constexpr int ESP_OK=0,portMAX_DELAY=0,pdTRUE=1;
using esp_err_t=int;using BaseType_t=int;
#define pdMS_TO_TICKS(x) (x)
template<class... T> void Log(T...){}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
constexpr char kTag[]="playback-host";
constexpr size_t kPlaybackRingbufItemMax=4096,kStreamTxDmaBytes=6144;
constexpr int kSampleRate=24000,kStreamDmaDescNum=6,kStreamDmaFrameNum=256,kAmpIdleTailMs=600;
struct Pause{};
struct Ring { std::deque<std::vector<uint8_t>> items;std::vector<uint8_t> borrowed;size_t used=0,capacity=524288; } ring;
struct {
 void* mutex=reinterpret_cast<void*>(1);Ring* playback_ringbuf=&ring;
 std::atomic<size_t> playback_queued_bytes{0};
 std::atomic<uint32_t> playback_generation{1},playback_reset_generation{0};
 std::atomic<bool> playback_stop{false},drain_playback{false};
 bool playback_accepting=true,playback_epoch_valid=true,playback_abort_requested=false;
 bool capture_started=false,amp_idle_armed=false,playback_write_active=false,changed=false;
 int64_t amp_idle_due_ms=0;size_t playback_dropped_bytes=0;
 void* playback_task=reinterpret_cast<void*>(1);void* stream_tx=reinterpret_cast<void*>(2);
} g_flash;
std::atomic<bool> g_teardown_pending{false};
int g_flash_audio_session=1,lock_depth=0,stop_requests=0,tx_resets=0;
int64_t clock_us=0;
bool amplifier=false;
std::function<void()> write_hook,delay_hook;
std::vector<int> played;
void xSemaphoreTake(void*,int){assert(lock_depth==0);++lock_depth;}
void xSemaphoreGive(void*){assert(lock_depth==1);--lock_depth;}
void MarkChanged(){g_flash.changed=true;}
void SetStreamAudioAmp(bool enabled){assert(lock_depth==0);amplifier=enabled;}
int64_t esp_timer_get_time(){return clock_us;}
void vTaskDelay(int ms){clock_us+=ms*1000;if(delay_hook)delay_hook();}
void vTaskDelete(void*){throw Pause{};}
enum class FlashTerminalReason{kPlaybackIncomplete};
void RequestFlashTerminalStop(FlashTerminalReason){++stop_requests;}
int xRingbufferSend(Ring* r,const void* data,size_t size,int){
 if(r->used+size>r->capacity)return 0;
 const auto* p=static_cast<const uint8_t*>(data);r->items.emplace_back(p,p+size);r->used+=size;return pdTRUE;
}
void* xRingbufferReceive(Ring* r,size_t* size,int){
 assert(r->borrowed.empty());if(r->items.empty())throw Pause{};
 r->borrowed=std::move(r->items.front());r->items.pop_front();*size=r->borrowed.size();return r->borrowed.data();
}
void vRingbufferReturnItem(Ring* r,void* item){assert(item==r->borrowed.data());r->used-=r->borrowed.size();r->borrowed.clear();}
namespace wqn::services {
int ResetAudioTxChannel(int,void*,size_t){assert(lock_depth==0);++tx_resets;return ESP_OK;}
int WriteAudioChannel(int,void*,const uint8_t* data,size_t size,size_t* written,int){
 assert(lock_depth==0&&amplifier);int16_t sample;std::memcpy(&sample,data,2);played.push_back(sample);
 *written=size;clock_us+=1000;if(write_hook){auto hook=std::move(write_hook);write_hook={};hook();}return ESP_OK;
}
}
'''
body = '\n'.join(function(source, name) for name in [
    'void CheckAmpIdleTail()\n{', 'void RetirePlaybackBytes(', 'bool EnqueuePlaybackPcm(',
    'void DrainPlaybackRingbuf()', 'bool WaitForPlaybackReset()', 'void FlashPlaybackTask(',
])
main = r'''
void run(){try{FlashPlaybackTask(nullptr);}catch(const Pause&){}assert(lock_depth==0&&ring.borrowed.empty());}
void reset(){ring={};g_flash.playback_queued_bytes=0;g_flash.playback_generation=1;g_flash.playback_reset_generation=0;
 g_flash.playback_stop=false;g_flash.drain_playback=false;g_flash.playback_accepting=true;g_flash.playback_epoch_valid=true;
 g_flash.playback_abort_requested=false;g_flash.capture_started=false;g_flash.amp_idle_armed=false;
 g_flash.playback_write_active=false;g_flash.changed=false;g_teardown_pending=false;
 stop_requests=tx_resets=0;clock_us=0;amplifier=false;played.clear();write_hook={};delay_hook={};}
int main(){
 reset();std::vector<int16_t> old(2500,11),fresh(480,22);
 assert(EnqueuePlaybackPcm(reinterpret_cast<uint8_t*>(old.data()),old.size()*2));
 assert(g_flash.playback_queued_bytes==5000);
 DrainPlaybackRingbuf();g_flash.playback_accepting=true;g_flash.playback_abort_requested=false;
 assert(EnqueuePlaybackPcm(reinterpret_cast<uint8_t*>(fresh.data()),fresh.size()*2));
 run();assert(played==std::vector<int>{22}&&g_flash.playback_queued_bytes==0&&ring.used==0);
 assert(tx_resets==1&&stop_requests==0&&g_flash.amp_idle_armed);
 clock_us+=2000000;g_flash.changed=false;run();assert(!amplifier&&!g_flash.amp_idle_armed&&g_flash.changed);
 reset();assert(EnqueuePlaybackPcm(reinterpret_cast<uint8_t*>(old.data()),old.size()*2));
 write_hook=[&]{DrainPlaybackRingbuf();g_flash.playback_accepting=true;g_flash.playback_abort_requested=false;
  assert(EnqueuePlaybackPcm(reinterpret_cast<uint8_t*>(fresh.data()),fresh.size()*2));};
 run();assert(played==std::vector<int>({11,22})&&g_flash.playback_queued_bytes==0);
 assert(tx_resets==1&&stop_requests==0); // stale second old packet never plays
 reset();ring.capacity=0;assert(!EnqueuePlaybackPcm(reinterpret_cast<uint8_t*>(fresh.data()),fresh.size()*2));
 assert(stop_requests==1&&g_flash.playback_queued_bytes==0&&ring.used==0);
 reset();assert(EnqueuePlaybackPcm(reinterpret_cast<uint8_t*>(fresh.data()),fresh.size()*2));
 g_flash.playback_abort_requested=true;run();assert(played.empty()&&g_flash.playback_queued_bytes==0);
 reset();int steps=0;delay_hook=[&]{++steps;if(steps==1)++g_flash.playback_generation;
  if(steps==2)g_flash.playback_reset_generation=g_flash.playback_generation.load();};
 assert(WaitForPlaybackReset()&&steps==2); // latest reset supersedes the first
 std::puts("Flash playback owner: stale/new packets, abort accounting, overflow, reset race and idle publication PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='flash-playback-') as directory:
    folder=Path(directory);unit=folder/'playback.cpp';unit.write_text(pre+body+main)
    binary=folder/'playback'
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-fno-pie','-no-pie',str(unit),'-o',str(binary)],check=True,timeout=60)
    subprocess.run([str(binary)],check=True,timeout=20)
