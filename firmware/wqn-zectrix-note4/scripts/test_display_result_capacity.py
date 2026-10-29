#!/usr/bin/env python3
"""R-04: bounded queue + real submission, owner, UI drain and ACK functions.

No forced send failure in the positive fixture. The queue rejects only when
full and the UI makes no progress during that hypothetical send wait. This is
a host contract check, not a proof against memory corruption or device HIL.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_audio_runtime import function
from test_display_recovery import production_fixture


QUEUE_PORT = r'''
size_t send_full=0, duplicate_terminals=0, unreserved_terminals=0, max_queue=0;
size_t actual_queue_capacity=0, actual_queue_item_size=0;
std::function<void()> before_send, after_receive, before_submission_return;
int xQueueSend(void*,const DisplayResult* result,TickType_t) {
    if (before_send) { auto hook=std::move(before_send);before_send={};hook(); }
    if (results.size()>=actual_queue_capacity) {++send_full;return pdFALSE;}
    if (std::find(g_outstanding_display_revisions.begin(),g_outstanding_display_revisions.end(),result->revision)
        ==g_outstanding_display_revisions.end()) ++unreserved_terminals;
    if (std::any_of(all_results.begin(),all_results.end(),[&](const auto& old){return old.revision==result->revision;}))
        ++duplicate_terminals;
    results.push_back(*result);all_results.push_back(*result);
    max_queue=std::max(max_queue,results.size());return pdTRUE;
}
int xQueueReceive(void*,DisplayResult* result,int) {
    if(results.empty())return pdFALSE;
    *result=results.front();results.pop_front();
    if (after_receive) {auto hook=std::move(after_receive);after_receive={};hook();}
    return pdTRUE;
}
void xTaskNotifyGive(void*) {}
void taskYIELD() {
    if (before_submission_return) {
        auto hook=std::move(before_submission_return);before_submission_return={};hook();
    }
}
'''

STARTUP_PORT = r'''
using BaseType_t=int;
constexpr int pdPASS=1;
void* xSemaphoreCreateMutex() {return reinterpret_cast<void*>(1);}
void* xQueueCreate(unsigned capacity,size_t item_size) {
    device_ui_internal::actual_queue_capacity=capacity;
    device_ui_internal::actual_queue_item_size=item_size;
    return reinterpret_cast<void*>(3);
}
int xTaskCreate(void (*)(void*),const char*,unsigned,void*,unsigned,void** handle) {
    *handle=reinterpret_cast<void*>(2);return pdPASS;
}
'''


def count_model(capacity):
    """All valid counts: L=queued+received-not-ACKed+not-yet-published."""
    states = transitions = 0
    for live in range(capacity + 1):
        for queued in range(live + 1):
            for received in range(live - queued + 1):
                pending = live - queued - received
                states += 1
                moves = []
                if live < capacity:  # admission reserves before work
                    moves.append((live+1, queued, received, pending+1))
                if pending:  # exactly one terminal uses its reservation
                    assert queued < capacity
                    moves.append((live, queued+1, received, pending-1))
                if queued:
                    moves.append((live, queued-1, received+1, pending))
                if received:  # ACK can only retire a received terminal
                    moves.append((live-1, queued, received-1, pending))
                for l, q, d, p in moves:
                    assert 0 <= l <= capacity and min(q, d, p) >= 0 and q+d+p == l
                transitions += len(moves)
    return dict(capacity=capacity, valid_count_states=states, transitions=transitions,
                scope='Count abstraction under unique terminals, reserved admission and receive-before-ACK; not exhaustive firmware scheduling')


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation',choices=('bypass-admission','early-ack','duplicate-terminal','short-queue'))
    parser.add_argument('--model-json',type=Path)
    args=parser.parse_args()
    fixture,policy,_=production_fixture(root)
    fixture=fixture[:fixture.index('void FailedCycle(')]
    refresh=(root/'main/ui/ui_refresh.cpp').read_text()
    globals_start=refresh.index('uint32_t g_outstanding_display_intents = 0;')
    globals_end=refresh.index('// ---- Schedule helpers',globals_start)
    old_start=fixture.index('DisplayRevision g_last_presented_revision=0;')
    old_end=fixture.index('wqn::runtime::SleepLease g_display_sleep_lease;',old_start)
    fixture=fixture[:old_start]+refresh[globals_start:globals_end]+fixture[old_end:]
    start=fixture.index('int xQueueSend(')
    end=fixture.index('\nvoid MaybeReleaseDisplayLeaseLocked(',start)
    fixture=fixture[:start]+QUEUE_PORT+fixture[end:]
    pump=function(fixture,'void PumpUi(')
    fixture=fixture.replace(pump,'void PumpUi() {}')  # UI is explicitly scheduled by this driver.
    if args.mutation=='bypass-admission':
        old='if (g_outstanding_display_intents >= wqn::display::kDisplayResultQueueDepth)'
        assert fixture.count(old)==1
        fixture=fixture.replace(old,'if (false)')
    elif args.mutation in ('early-ack','duplicate-terminal'):
        old=function(fixture,'bool PublishDisplayResult(')
        new=old.replace('    return true;',
            '    UntrackOutstandingRevisionLocked(result.revision);\n    return true;'
            if args.mutation=='early-ack' else
            '    xQueueSend(g_display_result_queue, &result, timeout);\n    return true;')
        assert old!=new
        fixture=fixture.replace(old,new)
    startup=function((root/'main/device_ui.cpp').read_text(),'esp_err_t EnsureDisplayPipelineStarted(')
    if args.mutation=='short-queue':
        old='xQueueCreate(wqn::display::kDisplayResultQueueDepth,'
        assert startup.count(old)==1
        startup=startup.replace(old,'xQueueCreate(wqn::display::kDisplayResultQueueDepth - 1,')
    fixture+=STARTUP_PORT+'\n'+startup+'\n'+(root/'scripts/testdata/display_result_capacity.cpp').read_text()
    types=(root/'components/display_service/include/display/display_types.h').read_text()
    cap=int(re.search(r'kDisplayResultQueueDepth = (\d+);',types).group(1))
    model=count_model(cap)
    if args.model_json: args.model_json.write_text(json.dumps(model,indent=2)+'\n')
    print('Count abstraction:',json.dumps(model),flush=True)
    with tempfile.TemporaryDirectory(prefix='wqn-display-capacity-') as directory:
        tmp=Path(directory)
        (tmp/'display_recovery.h').write_text(policy)
        (tmp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=257,ESP_ERR_INVALID_STATE=259;\n')
        (tmp/'test.cpp').write_text(fixture)
        code=0
        for hz in (100,1000):
            binary=tmp/f'test-{hz}'
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                '-fno-pie','-no-pie',f'-DconfigTICK_RATE_HZ={hz}','-DHAS_RECOVERY=1',
                '-I'+str(tmp),'-I'+str(root/'main'),'-I'+str(root/'components/display_service/include'),
                str(tmp/'test.cpp'),'-o',str(binary)],check=True,timeout=60)
            done=subprocess.run([str(binary)],timeout=60,env=dict(os.environ,
                ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
            if done.returncode:code=done.returncode
    return code


if __name__=='__main__':
    raise SystemExit(main())
