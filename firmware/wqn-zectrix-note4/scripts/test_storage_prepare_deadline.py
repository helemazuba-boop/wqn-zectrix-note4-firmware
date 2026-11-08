#!/usr/bin/env python3
"""Real storage service + sleep coordinator, with host RTOS/PM/VFS ports.

The storage owner is a real host thread. Checkpoint barriers control completion
ordering; virtual esp_timer/ticks bound waits without physical timing claims.
"""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

from test_audio_runtime import function


PORTS = {
    'esp_err.h': '''#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=257,ESP_ERR_INVALID_ARG=258;
constexpr int ESP_ERR_INVALID_STATE=259,ESP_ERR_NOT_FOUND=261,ESP_ERR_TIMEOUT=263;
inline const char* esp_err_to_name(int) { return "host"; }
''',
    'esp_log.h': '''#pragma once
void HostLog(const char*,const char*,...);
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
''',
    'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    'esp_pm.h': '''#pragma once
#include "esp_err.h"
struct HostPm;
using esp_pm_lock_handle_t=HostPm*;
constexpr int ESP_PM_NO_LIGHT_SLEEP=1,ESP_PM_CPU_FREQ_MAX=2;
int esp_pm_lock_create(int,int,const char*,esp_pm_lock_handle_t*);
int esp_pm_lock_acquire(esp_pm_lock_handle_t);
int esp_pm_lock_release(esp_pm_lock_handle_t);
''',
    'freertos/FreeRTOS.h': '''#pragma once
#include <cstdint>
#include <mutex>
using TickType_t=uint32_t;
using UBaseType_t=unsigned;
using portMUX_TYPE=std::mutex;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
constexpr int pdTRUE=1,pdFALSE=0,pdPASS=1;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>((ms)*configTICK_RATE_HZ/1000))
#define portMUX_INITIALIZER_UNLOCKED {}
#define taskENTER_CRITICAL(p) (p)->lock()
#define taskEXIT_CRITICAL(p) (p)->unlock()
''',
    'freertos/queue.h': '''#pragma once
#include "FreeRTOS.h"
#include <deque>
#include <vector>
struct StaticQueue_t {
    std::mutex mutex;
    size_t depth=0,item_size=0;
    std::deque<std::vector<uint8_t>> items;
};
using QueueHandle_t=StaticQueue_t*;
QueueHandle_t xQueueCreateStatic(unsigned,size_t,uint8_t*,StaticQueue_t*);
int xQueueSend(QueueHandle_t,const void*,TickType_t);
int xQueueReceive(QueueHandle_t,void*,TickType_t);
''',
    'freertos/semphr.h': '''#pragma once
