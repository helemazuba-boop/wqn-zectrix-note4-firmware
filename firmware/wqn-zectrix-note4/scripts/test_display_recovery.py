#!/usr/bin/env python3
"""Replay the actual EPD owner, submission/ACK and UI recovery gates on host.

RTOS/time/rendering are ports; no physical panel or power/sleep acceptance.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from types import SimpleNamespace

from test_audio_runtime import function


def structure(source,name):
    start=source.index(name)
    return source[start:source.index('\n};',start)+3]


def production_fixture(root, before=None, mutation=None):
    """Share production extraction with the separate bounded-result proof."""
    args=SimpleNamespace(before=before, mutation=mutation)
    def source(name):
        if args.before:
            return subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/{name}'],cwd=root,text=True)
        return (root/name).read_text()
    refresh=source('main/ui/ui_refresh.cpp'); device=source('main/device_ui.cpp')
    runtime=source('main/ui/ui_runtime.cpp'); header=source('main/ui/ui_runtime.h')
    internal=source('main/ui/ui_internal.h'); model=source('main/ui_model.cpp')
    policy=(root/'main/ui/display_recovery.h').read_text()
    has_recovery='DisplayRecoveryPolicy g_display_recovery;' in refresh
    if has_recovery:
        for call in ('MaybeRecoverDisplayForButton(event, &ui_runtime, &display_tracking, &refresh_schedule);',
                     'ApplyDisplayRecoveryGate(&ui_runtime, &display_tracking, &refresh_schedule);',
                     'poll_delay = DisplayRecoveryPollDelay(display_tracking, poll_delay);'):
            if device.count(call)!=1: raise SystemExit('FAIL: recovery UI callsite drift')
    if args.mutation:
        changes={
            'no-limit':('if (state_.consecutive_failures <= kAutomaticRetries)','if (true)'),
            'no-backoff':('now_ms + kRetryDelaysMs[state_.consecutive_failures - 1]','now_ms'),
            'bypass-owner':('g_display_recovery.TryBegin(esp_timer_get_time() / 1000);','(g_display_recovery.TryBegin(esp_timer_get_time() / 1000) || true);'),
            'bypass-request':('if (!g_display_recovery.status().CanQueue(esp_timer_get_time() / 1000))',
                              'if (!g_display_recovery.status().CanQueue(esp_timer_get_time() / 1000) && false)'),
            'bypass-ui':('if (!device_ui_internal::CopyDisplayRecoveryStatus().CanQueue(esp_timer_get_time() / 1000))',
                         'if (!device_ui_internal::CopyDisplayRecoveryStatus().CanQueue(esp_timer_get_time() / 1000) && false)'),
            'reset-on-probe':('        state_.probe_pending = true;','        state_ = {}; state_.probe_pending = true;'),
            'stale-probe':('input_at_ms <= state_.paused_at_ms ||','false ||'),
        }
        old,new=changes[args.mutation]
        target=refresh if args.mutation in ('bypass-owner','bypass-request') else device if args.mutation=='bypass-ui' else policy
        if target.count(old)!=1: raise SystemExit('FAIL: recovery mutation boundary drift')
        target=target.replace(old,new)
        if args.mutation=='no-limit': target=target.replace('kRetryDelaysMs[state_.consecutive_failures - 1]',
                                                          'kRetryDelaysMs[(state_.consecutive_failures - 1) % kAutomaticRetries]')
        if args.mutation in ('bypass-owner','bypass-request'): refresh=target
        elif args.mutation=='bypass-ui': device=target
        else: policy=target
    methods=['uint64_t UiRuntime::NextRevision(', 'UiUpdate UiRuntime::FinishEvent(', 'UiUpdate UiRuntime::DispatchDisplayResult(']
    runtime_bodies='\n'.join(function(runtime,s) for s in methods)
    if has_recovery: runtime_bodies+='\n'+function(runtime,'UiUpdate UiRuntime::DispatchDisplayRecovery(')
    else: runtime_bodies+='\nUiUpdate UiRuntime::DispatchDisplayRecovery() { return FinishEvent(AppEventKind::kDisplayResult,RefreshSchedule::kImmediate,false,true); }'
    helpers=['void MaybeReleaseDisplayLeaseLocked(', 'bool HasOutstandingRevisionLocked(',
        'bool TrackOutstandingRevisionLocked(', 'bool UntrackOutstandingRevisionLocked(', 'uint32_t RefreshReasonMask(',
        'constexpr WaveformRequirement StrongerWaveform(', 'constexpr TickType_t EarlierDeadline(',
        'constexpr bool HasMultipleReasons(', 'bool PublishDisplayResult(', 'DisplayResult SupersededResult(',
        'DisplayIntent NewDisplayIntent(', 'void MergeDisplayPolicy(', 'RefreshSchedule EffectiveSchedule(',
        'wqn::display::DisplaySubmission RequestEpdUiRefresh(', 'void AcknowledgeDisplayResult(']
    lower='\n'.join(function(refresh,s) for s in helpers)
    if has_recovery:
        lower+='\n'+function(refresh,'DisplayRecoveryStatus CopyDisplayRecoveryStatus(')+'\n'+function(refresh,'bool RequestDisplayRecovery(')
        gate='\n'.join(function(device,s) for s in ('void MaybeRecoverDisplayForButton(', 'void ApplyDisplayRecoveryGate(', 'TickType_t DisplayRecoveryPollDelay('))
    else:
        lower+='\nDisplayRecoveryStatus CopyDisplayRecoveryStatus() { return {}; }\nbool RequestDisplayRecovery(int64_t) { return false; }'
        gate='void MaybeRecoverDisplayForButton(const wqn::ButtonEvent&,UiRuntime*,DisplayTrackingState*,RefreshSchedule*) {}\nvoid ApplyDisplayRecoveryGate(UiRuntime*,DisplayTrackingState*,RefreshSchedule*) {}\nTickType_t DisplayRecoveryPollDelay(const DisplayTrackingState&,TickType_t fallback) { return fallback; }'
    pieces={
        '@@SCHEDULE@@':structure(internal,'enum class RefreshSchedule {'),
        '@@DELAYS@@':'\n'.join(re.search(r'constexpr TickType_t '+name+r' = [^;]+;',internal).group() for name in (
            'kCommitRefreshDelay','kClockRefreshDelay','kTimerRefreshDelay','kSelectionRefreshDelay','kConfigRefreshDelay','kAiRefreshDelay')),
        '@@EVENT@@':structure(header,'enum class AppEventKind :'), '@@UPDATE@@':structure(header,'struct UiUpdate {'),
        '@@RUNTIME@@':runtime_bodies, '@@SECONDARY@@':structure(internal,'struct SecondarySlot {'),
        '@@REFRESH_HELPERS@@':'\n'.join(function(refresh,s) for s in ('int RefreshRank(', 'const char* RefreshScheduleName(',
            'TickType_t RefreshDelay(', 'bool TickReached(', 'TickType_t TicksUntil(', 'RefreshSchedule StrongerSchedule(')),
        '@@LOWER@@':lower,
        '@@WAIT@@':structure(refresh,'struct EpdIdleSchedule {')+'\n'+function(refresh,'TickType_t EpdIdleWaitTicks(')+'\n'+function(refresh,'void WaitForEpdFrameOrIdleMaintenance('),
        '@@OWNER@@':function(refresh,'void EpdRefreshTask('),
        '@@TRACKING_TYPES@@':structure(device,'struct SubmittedDisplayRecord {')+'\n'+structure(device,'struct DisplayTrackingState {'),
        '@@UPPER@@':'\n'.join(function(device,s) for s in ('SubmittedDisplayRecord* FindSubmittedRecord(',
            'void RecomputeLatestSubmission(', 'bool TrackDisplaySubmission(', 'const char* DisplayStatusName(', 'void DrainDisplayResults(')),
        '@@GATE@@':function(device,'TickType_t DelayMsToTicks(')+'\n'+gate,
        '@@WAVEFORM@@':function(device,'wqn::display::WaveformRequirement RequestedWaveform('),
        '@@GLOBAL_FORCE@@':function(model,'void RequestForceFullRefresh()')+'\n'+function(model,'bool ConsumeForceFullRefresh()'),
    }
    fixture=(root/'scripts/testdata/display_recovery.cpp').read_text()
    for marker,body in pieces.items():
        if fixture.count(marker)!=1: raise SystemExit('FAIL: recovery fixture drift: '+marker)
        fixture=fixture.replace(marker,body)
    return fixture, policy, has_recovery


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before')
    parser.add_argument('--mutation',choices=('no-limit','no-backoff','bypass-owner','bypass-request',
                                             'bypass-ui','reset-on-probe','stale-probe'))
    args=parser.parse_args()
    fixture, policy, has_recovery = production_fixture(root, args.before, args.mutation)
    with tempfile.TemporaryDirectory(prefix='wqn-display-recovery-') as directory:
        tmp=Path(directory); (tmp/'display_recovery.h').write_text(policy)
        (tmp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=259;\n')
        (tmp/'test.cpp').write_text(fixture)
        exit_code=0
        for rate in (100,1000):
            binary=tmp/f'test-{rate}'
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                f'-DconfigTICK_RATE_HZ={rate}',f'-DHAS_RECOVERY={int(has_recovery)}',
                '-I'+str(tmp),'-I'+str(root/'main'),'-I'+str(root/'components/display_service/include'),
                str(tmp/'test.cpp'),'-o',str(binary)],check=True,timeout=60)
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
            result=subprocess.run([str(binary)],env=env,timeout=30)
            if result.returncode: exit_code=result.returncode
    return exit_code


if __name__=='__main__': raise SystemExit(main())
