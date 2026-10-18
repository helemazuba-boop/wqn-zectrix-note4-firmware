#!/usr/bin/env python3
"""Run production word projection, signatures and native 1bpp rendering.

Only the panel/RTOS/storage ports are host seams. Exported frames are production
draw calls with real fonts, not HTML screenshots or device/HIL evidence.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def section(text, start, end):
    if text.count(start) != 1:
        raise ValueError(f'Production boundary drift: {start}')
    first = text.index(start)
    return text[first:text.index(end, first)]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render-dir', type=Path)
    args = parser.parse_args()
    display = (root/'components/display_service/display_service.cpp').read_text()
    app = (root/'main/word_app.cpp').read_text()
    header = (root/'main/word_app.h').read_text()
    digits = (root/'main/ui/seven_segment.cpp').read_text()
    page = (root/'main/ui/page_word.cpp').read_text()
    refresh = (root/'main/ui/ui_refresh.cpp').read_text()
    pieces = {
        '@@ENUMS@@': section(header, 'enum class WordInput {', '// One word the user'),
        '@@SNAPSHOT_TYPE@@': section(header, 'struct WordAppSnapshot {', '// [word-due-hint]'),
        '@@SNAPSHOT@@': section(app, 'WordAppSnapshot BuildWordAppSnapshot(', 'std::string WordAppSignature('),
        '@@COUNT_SETTERS@@': section(app, 'void SetWordReviewDueCount(', 'esp_err_t InitWordApp('),
        '@@COUNT_GUARD@@': section(app, 'class WordDueCountFixtureGuard {', '// WordAppState is intentionally large'),
        '@@FONT_LOOKUP@@': section(display, 'const lv_font_fmt_txt_glyph_dsc_t* FindGlyphInFont(', 'int MeasureCodepointWidth('),
        '@@FONT_DRAW@@': section(display, 'void DrawGlyphFromFont(', 'void DrawGlyphFromFontScaled('),
        '@@FONT_TEXT@@': section(display, 'void DrawTextWithFont(', 'void DrawTextWithFontCentered(').replace('void DrawTextWithFont(', 'void ProductionDrawTextWithFont('),
        '@@LAYOUT@@': section((root/'main/ui/ui_layout.h').read_text(), '// ---- Screen (re-exported', '// ---- Global page navigation ----'),
        '@@BATTERY@@': section(digits, 'const char* BatteryIconGlyph(', '\n}  // namespace\n'),
        '@@ICONS@@': section(digits, 'int DrawWifiStatusIcon(', '\n}  // namespace device_ui_internal'),
        '@@PAGE@@': re.sub(r'^#include[^\n]*\n', '', page, flags=re.M),
        '@@SIGNATURE@@': section(refresh, '    if (frame.screen == wqn::UiScreen::kWord) {', '    if (frame.screen == wqn::UiScreen::kNote) {'),
    }
    fixture = (root/'scripts/testdata/ui_word.cpp').read_text()
    for key, value in pieces.items():
        assert fixture.count(key) == 1, key
        fixture = fixture.replace(key, value)
    assert 'DrawWordActionFooter' not in page and 'DrawRoundedRect' not in page
    assert 'DrawTextWithFontSized' not in page and 'Scaled' not in page
    assert 'kReader' not in page and 'kReader' not in header
    with tempfile.TemporaryDirectory(prefix='wqn-word-ui-') as temp:
        folder = Path(temp)
        (folder/'lvgl.h').write_text('#include "font_fmt.h"\n')
        inc = root/'components/display_service/include'
        objects = []
        for name, source in [('primary', root/'components/display_service/fonts/SourceHanSansSC_Regular_slim.c'),
                             ('icons', root/'components/display_service/fonts/font_zectrix_16_1.c'),
                             ('native', root/'main/ui/ui_fonts.c'), ('word', root/'main/ui/word_fonts.c')]:
            obj = folder/(name+'.o')
            subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),
                            '-I',str(root/'main/ui'),'-c',str(source),'-o',str(obj)],check=True,timeout=60)
            objects.append(str(obj))
        unit = folder/'word.cpp'
        unit.write_text(fixture)
        binary = folder/'word'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),
                        '-I',str(root/'main/ui'),str(unit),*objects,'-o',str(binary)],check=True,timeout=60)
        command = [str(binary)]
        if args.render_dir:
            args.render_dir.mkdir(parents=True, exist_ok=True)
            command.append(str(args.render_dir.resolve()))
        subprocess.run(command,check=True,timeout=30,cwd=folder)


if __name__ == '__main__':
    main()
