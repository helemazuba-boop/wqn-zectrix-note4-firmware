#!/usr/bin/env python3
"""Production problem state, native page pixels, paging and refresh replay.

File/clock/panel ports are host substitutes; this is not device HIL.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_ui_note import section


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--render-dir', type=Path)
    parser.add_argument('--input-source', type=Path, default=root/'main/ui/ui_input.cpp')
    args = parser.parse_args()
    app = (root/'main/problem_app.cpp').read_text()
    display = (root/'components/display_service/display_service.cpp').read_text()
    inputs = args.input_source.read_text()
    refresh = (root/'main/ui/ui_refresh.cpp').read_text()
    digits = (root/'main/ui/seven_segment.cpp').read_text()
    pieces = {
        '@@PROTOCOL@@': section((root/'components/device_protocol/include/device_protocol/problem_study.h').read_text(), 'inline constexpr size_t kMaxImagesPerRow', 'struct ManifestPack'),
        '@@PACK@@': section((root/'main/problem_pack.h').read_text(), 'struct WqnProblemPackChoice', 'esp_err_t InitProblemPackStorage();'),
        '@@STORE@@': section((root/'main/problem_store.h').read_text(), 'struct DurableProblemObservation', '// Appends one verdict'),
        '@@MODEL@@': section((root/'main/problem_app.h').read_text(), 'enum class ProblemInput', 'esp_err_t InitProblemApp('),
        '@@APP_LOCAL@@': section(app, 'constexpr char kTag[]', '\n}  // namespace\n\nnamespace wqn {'),
        '@@APP@@': section(app, 'void SetProblemImageRenderMode(', 'esp_err_t InitProblemApp(') + section(app, 'esp_err_t HandleProblemAppInput(', '\n}  // namespace wqn'),
        '@@SCHEDULE@@': section((root/'main/ui/ui_internal.h').read_text(), 'enum class RefreshSchedule', '\nint RefreshRank('),
        '@@OLD_WINDOW@@': '\n'.join(re.findall(r'^    const size_t old_problem_list_window =[^\n]+', inputs, flags=re.M)),
        '@@POLICY@@': section(inputs, '        if (state->problem_app.active != old_problem_active ||', '    if (state->screen == wqn::UiScreen::kNote &&\n        wqn::NoteAppSignature'),
        '@@SIGNATURE@@': section(refresh, '        const wqn::ProblemAppSnapshot& problem = frame.problem_app;', '    if (frame.screen == wqn::UiScreen::kProvisioning)'),
        '@@FONT_LOOKUP@@': section(display, 'const lv_font_fmt_txt_glyph_dsc_t* FindGlyphInFont(', 'int MeasureCodepointWidth('),
        '@@FONT_DRAW@@': section(display, 'void DrawGlyphFromFont(', 'void DrawCjkGlyph('),
        '@@FONT_MEASURE@@': section(display, 'int MeasureTextWithFont(', 'std::string TruncateUtf8TextToWidth('),
        '@@BATTERY@@': section(digits, 'const char* BatteryIconGlyph(', '\n}  // namespace\n'),
        '@@ICONS@@': section(digits, 'int DrawWifiStatusIcon(', '\n}  // namespace device_ui_internal'),
        '@@MD@@': re.sub(r'^#include[^\n]*\n', '', (root/'main/ui/markdown_render.cpp').read_text(), flags=re.M),
        '@@PAGE@@': section((root/'main/ui/page_problem.cpp').read_text(), 'namespace device_ui_internal {', '\n}  // namespace device_ui_internal') + '\n}',
    }
    fixture = (root/'scripts/testdata/ui_problem.cpp').read_text()
    for marker, body in pieces.items():
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-problem-ui-') as temp:
        folder = Path(temp)
        (folder/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0;\n')
        (folder/'lvgl.h').write_text('#include "font_fmt.h"\n')
        inc = root/'components/display_service/include'
        objects = []
        for name, source in [('primary', root/'components/display_service/fonts/SourceHanSansSC_Regular_slim.c'), ('native', root/'main/ui/ui_fonts.c'), ('icons', root/'components/display_service/fonts/font_zectrix_16_1.c')]:
            obj = folder/(name+'.o')
            subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),'-I',str(root/'main/ui'),'-c',str(source),'-o',str(obj)],check=True,timeout=60)
            objects.append(str(obj))
        unit, binary = folder/'problem.cpp', folder/'problem'
        unit.write_text(fixture)
        sanitize = ['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run(['g++','-DWQN_HOST_TEST','-std=c++17','-O1','-Wall','-Wextra','-Werror','-pthread',*sanitize,'-I',str(folder),'-I',str(inc),'-I',str(root/'main'),'-I',str(root/'main/ui'),str(unit),str(root/'main/ui/ai_text_layout.cpp'),str(root/'main/ui/markdown_layout.cpp'),*objects,'-o',str(binary)],check=True,timeout=180)
        command = [str(binary)]
        if args.render_dir:
            args.render_dir.mkdir(parents=True,exist_ok=True)
            command.append(str(args.render_dir.resolve()))
        subprocess.run(command,check=True,timeout=60)


if __name__ == '__main__':
    main()
