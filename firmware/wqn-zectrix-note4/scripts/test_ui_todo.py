#!/usr/bin/env python3
"""Replay production Todo input, cloud-result and native-render seams.

Network, clock and display ports are host substitutes, not panel or cloud HIL.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_audio_runtime import function


def section(source, start, end):
    assert source.count(start) == 1, start
    first = source.index(start)
    return source[first:source.index(end, first)]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--render-dir', type=Path)
    args = parser.parse_args()
    model = (root/'main/ui_model.h').read_text()
    app = (root/'main/ui/todo_cloud.cpp').read_text()
    internal = (root/'main/ui/ui_internal.h').read_text()
    source = (root/'main/ui_model.cpp').read_text()
    pieces = {
        '@@ITEM@@': section((root/'main/wqn_api.h').read_text(), 'struct WqnTodoItem {', 'struct WqnWordEntry {'),
        '@@MODEL@@': section(model, 'enum class TodoSyncStatus {', 'struct HomeSummary {'),
        '@@SCHEDULE@@': section(internal, 'enum class RefreshSchedule {', '\nint RefreshRank('),
        '@@RESULT@@': section(internal, 'enum class TodoCloudOp {', 'enum class WordCloudOp {'),
        '@@CLAMP@@': section(source, '    if (state->todo.todos.empty()) {', '    if (state->settings.selected >= kSettingsItemCount) {'),
        '@@CLOUD@@': '\n'.join(function(app, start) for start in [
            'bool QueueTodoRefresh()', 'bool QueueTodoRefreshCursor(', 'bool QueueTodoComplete(',
            'void ApplyTodoList(', 'bool TodoPageMatchesState(', 'bool ApplyTodoCloudResult(',
            'bool RefreshTodosFromCloud(', 'RefreshSchedule CompleteSelectedTodo(', 'RefreshSchedule HandleTodoInput(']),
    }
    fixture = (root/'scripts/testdata/ui_todo.cpp').read_text()
    for marker, body in pieces.items():
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-todo-host-') as temp:
        folder = Path(temp)
        unit, binary = folder/'todo.cpp', folder/'todo'
        unit.write_text(fixture)
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', *flags,
                        '-I', str(root/'main'), str(unit), '-o', str(binary)], check=True, timeout=60)
        command = [str(binary)]
        if args.render_dir:
            args.render_dir.mkdir(parents=True, exist_ok=True)
            command.append(str(args.render_dir.resolve()))
        subprocess.run(command, check=True, timeout=45)


if __name__ == '__main__':
    main()
