#!/usr/bin/env python3
"""Calibrated learning admission and exact production timestamp/commit pumps.

Input prefix ends at the reducer dispatch boundary (a counting port), and
storage/leases are host ports. No network, device retention or physical HIL.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function


def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mutation',choices=('allow-build','skip-input','skip-epoch','fabricate-timestamp','hold-old-buffer','skip-note-start','hold-retry'))
    a=p.parse_args()
    clock=(root/'main/ui/clock.cpp').read_text()
    model=(root/'main/ui_model.cpp').read_text()
    word=(root/'main/ui/word_cloud.cpp').read_text()
    note=(root/'main/ui/note_cloud.cpp').read_text()
    problem=(root/'main/ui/problem_cloud.cpp').read_text()
    if a.mutation=='hold-retry':
        old='? CurrentIsoTimestamp() : pending.occurred_at;'
        assert word.count(old)==note.count(old)==problem.count(old)==1
        word=word.replace(old,'? CurrentIsoTimestamp() : CurrentIsoTimestamp();')
        note=note.replace(old,'? CurrentIsoTimestamp() : CurrentIsoTimestamp();')
        problem=problem.replace(old,'? CurrentIsoTimestamp() : CurrentIsoTimestamp();')
    if a.mutation=='skip-note-start':
        old='!wqn::runtime::IsLearningTimeReady() ||';assert note.count(old)==1;note=note.replace(old,'false ||')
    if a.mutation=='skip-input':
        old='!runtime::IsLearningTimeReady())';assert model.count(old)==1;model=model.replace(old,'false)')
    if a.mutation=='skip-epoch':
        old='if (!after.network_calibrated() || after.epoch != clock.epoch) return "";';assert clock.count(old)==1;clock=clock.replace(old,'(void)after;')
    if a.mutation=='fabricate-timestamp':
        old='if (!wqn::runtime::IsCalibratedLearningTime(clock, now)) {\n        return "";';assert clock.count(old)==1;clock=clock.replace(old,'if (!wqn::runtime::IsCalibratedLearningTime(clock, now)) {\n        return "2024-01-01T00:00:00Z";')
    if a.mutation=='hold-old-buffer':
        old='refresh = runtime->DispatchLearningTimeRequired().refresh;';assert word.count(old)==1;word=word.replace(old,'return runtime->DispatchLearningTimeRequired().refresh;')
    start=model.index('void HandleUiInput(');end=model.index('    ClampUiSelection(state);',start)
    prefix=model[start:end]+'    ++dispatches; // downstream reducer dispatch port\n}\n'
    header=(root/'main/runtime/wall_clock.h').read_text()
    if a.mutation=='allow-build':
        old='return source == WallClockSource::kSntp || source == WallClockSource::kRtcSntp;'
        assert header.count(old)==1
        header=header.replace(old,'return source == WallClockSource::kBuildTime || source == WallClockSource::kSntp || source == WallClockSource::kRtcSntp;')
    model_header=(root/'main/ui_model.h').read_text();start=model_header.index('enum class UiInput {');end=model_header.index('\n};',start)+3
    runtime=(root/'main/ui/ui_runtime.cpp').read_text()
    parts={'@@UI_INPUT_ENUM@@':model_header[start:end],'@@INPUT_PREFIX@@':prefix,
        '@@NOTE_START@@':function(note,'bool QueueNoteSessionStart('),
        '@@CLOCK@@':'\n'.join(function(clock,n) for n in ['std::time_t CurrentUnixTime(', 'std::string CurrentDateLabel(', 'std::string CurrentClockLabel(', 'std::string CurrentIsoTimestamp(']),
        '@@NOTICE@@':function(runtime,'UiUpdate UiRuntime::DispatchLearningTimeRequired('),
        '@@PUMPS@@':function(word,'RefreshSchedule PumpWordObservationCommit(')+'\n'+function(note,'RefreshSchedule PumpNoteObservationCommit(')+'\n'+function(problem,'RefreshSchedule PumpProblemVerdictCommit(')}
    fixture=(root/'scripts/testdata/learning_time.cpp').read_text()
    for k,v in parts.items():assert fixture.count(k)==1,k;fixture=fixture.replace(k,v)
    with tempfile.TemporaryDirectory(prefix='note4-learning-time-') as t:
        d=Path(t);(d/'runtime').mkdir();(d/'runtime/wall_clock.h').write_text(header)
        (d/'esp_timer.h').write_text('#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n')
        (d/'test.cpp').write_text(fixture)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie',
            '-I'+str(d),'-I'+str(root/'main'),str(d/'test.cpp'),str(root/'main/runtime/wall_clock.cpp'),'-Wl,--wrap=time,--wrap=gmtime_r','-o',str(d/'test')],check=True,timeout=60)
        return subprocess.run([str(d/'test')],timeout=30).returncode


if __name__=='__main__':raise SystemExit(main())
