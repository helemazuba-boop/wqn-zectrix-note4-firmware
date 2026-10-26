#!/usr/bin/env python3
"""Production AI layout/cache, viewport pixels and concurrent host consumers.

No real panel, RTOS scheduler or device heap measurements are claimed.
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
    parser.add_argument('--render-dir', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    display = (root/'components/display_service/display_service.cpp').read_text()
    page = (root/'main/ui/page_ai.cpp').read_text()
    fixture = (root/'scripts/testdata/ai_reading.cpp').read_text()
    owner = (root/'main/opencode_session.cpp').read_text()
    owner = owner[:owner.rindex('\n#else')]
    std_owner = (root/'main/ai_session.cpp').read_text()
    std_owner = std_owner[:std_owner.rindex('\n#else')]
    runtime = (root/'main/ui/ui_runtime.cpp').read_text()
    replacements = {
        '@@STD_FOLLOW_OWNER@@': '\n'.join(function(std_owner, name) for name in ['void ArmAiFollowLocked()', 'bool ApplyAiViewportFollow(', 'void SetAiFollowState(']),
        '@@FOLLOW_OWNER@@': '\n'.join(function(owner, name) for name in ['void ArmAgentFollowLocked()', 'bool ApplyOpenCodeViewportFollow(', 'void SetOpenCodeFollowState(']),
        '@@FOLLOW_RUNTIME@@': section(runtime, 'UiUpdate UiRuntime::DispatchAiViewportFollow()', 'UiUpdate UiRuntime::DispatchFlashSnapshot('),
        '@@AI_MODEL@@': section((root/'main/ui_model.h').read_text(), 'enum class AiSessionStatus {', 'enum class TodoSyncStatus {'),
        '@@FONT_LOOKUP@@': section(display, 'const lv_font_fmt_txt_glyph_dsc_t* FindGlyphInFont(', 'int MeasureCodepointWidth('),
        '@@FONT_DRAW@@': section(display, 'void DrawGlyphFromFont(', 'void DrawCjkGlyph('),
        '@@FONT_MEASURE@@': section(display, 'int MeasureTextWithFont(', 'std::string TruncateUtf8TextToWidth('),
        '@@HISTORY_MODEL@@': section((root/'main/ui/ui_internal.h').read_text(), 'struct AiHistoryLayout {', '// Scroll offset that brings'),
        '@@MD_RENDER@@': re.sub(r'^#include[^\n]*\n', '', (root/'main/ui/markdown_render.cpp').read_text(), flags=re.M),
        '@@PAGE@@': section(page, 'std::vector<std::string> WrapForViewport(', 'esp_err_t RenderAiToEpd('),
    }
    for key, value in replacements.items():
        assert fixture.count(key) == 1, key
        fixture = fixture.replace(key, value)
    with tempfile.TemporaryDirectory(prefix='wqn-ai-reading-') as temp:
        folder = Path(temp)
        (folder/'esp_err.h').write_text('#pragma once\nusing esp_err_t = int;\nconstexpr int ESP_OK=0;\n')
        (folder/'lvgl.h').write_text('#include "font_fmt.h"\n')
        (folder/'freertos').mkdir()
        (folder/'freertos/FreeRTOS.h').write_text('#pragma once\nstruct StaticSemaphore_t {};\nusing SemaphoreHandle_t=void*;\n')
        (folder/'freertos/semphr.h').write_text('#include "FreeRTOS.h"\n')
        inc = root/'components/display_service/include'
        objects = []
        for name, source in [('primary', root/'components/display_service/fonts/SourceHanSansSC_Regular_slim.c'),
                             ('native', root/'main/ui/ui_fonts.c')]:
            obj = folder/(name+'.o')
            subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),
                            '-I',str(root/'main/ui'),'-c',str(source),'-o',str(obj)],check=True,timeout=60)
            objects.append(str(obj))
        unit = folder/'reading.cpp'
        unit.write_text(fixture)
        binary = folder/'reading'
        sanitizers = ['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run(['g++','-DWQN_HOST_TEST=1','-std=c++17','-O1','-Wall','-Wextra','-Werror','-pthread',*sanitizers,
                        '-I',str(folder),'-I',str(inc),'-I',str(root/'main'),'-I',str(root/'main/ui'),
                        str(unit),str(root/'main/ui/ai_text_layout.cpp'),str(root/'main/ui/markdown_layout.cpp'),
                        *objects,'-o',str(binary)],check=True,timeout=60)
        command = [str(binary)]
        if args.render_dir:
            args.render_dir.mkdir(parents=True,exist_ok=True)
            command.append(str(args.render_dir.resolve()))
        subprocess.run(command,check=True,timeout=60)


if __name__ == '__main__':
    main()
