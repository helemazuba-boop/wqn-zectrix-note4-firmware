#!/usr/bin/env python3
"""Replay production Agent picker/status drawing with real native font bitmaps.

The panel/network/clock ports are host substitutes, not device acceptance.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def section(source, start, end):
    assert source.count(start) == 1, start
    first = source.index(start)
    return source[first:source.index(end, first)]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render-dir', type=Path)
    parser.add_argument('--handoff-only', action='store_true')
    parser.add_argument('--mutation', choices=('bypass-transcribing-modal', 'keep-transcribing-editor', 'lose-midlength-short'))
    args = parser.parse_args()
    if args.mutation and not args.handoff_only:
        parser.error('--mutation requires --handoff-only')
    page = (root/'main/ui/page_ai_agent.cpp').read_text()
    display = (root/'components/display_service/display_service.cpp').read_text()
    refresh = (root/'main/ui/ui_refresh.cpp').read_text()
    fixture = (root/'scripts/testdata/ui_agent.cpp').read_text()
    input_source = (root/'main/ui/ui_input.cpp').read_text()
    ai_model = (root/'main/ui_model.h').read_text()
    ai_model = ai_model[ai_model.index('enum class AiSessionStatus'):ai_model.index('enum class TodoSyncStatus')]
    std_ui = (root/'main/ui/ai_std_interaction.cpp').read_text()
    std_ui = std_ui[std_ui.index('wqn::AgentSessionState StdDecisionSurface('):std_ui.rfind('\n}')]
    runtime = (root/'main/ui/ui_runtime.cpp').read_text()
    if args.mutation == 'bypass-transcribing-modal':
        needle = 'state.ai.status == wqn::AiSessionStatus::kTranscribing ||'
        assert std_ui.count(needle) == 1
        std_ui = std_ui.replace(needle, 'false ||')
    if args.mutation == 'lose-midlength-short':
        needle = 'event.type == wqn::ButtonEventType::kLongPress ||'
        assert std_ui.count(needle) == 1
        std_ui = std_ui.replace(needle, 'event.type == wqn::ButtonEventType::kHoldPress ||')
    if args.mutation == 'keep-transcribing-editor':
        needle = 'snapshot.status == wqn::AiSessionStatus::kTranscribing ||'
        assert runtime.count(needle) == 1
        runtime = runtime.replace(needle, 'false ||')
    replacements = {
        '@@AI_MODEL@@': ai_model,
        '@@SNAPSHOT@@': section(runtime, 'UiUpdate UiRuntime::DispatchAiSessionSnapshot(', '// [follow] Per-tick auto-follow.'),
        '@@STD_UI@@': std_ui,
        '@@STD_ROUTING@@': section(input_source,
            '    // [shell] Status-bar edit mode intercepts all input on the AI page.',
            '    RefreshSchedule agent_schedule ='),
        '@@FONT_LOOKUP@@': section(display, 'const lv_font_fmt_txt_glyph_dsc_t* FindGlyphInFont(', 'int MeasureCodepointWidth('),
        '@@FONT_DRAW@@': section(display, 'void DrawGlyphFromFont(', 'void DrawCjkGlyph('),
        '@@FONT_MEASURE@@': section(display, 'int MeasureTextWithFont(', 'std::string TruncateUtf8TextToWidth('),
        '@@BAR_CONSTANTS@@': section(page, 'constexpr int kAgentBarY', '#define AGENT_TEXT'),
        '@@DECISIONS@@': section(page, 'AgentOptionMode AgentOptionModeFor(', '// ---------------------------------------------------------------------------\n// Session picker'),
        '@@INPUT@@': section(input_source, 'static RefreshSchedule ExecuteAgentOption(', '// [shell] Status-bar edit mode owns'),
        '@@SCROLL_OWNER@@': section((root/'main/opencode_session.cpp').read_text(), 'bool ScrollOpenCodeResponse(int direction,', 'bool CopyOpenCodeSessionToUi('),
        '@@PICKER@@': section(page, 'static std::string AgentSessionMetadata(', '// ---------------------------------------------------------------------------\n// Status bar'),
        '@@STATUS@@': section(page, 'static const WqnBitmapAsset& AgentDetailAsset(', '// ---------------------------------------------------------------------------\n// Entry point'),
        '@@SIGNATURE@@': section(refresh, '    if (frame.screen == wqn::UiScreen::kAi &&\n        frame.ai.tier == wqn::AiTier::kAgent) {', '    if (frame.screen == wqn::UiScreen::kTodo) {'),
    }
    for key, value in replacements.items():
        assert fixture.count(key) == 1, key
        fixture = fixture.replace(key, value)
    if args.handoff_only:
        assert fixture.count('int main(') == 1
        fixture = fixture.replace('int main(', 'int LegacyUiMain(')
        fixture += (root/'scripts/testdata/std_gesture_handoff.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='wqn-agent-host-') as temp:
        folder = Path(temp)
        (folder/'esp_err.h').write_text('#pragma once\nusing esp_err_t = int;\nconstexpr int ESP_OK=0;\n')
        (folder/'lvgl.h').write_text('#include "font_fmt.h"\n')
        inc = root/'components/display_service/include'
        objects = []
        for name, source in [('primary', root/'components/display_service/fonts/SourceHanSansSC_Regular_slim.c'),
                             ('native', root/'main/ui/ui_fonts.c')]:
            obj = folder/(name+'.o')
            subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),
                            '-I',str(root/'main/ui'),'-c',str(source),'-o',str(obj)],check=True,timeout=60)
            objects.append(str(obj))
        unit = folder/'agent.cpp'
        unit.write_text(fixture)
        binary = folder/'agent'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),
                        '-I',str(root/'main'),'-I',str(root/'main/ui'),str(unit),str(root/'main/ui/ai_text_layout.cpp'),str(root/'main/ui/markdown_layout.cpp'),*objects,'-pthread','-o',str(binary)],
                       check=True,timeout=60)
        command = [str(binary)]
        if args.render_dir:
            args.render_dir.mkdir(parents=True,exist_ok=True)
            command.append(str(args.render_dir.resolve()))
        return subprocess.run(command,check=False,timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
