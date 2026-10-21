#!/usr/bin/env python3
"""Run production settings input, save-ACK, signature and native drawing seams.

Cloud, NVS, the panel and power requests are controlled/counting ports. This
does not establish RTOS races, panel quality, power or device acceptance.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def section(text, start, end):
    if text.count(start) != 1:
        raise ValueError(f'Production boundary changed: {start}')
    first = text.index(start)
    return text[first:text.index(end, first)]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render-dir', type=Path)
    parser.add_argument('--sanitize', action='store_true', help='enable host address/undefined sanitizers')
    parser.add_argument('--page-source', type=Path, default=root/'main/ui/page_settings.cpp')
    parser.add_argument('--input-source', type=Path, default=root/'main/ui/ui_input.cpp')
    parser.add_argument('--refresh-source', type=Path, default=root/'main/ui/ui_refresh.cpp')
    args = parser.parse_args()
    model = (root/'main/ui_model.h').read_text()
    display = (root/'components/display_service/display_service.cpp').read_text()
    internal = (root/'main/ui/ui_internal.h').read_text()
    layout = (root/'main/ui/ui_layout.h').read_text()
    runtime = (root/'main/ui/ui_runtime.cpp').read_text()
    inputs = args.input_source.read_text()
    graphics = (root/'main/ui/graphics.cpp').read_text()
    digits = (root/'main/ui/seven_segment.cpp').read_text()
    diag = (root/'main/ui/diagnostics.cpp').read_text()
    refresh = args.refresh_source.read_text()
    storage = (root/'main/storage.cpp').read_text()
    model_cpp = (root/'main/ui_model.cpp').read_text()
    fixture = (root/'scripts/testdata/ui_settings.cpp').read_text()
    parts = {
        '@@CONNECTIVITY_TYPES@@': section((root/'main/services/connectivity_service.h').read_text(), 'enum class ConnectivityState', 'struct ConnectivityDemandTicket'),
        '@@WIFI_API@@': section((root/'main/services/connectivity_service.h').read_text(), 'enum class WifiNetworkRole', 'class ConnectivityDemand'),
        '@@CONNECTIVITY_SNAPSHOT@@': section((root/'main/services/connectivity_service.h').read_text(), 'struct ConnectivitySnapshot', 'ConnectivityDemand AcquireConnectivityDemand('),
        '@@MODEL@@': section(model, '// Settings-page row count', 'struct UiRuntimeStatus'),
        '@@FONT_LOOKUP@@': section(display, 'const lv_font_fmt_txt_glyph_dsc_t* FindGlyphInFont(', 'int MeasureCodepointWidth('),
        '@@FONT_DRAW@@': section(display, 'void DrawGlyphFromFont(', 'void DrawCjkGlyph('),
        '@@FONT_TEXT@@': section(display, 'void DrawTextWithFont(', 'std::string TruncateUtf8TextToWidth(').replace('void DrawTextWithFont(', 'void DrawProductionTextWithFont('),
        '@@LAYOUT@@': section(layout, '// ---- Screen (re-exported', '// ---- Global page navigation ----'),
        '@@OPTIONS@@': section(internal, 'constexpr size_t kSettingsVisibleRows', '// kStatusBarRect removed'),
        '@@LINES@@': section(graphics, 'void DrawHorizontalLine(', '// [agent] Dashed outline.'),
        '@@FILL@@': section(graphics, 'void FillRect(', '// [rounded]'),
        '@@BATTERY@@': section(digits, 'const char* BatteryIconGlyph(', '\n}  // namespace\n'),
        '@@ICONS@@': section(digits, 'int DrawWifiStatusIcon(', '\n}  // namespace device_ui_internal'),
        '@@OPTIONS_INDEX@@': section(diag, 'size_t AutoSyncOptionIndex(', 'std::string OnlineSyncStatusLabel('),
        '@@PAGE@@': re.sub(r'^#include[^\n]*\n', '', args.page_source.read_text(), flags=re.M),
        '@@PROVISION_PAGE@@': re.sub(r'^#include[^\n]*\n', '', (root/'main/ui/page_provisioning.cpp').read_text(), flags=re.M),
        '@@INPUT@@': section(inputs, 'bool AnyLocalPersistPending(', '// [detail] Cycle the Agent tier'),
        '@@SIGNATURE@@': section(refresh, '    if (frame.screen == wqn::UiScreen::kProvisioning) {', '    for (const wqn::UiLine& line : frame.lines)'),
        '@@SIGNATURE_HEADER@@': section(refresh, '    std::string signature = std::to_string(static_cast<int>(frame.screen));', '    const bool time_config_mode ='),
        '@@TICK@@': section(runtime, 'UiUpdate UiRuntime::DispatchSettingsTick(', 'UiUpdate UiRuntime::DispatchStatusReload('),
        '@@AUTO_ACK@@': section(runtime, 'UiUpdate UiRuntime::DispatchAutoSyncSaveResult(', 'UiUpdate UiRuntime::DispatchWordSessionResetResult('),
        '@@OTHER_ACK@@': section(runtime, 'UiUpdate UiRuntime::DispatchVolumeSaveResult(', 'UiUpdate UiRuntime::DispatchAgentDetailSaveResult('),
        '@@LABELS@@': section(storage, 'std::string AutoSyncIntervalLabel(', '// [hw-volume]') +
                      section(storage, 'std::string VolumeLabel(', 'int GetPlaybackVolumePercent(') +
                      section(storage, 'std::string ImageRenderModeLabel(', 'esp_err_t LoadAiAutoFollow('),
        '@@ROUTE@@': section(inputs, 'RefreshSchedule ApplyButtonEvent(', '    // [shell] Status-bar edit mode intercepts') +
                     '    if (state->screen == wqn::UiScreen::kSettings) return ApplySettingsButtonEvent(event, state);\n    return RefreshSchedule::kNone;\n}\n',
        '@@PROVISION_PROJECT@@': section(model_cpp, '    const wqn::ProvisionState provisioning_phase =', '    frame.screen = state.screen;'),
        '@@DURABLE_RELOAD@@': section(diag, 'void UpdateSettingsDiagnostics(', '    // [ai-follow] Durable follow toggle:').replace('void UpdateSettingsDiagnostics(', 'void RefreshDurableSelections(') + '}\n',
    }
    for key, value in parts.items():
        assert fixture.count(key) == 1, key
        fixture = fixture.replace(key, value)
    # Real routing remains explicit; recovery frames keep their generic path.
    render = (root/'main/ui/ui_render.cpp').read_text()
    assert 'frame.provisioning_phase != wqn::ProvisionState::kIdle' in render
    assert 'RenderProvisioningToEpd(frame, schedule)' in render
    with tempfile.TemporaryDirectory(prefix='wqn-settings-host-') as temp:
        folder = Path(temp)
        (folder/'lvgl.h').write_text('#include "font_fmt.h"\n')
        inc = root/'components/display_service/include'
        objects = []
        for name, source in [('primary', root/'components/display_service/fonts/SourceHanSansSC_Regular_slim.c'),
                             ('icons', root/'components/display_service/fonts/font_zectrix_16_1.c'),
                             ('native', root/'main/ui/ui_fonts.c')]:
            obj = folder/(name+'.o')
            subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(folder), '-I', str(inc),
                            '-I', str(root/'main/ui'), '-c', str(source), '-o', str(obj)], check=True, timeout=60)
            objects.append(str(obj))
        qr_include = root/'components/qr_code/include'
        qr_object = folder/'qrcodegen.o'
        subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(qr_include),
                        '-c', str(root/'components/qr_code/qrcodegen.c'), '-o', str(qr_object)], check=True, timeout=30)
        objects.append(str(qr_object))
        unit = folder/'settings.cpp'
        unit.write_text(fixture)
        for dev in (0, 1):
            binary = folder/f'settings-{dev}'
            sanitizer_flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if args.sanitize else []
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', f'-DCONFIG_WQN_DEV_MENU_ENABLE={dev}',
                            *sanitizer_flags,
                            '-I', str(folder), '-I', str(inc), '-I', str(root/'main'), '-I', str(root/'main/ui'),
                            '-I', str(qr_include), str(unit), str(root/'main/ui/navigation_model.cpp'),
                            str(root/'main/ui/provision_qr.cpp'), *objects, '-o', str(binary)], check=True, timeout=60)
            command = [str(binary)]
            if args.render_dir:
                directory = args.render_dir/f'dev-{dev}'
                directory.mkdir(parents=True, exist_ok=True)
                command.append(str(directory.resolve()))
            subprocess.run(command, check=True, timeout=30, cwd=folder)


if __name__ == '__main__':
    main()
