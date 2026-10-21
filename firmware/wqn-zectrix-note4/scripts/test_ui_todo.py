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
    parser.add_argument('--cloud-source', type=Path, default=root/'main/ui/todo_cloud.cpp')
    args = parser.parse_args()
    model = (root/'main/ui_model.h').read_text()
    app = args.cloud_source.read_text()
    internal = (root/'main/ui/ui_internal.h').read_text()
    source = (root/'main/ui_model.cpp').read_text()
    display = (root/'components/display_service/display_service.cpp').read_text()
    digits = (root/'main/ui/seven_segment.cpp').read_text()
    page = (root/'main/ui/page_todo.cpp').read_text()
    refresh = (root/'main/ui/ui_refresh.cpp').read_text()
    pieces = {
        '@@CLOCK_POLICY@@': section((root/'main/ui/clock.cpp').read_text(), 'bool ScreenUsesClockMinute(', '\n}  // namespace device_ui_internal'),
        '@@MINUTE@@': section((root/'main/ui/ui_runtime.cpp').read_text(), 'UiUpdate UiRuntime::DispatchClockMinute(', 'UiUpdate UiRuntime::DispatchStatusEditTimeout('),
        '@@FONT_LOOKUP@@': section(display, 'const lv_font_fmt_txt_glyph_dsc_t* FindGlyphInFont(', 'int MeasureCodepointWidth('),
        '@@FONT_DRAW@@': section(display, 'void DrawGlyphFromFont(', 'void DrawCjkGlyph('),
        '@@FONT_MEASURE@@': section(display, 'int MeasureTextWithFont(', 'std::string TruncateUtf8TextToWidth('),
        '@@BATTERY@@': section(digits, 'const char* BatteryIconGlyph(', '\n}  // namespace\n'),
        '@@ICONS@@': section(digits, 'int DrawWifiStatusIcon(', '\n}  // namespace device_ui_internal'),
        '@@PAGE@@': re.sub(r'^#include[^\n]*\n', '', page, flags=re.M),
        '@@SIGNATURE@@': section(refresh, '    if (frame.screen == wqn::UiScreen::kTodo) {', '    if (frame.screen == wqn::UiScreen::kWord) {'),
        '@@ITEM@@': section((root/'main/wqn_api.h').read_text(), 'struct WqnTodoItem {', 'struct WqnWordEntry {'),
        '@@MODEL@@': section(model, 'enum class TodoSyncStatus {', 'struct HomeSummary {'),
        '@@SCHEDULE@@': section(internal, 'enum class RefreshSchedule {', '\nint RefreshRank('),
        '@@RESULT@@': section(internal, 'enum class TodoCloudOp {', 'enum class WordCloudOp {'),
        '@@CLAMP@@': section(source, '    if (state->todo.todos.empty()) {', '    if (state->settings.selected >= kSettingsItemCount) {'),
        '@@EXECUTE@@': function(app, 'void ExecuteTodoCloudRequest('),
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
        (folder/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0;\n')
        (folder/'lvgl.h').write_text('#include "font_fmt.h"\n')
        inc = root/'components/display_service/include'
        objects = []
        for name, font in [('primary', root/'components/display_service/fonts/SourceHanSansSC_Regular_slim.c'),
                           ('native', root/'main/ui/ui_fonts.c'),
                           ('icons', root/'components/display_service/fonts/font_zectrix_16_1.c')]:
            obj = folder/(name+'.o')
            subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(folder), '-I', str(inc),
                            '-I', str(root/'main/ui'), '-c', str(font), '-o', str(obj)], check=True, timeout=60)
            objects.append(str(obj))
        unit, binary = folder/'todo.cpp', folder/'todo'
        unit.write_text(fixture)
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', *flags,
                        '-I', str(folder), '-I', str(inc), '-I', str(root/'main'), '-I', str(root/'main/ui'), str(unit), *objects, '-o', str(binary)], check=True, timeout=60)
        command = [str(binary)]
        if args.render_dir:
            args.render_dir.mkdir(parents=True, exist_ok=True)
            command.append(str(args.render_dir.resolve()))
        subprocess.run(command, check=True, timeout=45)


if __name__ == '__main__':
    main()
