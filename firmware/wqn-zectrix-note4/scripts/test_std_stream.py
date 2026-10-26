#!/usr/bin/env python3
"""Replay the production STD SSE consumer with real history and sanitizers.

RTOS scheduling, TLS resources and physical EPD presentation remain device HIL.
"""
from pathlib import Path
import hashlib
import subprocess
import tempfile
from test_audio_runtime import function

PORTS = {
    'freertos/FreeRTOS.h': '''#pragma once
#include <cstdint>
#include <mutex>
struct StaticSemaphore_t { std::mutex mutex; };
using SemaphoreHandle_t=std::mutex*;
constexpr int portMAX_DELAY=-1;
''',
    'freertos/semphr.h': '''#pragma once
#include "FreeRTOS.h"
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* p) { return &p->mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t p,int) { p->lock();return 1; }
inline void xSemaphoreGive(SemaphoreHandle_t p) { p->unlock(); }
''',
    'esp_err.h': '''#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NO_MEM=0x101;
''',
    'esp_log.h': '''#pragma once
void HostLog(const char*,const char*,...) __attribute__((format(printf,2,3)));
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGD(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
''',
    'esp_heap_caps.h': '''#pragma once
#include <algorithm>
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_INTERNAL=2,MALLOC_CAP_8BIT=4;
inline size_t heap_caps_get_total_size(int) { return 1; }
inline void* heap_caps_aligned_alloc(size_t alignment,size_t bytes,int) {
    alignment=std::max(alignment,sizeof(void*));
    return std::aligned_alloc(alignment,(bytes+alignment-1)/alignment*alignment);
}
inline void heap_caps_free(void* p) { std::free(p); }
''',
}


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root/'main/ai_session.cpp').read_text()
    source = source[:source.rindex('\n#else')]
    model = (root/'main/ui_model.h').read_text()
    model = model[model.index('enum class AiSessionStatus {'):model.index('enum class TodoSyncStatus {')]
    turn = source[source.index('struct StdProTurnAssembly {'):source.index('StdProTurnAssembly g_turn;')+len('StdProTurnAssembly g_turn;')]
    names = ['std::string ThinkingLabel(', 'void ResetTurnAssemblyLocked()',
             'bool EnsureThinkingHistoryLocked(', 'bool FinalizeThinkingLocked(',
             'bool FinalizeAssistantLocked(', 'void SealAssistantSegmentLocked(',
             'std::string ResolveSegmentTextLocked(', 'void MarkChanged()',
             'void ReceiveInteractionLocked(', 'void OnSseEvent(']
    fixture = (root/'scripts/testdata/std_stream.cpp').read_text()
    fixture = fixture.replace('@@MODEL@@',model).replace('@@TURN@@',turn)
    fixture = fixture.replace('@@CONSUMER@@','\n'.join(function(source,name) for name in names))
    flags=[]
    for domain,schema in [('DEVICE_CONTROL','device-control-v3'),('NOTE_STUDY','note-study-v1'),('PROBLEM_STUDY','problem-study-v1'),('WORD_STUDY','word-study-v1')]:
        path=next((root/'contracts').rglob(schema+'.schema.json'))
        flags.append(f'-DWQN_{domain}_SCHEMA_SHA256="{hashlib.sha256(path.read_bytes()).hexdigest()}"')
    with tempfile.TemporaryDirectory(prefix='wqn-std-stream-') as temp:
        folder=Path(temp)
        for name,text in PORTS.items():
            path=folder/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
        unit=folder/'stream.cpp';unit.write_text(fixture)
        binary=folder/'stream'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer',*flags,
                        '-I'+str(folder),'-I'+str(root/'main'),
                        '-I'+str(root/'components/device_protocol/include'),
                        str(unit),str(root/'main/ai_history.cpp'),'-pthread','-o',str(binary)],check=True,timeout=60)
        subprocess.run([str(binary)],check=True,timeout=30)


if __name__ == '__main__':
    main()
