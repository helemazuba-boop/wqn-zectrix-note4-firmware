#!/usr/bin/env python3
"""Trace production AI force-full delivery through the final display dispatch.

Rendering, HTTP/RTOS/panel I/O are ports. Reducer, update, tracking, throttle,
deduplication and final request call are compiled from production source.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_audio_runtime import function


def structure(source, name):
    first = source.index(name)
    return source[first:source.index('\n};', first)+3]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', help='read production bodies from a git revision')
    parser.add_argument('--mutation', choices=('drop-update-force', 'drop-owner-force',
                                              'overwrite-pending-force', 'clear-throttled-force',
                                              'clear-rejected-force', 'ignore-dispatch-force'))
    args = parser.parse_args()

    def source(name):
        if args.before:
            return subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/{name}'],
                                           cwd=root,text=True)
        return (root/name).read_text()

    runtime = source('main/ui/ui_runtime.cpp')
    header = source('main/ui/ui_runtime.h')
    device = source('main/device_ui.cpp')
    ai = source('main/ai_session.cpp').split('\n#else')[0]
    model = source('main/ui_model.cpp')
    schedule = source('main/ui/ui_internal.h')
    refresh = source('main/ui/ui_refresh.cpp')
    dispatch = function(runtime,'UiUpdate UiRuntime::DispatchAiStreamingSnapshot(')
    first = device.index('        if (streaming_changed) {')
    owner = device[first:device.index('\n#if CONFIG_WQN_AI_ENABLE',first)]
    first = device.index('            // [ui-stack] As above, the frame lives')
    first = device.index('            [&]() __attribute__((noinline)) {',first)
    end = device.index('\n            LogUiStackHighWater("frame-dispatched");',first)
    render = device[first:end]
    if args.mutation:
        changes = {
            'drop-update-force': ('update.force_full_refresh = force_full;', 'update.force_full_refresh = false;'),
            'drop-owner-force': ('display_tracking.force_next_submission |= update.force_full_refresh;', '(void)update.force_full_refresh;'),
            'overwrite-pending-force': ('display_tracking.force_next_submission |= update.force_full_refresh;',
                                        'display_tracking.force_next_submission = update.force_full_refresh;'),
            'clear-throttled-force': ('display_tracking.force_next_submission = force_submission;',
                                      'display_tracking.force_next_submission = false;'),
            'clear-rejected-force': ('display_tracking.force_next_submission = force_submission;',
                                     'display_tracking.force_next_submission = false;'),
            'ignore-dispatch-force': ('consumed_force_full || display_tracking.force_next_submission ||',
                                      'consumed_force_full || false ||'),
        }
        old,new = changes[args.mutation]
        target = dispatch if args.mutation=='drop-update-force' else owner if args.mutation in (
            'drop-owner-force','overwrite-pending-force') else render
        if args.mutation in ('clear-throttled-force','clear-rejected-force'):
            if target.count(old)!=2: raise SystemExit('FAIL: force retry mutation boundary drift')
            which=0 if args.mutation=='clear-throttled-force' else 1
            locations=[i for i in range(len(target)) if target.startswith(old,i)]
            at=locations[which]
            target=target[:at]+new+target[at+len(old):]
        else:
            if target.count(old)!=1: raise SystemExit('FAIL: force mutation boundary drift')
            target=target.replace(old,new)
        if args.mutation=='drop-update-force': dispatch=target
        elif args.mutation in ('drop-owner-force','overwrite-pending-force'): owner=target
        else: render=target
    fixture=(root/'scripts/testdata/ai_force_full.cpp').read_text()
    replacements={
        '@@SCHEDULE@@':structure(schedule,'enum class RefreshSchedule {'),
        '@@EVENT@@':structure(header,'enum class AppEventKind :'),
        '@@UPDATE@@':structure(header,'struct UiUpdate {'),
        '@@VIEW@@':structure(source('main/ai_session.h'),'struct AiStreamingStatusView {'),
        '@@RUNTIME@@':'\n'.join([function(runtime,'uint64_t UiRuntime::NextRevision('),
            function(runtime,'UiUpdate UiRuntime::FinishEvent('),dispatch,
            function(runtime,'UiUpdate UiRuntime::DispatchDisplayResult(')]),
        '@@SCHEDULE_HELPERS@@':'\n'.join(function(refresh,s) for s in (
            'int RefreshRank(', 'const char* RefreshScheduleName(', 'RefreshSchedule StrongerSchedule(')),
        '@@PRODUCER@@':function(ai,'bool CopyAiStreamingStatus('),
        '@@GLOBAL_FORCE@@':function(model,'void RequestForceFullRefresh()')+'\n'+function(model,'bool ConsumeForceFullRefresh()'),
        '@@TRACKING_TYPES@@':structure(device,'struct SubmittedDisplayRecord {')+'\n'+structure(device,'struct DisplayTrackingState {'),
        '@@TRACKING@@':'\n'.join(function(device,s) for s in (
            'SubmittedDisplayRecord* FindSubmittedRecord(', 'void RecomputeLatestSubmission(',
            'bool TrackDisplaySubmission(', 'const char* DisplayStatusName(',
            'void DrainDisplayResults(')),
        '@@WAVEFORM@@':function(device,'wqn::display::WaveformRequirement RequestedWaveform('),
        '@@OWNER@@':owner,'@@RENDER@@':render,
    }
    for marker,body in replacements.items():
        if fixture.count(marker)!=1: raise SystemExit('FAIL: force fixture boundary drift: '+marker)
        fixture=fixture.replace(marker,body)
    with tempfile.TemporaryDirectory(prefix='wqn-force-full-') as directory:
        tmp=Path(directory)
        (tmp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0,ESP_FAIL=-1;\n')
        (tmp/'test.cpp').write_text(fixture)
        binary=tmp/'test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                        f'-DHAS_UPDATE_FORCE={int("bool force_full_refresh" in header)}',
                        '-I'+str(tmp),'-I'+str(root/'components/display_service/include'),
                        str(tmp/'test.cpp'),'-o',str(binary)],check=True,timeout=60)
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
        return subprocess.run([str(binary)],env=env,timeout=30).returncode


if __name__=='__main__':
    raise SystemExit(main())
