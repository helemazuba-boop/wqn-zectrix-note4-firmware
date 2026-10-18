#!/usr/bin/env python3
"""Host production seams for Home A: idle policy, status, routes, query and pixels.

Panel, network, time and worker ownership primitives are controlled seams.
This does not establish RTOS concurrency, physical display timing or sleep HIL.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def part(source, start, end):
    assert source.count(start) == 1 and source.count(end) == 1, start
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render-dir', type=Path)
    parser.add_argument('--refresh-source', type=Path, default=root/'main/ui/ui_refresh.cpp')
    parser.add_argument('--model-source', type=Path, default=root/'main/ui/home_model.cpp')
    parser.add_argument('--worker-source', type=Path, default=root/'main/opencode_session.cpp')
    args = parser.parse_args()
    display = (root/'components/display_service/display_service.cpp').read_text()
    internal = (root/'main/ui/ui_internal.h').read_text()
    worker = args.worker_source.read_text().split('#else  // !CONFIG_WQN_AGENT_ENABLE')[0]
    state = (root/'main/ui/ui_state.cpp').read_text()
    runtime = (root/'main/ui/ui_runtime.cpp').read_text()
    button = part(runtime, 'UiUpdate UiRuntime::DispatchButton(', 'UiUpdate UiRuntime::DispatchTodoCloudResult(')
    inputs = (root/'main/ui/ui_input.cpp').read_text()
    page = (root/'main/ui/page_home.cpp').read_text()
    digits = (root/'main/ui/seven_segment.cpp').read_text()
    layout = (root/'main/ui/ui_layout.h').read_text()
    fixture = (root/'scripts/testdata/ui_home.cpp').read_text()
    refresh = args.refresh_source.read_text()
    pieces = {
        '@@SCHEDULE@@': part(internal, 'enum class RefreshSchedule {', '\nint RefreshRank('),
        '@@RANK@@': part(refresh, 'int RefreshRank(', 'const char* RefreshScheduleName('),
        '@@COMBINE@@': part(refresh, 'RefreshSchedule StrongerSchedule(', '\nnamespace {\n\nusing wqn::display::DisplayIntent;'),
        '@@RENDER@@': part((root/'main/ui/ui_render.cpp').read_text(), 'esp_err_t RenderFrameToEpd(', '\n}  // namespace device_ui_internal'),
        '@@FONT_LOOKUP@@': part(display, 'const lv_font_fmt_txt_glyph_dsc_t* FindGlyphInFont(', 'int MeasureCodepointWidth('),
        '@@FONT_DRAW@@': part(display, 'void DrawGlyphFromFont(', 'void DrawGlyphFromFontScaled('),
        '@@FONT_SIZE@@': part(display, 'void DrawTextWithFontSized(', 'const lv_font_t* PrimaryUiFont('),
        '@@FONT_MEASURE@@': part(display, 'int MeasureTextWithFont(', 'void DrawTextWithFontCentered('),
        '@@ASCII@@': re.search(r'const uint8_t kFont5x7\[\]\[5\].*?\n\};', display, re.S).group(),
        '@@SUMMARY@@': part((root/'main/ui_model.h').read_text(), 'struct HomeSummary {', '// Settings-page row count'),
        '@@PROJECT@@': part(state, 'void UpdateHomeAiSummary(', 'void BuildHomeSummary('),
        '@@HOME_INPUT@@': part(inputs, '    // [home-actions]', '    // [home-actions-end]'),
        '@@RUNTIME_POLL@@': part(runtime, 'UiUpdate UiRuntime::DispatchHomeStatusPoll(', 'UiUpdate UiRuntime::DispatchAgentSessionListPoll('),
        '@@ACTIVITY@@': part(button, '    if (event.HasEvent() && !event.repeat && event_time_ms >= home_activity_at_ms_)', '    if (wqn::HasBufferedWordObservations('),
        '@@QUERY@@': part(worker, 'void LoadHomeStatus()', 'void LoadSessions()'),
        '@@QUERY_REQUEST@@': part(worker, 'esp_err_t RequestOpenCodeHomeStatus()', 'esp_err_t CreateNewOpenCodeSession('),
        '@@OPEN@@': part(worker, 'esp_err_t OpenCodeHomeSession(', 'esp_err_t RequestOpenCodeHomeStatus()'),
        '@@ATTACHED_ROW@@': part(worker, 'static void RememberAttachedSessionLocked(', 'static esp_err_t LockSessionAndUnlock('),
        '@@LAYOUT@@': part(layout, '// ---- Screen (re-exported', '// ---- Global page navigation ----'),
        '@@RECT@@': re.search(r'constexpr UiRect kHomePrimaryRect[^;]+;', internal).group(),
        '@@PAGE@@': re.sub(r'^#include[^\n]*\n', '', page, flags=re.M),
        '@@BATTERY@@': part(digits, 'const char* BatteryIconGlyph(', '\n}  // namespace\n'),
        '@@STATUS_ICONS@@': part(digits, 'int DrawWifiStatusIcon(', '\n}  // namespace device_ui_internal'),
    }
    for key, value in pieces.items():
        assert fixture.count(key) == 1, key
        fixture = fixture.replace(key, value)
    with tempfile.TemporaryDirectory(prefix='wqn-home-host-') as temp:
        folder = Path(temp)
        (folder/'lvgl.h').write_text('#include "font_fmt.h"\n')
        font = folder/'font.o'
        icon_font = folder/'icons.o'
        native_font = folder/'native.o'
        inc = root/'components/display_service/include'
        subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),
                        '-c',str(root/'components/display_service/fonts/SourceHanSansSC_Regular_slim.c'),
                        '-o',str(font)], check=True, timeout=30)
        subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),
                        '-c',str(root/'components/display_service/fonts/font_zectrix_16_1.c'),
                        '-o',str(icon_font)], check=True, timeout=30)
        subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),
                        '-I',str(root/'main/ui'),'-c',str(root/'main/ui/ui_fonts.c'),
                        '-o',str(native_font)], check=True, timeout=30)
        (folder/'home.cpp').write_text(fixture)
        binary = folder/'home'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(inc),
                        '-I',str(folder),'-I',str(root/'main'),'-I',str(root/'main/ui'),str(folder/'home.cpp'),str(args.model_source),
                        str(root/'main/ui/navigation_model.cpp'),str(font),str(icon_font),str(native_font),'-o',str(binary)], check=True, timeout=30)
        command = [str(binary)]
        if args.render_dir:
            args.render_dir.mkdir(parents=True, exist_ok=True)
            command.append(str(args.render_dir.resolve()))
        subprocess.run(command, check=True, timeout=20, cwd=folder)


if __name__ == '__main__':
    main()
