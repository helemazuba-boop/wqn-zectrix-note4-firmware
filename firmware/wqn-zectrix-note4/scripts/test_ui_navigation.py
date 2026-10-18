#!/usr/bin/env python3
"""Exercise production menu input, direct navigation, modal gates and composition.

Hardware, storage workers and cloud requests are counting seams. These host
checks do not validate panel waveforms, on-device concurrency or sleep/wake.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def section(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError(f'production boundary drift: {start}')
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input-source', type=Path, default=root / 'main/ui/ui_input.cpp')
    parser.add_argument('--navigation-source', type=Path, default=root / 'main/ui/navigation_model.cpp')
    parser.add_argument('--model-source', type=Path, default=root / 'main/ui_model.cpp')
    parser.add_argument('--graphics-source', type=Path, default=root / 'main/ui/graphics.cpp')
    parser.add_argument('--view-source', type=Path, default=root / 'main/ui/navigation_view.cpp')
    args = parser.parse_args()
    inputs = args.input_source.read_text()
    model = args.model_source.read_text()
    graphics = args.graphics_source.read_text()
    layout = (root / 'main/ui/ui_layout.h').read_text()
    internal = (root / 'main/ui/ui_internal.h').read_text()
    view = args.view_source.read_text()
    display = (root / 'components/display_service/display_service.cpp').read_text()
    fixture = (root / 'scripts/testdata/ui_navigation.cpp').read_text()
    pieces = {
        '@@SETTINGS_DIALOG@@': section((root/'main/ui_model.h').read_text(), 'enum class SettingsDialog {', '// [dev-diag] One formatted error-log line'),
        '@@TRANSITIONS@@': section(model, 'static void ApplyUiScreenTransition(', '\nvoid HandleUiInput('),
        '@@INPUT@@': section(inputs, 'static RefreshSchedule FinishScreenNavigation(', '\nRefreshSchedule ApplyButtonEvent('),
        '@@BRIDGE@@': section(inputs, '    const wqn::NavigationAction navigation =', '\n    // [shell] Status-bar edit mode intercepts'),
        '@@GEOMETRY@@': section(layout, '// ---- Global page navigation ----', '// ---- AI page'),
        '@@SCHEDULE@@': section(internal, 'enum class RefreshSchedule {', '\nint RefreshRank('),
        '@@VIEW@@': section(view, 'static_assert(', '\nesp_err_t RefreshNavigationGrayImage('),
        '@@GRAY@@': section(view, 'esp_err_t RefreshNavigationGrayImage(', '\n}  // namespace device_ui_internal'),
        '@@LINES@@': section(graphics, 'void DrawHorizontalLine(', '// [agent] Dashed outline.'),
        '@@OUTLINE@@': section(graphics, 'void DrawRoundedRect(', '// [rowfill] Filled rounded rect'),
        '@@ROUND@@': section(graphics, 'void FillRoundedRect(', '\nvoid ClearRect('),
        '@@READBACK@@': section(display, 'bool ReadEpdPixel(', '\nvoid DrawEpdPixel('),
        '@@REFRESH@@': section(graphics, 'esp_err_t RefreshFrame(', '\n}  // namespace device_ui_internal'),
    }
    for marker, body in pieces.items():
        if fixture.count(marker) != 1:
            raise ValueError(f'fixture marker drift: {marker}')
        fixture = fixture.replace(marker, body)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-navigation-') as directory:
        unit, binary = Path(directory) / 'navigation.cpp', Path(directory) / 'navigation'
        unit.write_text(fixture)
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I', str(root / 'main'), '-I', str(root / 'main/ui'), str(unit),
                        str(args.navigation_source), '-o', str(binary)],
                       check=True, timeout=30)
        return subprocess.run([str(binary)], check=False, timeout=15, cwd=directory).returncode


if __name__ == '__main__':
    raise SystemExit(main())