#include "FreeRTOS.h"
#include <condition_variable>
struct StaticSemaphore_t { std::mutex mutex; std::condition_variable cv; bool ready=false; };
using SemaphoreHandle_t=StaticSemaphore_t*;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t*);
int xSemaphoreTake(SemaphoreHandle_t,TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
''',
    'freertos/task.h': '''#pragma once
#include "FreeRTOS.h"
using TaskHandle_t=void*;
int xTaskCreate(void (*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*);
TaskHandle_t xTaskGetCurrentTaskHandle();
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(int,TickType_t);
void vTaskDelay(TickType_t);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);
''',
}


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before')
    parser.add_argument('--tick-hz',type=int,choices=(100,1000),default=100)
    parser.add_argument('--mutation',choices=('double-budget','abandon-payload','skip-blocker',
        'wrong-generation','skip-expired-owner','publish-ready-late','skip-worker-reference','entry-only-deadline','control-lost-notify'))
    args=parser.parse_args()
    def source(name):
        if args.before:
            return subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/{name}'],cwd=root,text=True)
        return (root/name).read_text()
    service=source('main/services/storage_service.cpp')
    runtime=source('components/power_runtime/runtime/sleep_coordinator.cpp')
    word=source('main/word_study_store.cpp')
    storage=source('main/storage.cpp')
    power=source('main/power_manager.cpp')
    owned='ExecuteOwnedStorageTransactionUntil(' in service
    mutations={
        'double-budget':('service','const TickType_t observation_ticks = RemainingStorageDeadlineTicks(deadline_us);',
                         'const TickType_t observation_ticks = admission_ticks;'),
        'abandon-payload':('service','    ReleaseOwnedCompletion(completion);\n    return result;',
                            '    ReleaseOwnedCompletion(completion);\n    ReleaseOwnedCompletion(completion);\n    return result;'),
        'wrong-generation':('runtime','g_quiesce_generation.load(std::memory_order_acquire) == preparation_generation','true'),
        'skip-expired-owner':('service','completion->deadline_us > 0 && esp_timer_get_time() >= completion->deadline_us','false'),
        'publish-ready-late':('service','esp_timer_get_time() >= deadline_us ? ESP_ERR_TIMEOUT : completion->result','completion->result'),
        'skip-worker-reference':('service','if (owned) ReleaseOwnedCompletion(completion);','if (owned) { /* leak command reference */ }'),
    }
    if args.mutation and args.mutation != 'control-lost-notify':
        if args.mutation=='entry-only-deadline':
            needle='    if (deadline_reached()) return ESP_OK;'
            assert word.count(needle)==3
            first=word.index(needle)+len(needle)
            word=word[:first]+word[first:].replace(needle,'')
        elif args.mutation=='skip-blocker':
            old='    auto* request = new (std::nothrow) SleepOutboxMaintenanceRequest(std::move(lease), deadline_us);'
            assert word.count(old)==1
            word=word.replace(old,'    lease.Reset();\n'+old)
        else:
            target,old,new=mutations[args.mutation]
            code=service if target=='service' else runtime
            assert code.count(old)==1,'mutation boundary drift'
            code=code.replace(old,new)
            if target=='service': service=code
            else: runtime=code
    fixture=(root/'scripts/testdata/storage_prepare_deadline.cpp').read_text()
    if args.mutation == 'control-lost-notify':
        old='changed.wait_until(lock,std::min(deadline,now+1ms));'
        assert fixture.count(old)==1,'controller mutation boundary drift'
        fixture=fixture.replace(old,'changed.wait_until(lock,deadline);')
    start=word.index('struct OutboxMaintenanceContext {')
    context=word[start:word.index('\nesp_err_t OutboxMaintenanceStepTransaction(',start)]
    request=''
    if owned:
        start=word.index('struct SleepOutboxMaintenanceRequest {')
        request=word[start:word.index('\n}  // namespace',start)]
        # Test-only destructor observation; the actual production delete and
        # its lease/context destruction remain intact.
        needle='delete static_cast<SleepOutboxMaintenanceRequest*>(opaque);'
        assert request.count(needle)==1
        request=request.replace(needle,needle+' HostCleanup();')
    pieces={'@@CONTEXT@@':context,'@@STEP@@':function(word,'esp_err_t OutboxMaintenanceStepTransaction('),
        '@@DRIVER@@':function(word,'esp_err_t RunOutboxMaintenance(')+'\n'+request,
        '@@PREPARE@@':function(word,'esp_err_t PrepareWordObservationOutboxForSleep(')+'\n'+
                      function(storage,'esp_err_t PrepareStorageForSleep(')}
    start=power.index('constexpr int64_t kPrepareSleepBudgetUs[')
    pieces['@@POWER@@']=power[start:power.index('\n};',start)+3]+'\n'+'\n'.join(
        function(power,signature) for signature in ('constexpr int64_t PrepareSleepBudgetUs(',
        'static power::PrepareSleepResult MakePrepareResult(',
        'static power::PrepareSleepResults BroadcastPrepareSleep(', 'static bool AllServicesReady('))
    for marker,code in pieces.items():
        assert fixture.count(marker)==1,marker
        fixture=fixture.replace(marker,code)
    with tempfile.TemporaryDirectory(prefix='wqn-storage-deadline-') as directory:
        tmp=Path(directory)
        for name,code in PORTS.items():
            p=tmp/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(code)
        (tmp/'storage_service.cpp').write_text(service)
        (tmp/'sleep_coordinator.cpp').write_text(runtime)
        (tmp/'test.cpp').write_text(fixture)
        binary=tmp/'test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie',
            '-DCONFIG_PM_ENABLE=1',f'-DconfigTICK_RATE_HZ={args.tick_hz}',f'-DHAS_OWNED={int(owned)}',
            '-I'+str(tmp),'-I'+str(root/'main'),'-I'+str(root/'components/power_runtime/include'),
            str(tmp/'storage_service.cpp'),str(tmp/'sleep_coordinator.cpp'),
            str(root/'components/power_runtime/power/sleep_protocol.cpp'),str(tmp/'test.cpp'),'-o',str(binary)],
            check=True,timeout=60)
        cases={'double-budget':'queue','abandon-payload':'running','skip-blocker':'running',
               'wrong-generation':'lease','skip-expired-owner':'expired','publish-ready-late':'late',
               'skip-worker-reference':'publication','entry-only-deadline':'cache','control-lost-notify':'control-wakeup'}
        case=cases[args.mutation] if args.mutation else ('running' if args.before else 'all')
        return subprocess.run([str(binary),case],timeout=30,env=dict(os.environ,
            ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')).returncode


if __name__=='__main__':
    raise SystemExit(main())
