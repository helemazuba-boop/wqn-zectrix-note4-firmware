#!/usr/bin/env python3
"""Production note snapshots, native pixels, state and refresh seams.

Clock/storage/hardware ports are host substitutes, not device HIL.
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
    parser.add_argument('--input-source', type=Path, default=root/'main/ui/ui_input.cpp')
    args = parser.parse_args()
    page = (root/'main/ui/page_note.cpp').read_text()
    app = (root/'main/note_app.cpp').read_text()
    model = (root/'main/note_app.h').read_text()
    pack = (root/'main/note_pack.h').read_text()
    store = (root/'main/note_store.h').read_text()
    display = (root/'components/display_service/display_service.cpp').read_text()
    digits = (root/'main/ui/seven_segment.cpp').read_text()
    refresh = (root/'main/ui/ui_refresh.cpp').read_text()
    inputs = args.input_source.read_text()
    internal = (root/'main/ui/ui_internal.h').read_text()
    protocol = (root/'components/device_protocol/include/device_protocol/note_study.h').read_text()
    pieces = {
        '@@SCHEDULE@@': section(internal, 'enum class RefreshSchedule {', '\nint RefreshRank('),
        '@@OLD_NOTE_WINDOWS@@': '\n'.join(re.findall(r'^    const size_t old_(?:notebook|note_list)_window =[^\n]+', inputs, flags=re.M)),
        '@@NOTE_REFRESH_POLICY@@': section(inputs,
            '        if (state->note_app.mode != old_note_mode ||',
            '    if (state->selected_home_task != old_home_task ||'),
        '@@PROTOCOL@@': section(protocol, 'enum class Mode', 'struct CandidatePageRequest'),
        '@@PACK@@': section(pack, 'struct WqnNoteEntry {', 'esp_err_t InitNotePackStorage();'),
        '@@STORE@@': section(store, 'struct StoredNotePackSnapshot {', 'esp_err_t CompactNoteSessionData(') + '\n' + section(store, 'struct DurableNoteObservation {', 'struct NoteOutboxSnapshot {'),
        '@@MODEL@@': section(model, 'enum class NoteInput {', 'esp_err_t InitNoteApp('),
        '@@FIND@@': function(app, 'size_t FindNotebookRow(') + '\n' + function(app, 'const wqn::NotePackIndexEntry* FindPackEntry('),
        '@@LOAD@@': function(app, 'void LoadCurrentNoteBody('),
        '@@BOUNDS@@': function(app, 'device_ui_internal::NoteReadingGeometry NoteBodyReadingGeometry('),
        '@@INTERACTION@@': '\n'.join(function(app, name) for name in [
            'uint32_t NextTransferProgressGeneration(', 'void ArmNoteBodyFetch(', 'void RequestNoteCandidatePageIfNeeded(',
            'void InstallNotePackIndex(', 'void ActivatePendingNotePackIndex(',
            'void HandleNotebookListInput(', 'void HandleSessionStartingInput(',
            'void ArmOpenObservation(',
            'void ResetNoteImageViewer(wqn::NoteAppState* state)\n{',
            'void HandleNoteListInput(wqn::NoteAppState* state, wqn::NoteInput input)\n{',
            'void RequestCurrentNoteImage(', 'void EnterNoteImageView(', 'void ExitNoteImageViewToBody(',
            'void HandleNoteViewInput(wqn::NoteAppState* state, wqn::NoteInput input)\n{',
            'void HandleNoteImageViewInput(']),
        '@@DISPATCH@@': function(app, 'bool TakeNoteBodyFetchRequest(') + '\n' + function(app, 'esp_err_t HandleNoteAppInput(') + '\n' + function(app, 'void ApplyNoteBodyFetchResult('),
        '@@BODY@@': section(page, 'void DrawTransferProgressBar(', '\n}  // namespace\n'),
        '@@MD_RENDER@@': re.sub(r'^#include[^\n]*\n', '', (root/'main/ui/markdown_render.cpp').read_text(), flags=re.M),
        '@@WINDOW@@': function(app, 'void UpdateListViewport('),
        '@@SNAPSHOT@@': section(app, 'NoteAppSnapshot BuildNoteAppSnapshot(', 'std::string NoteAppStatusLine('),
        '@@NOTICE@@': section(app, 'std::string NoteAppStatusLine(', '\nnamespace {\n\nstruct NotePageFixture'),
        '@@FONT_LOOKUP@@': section(display, 'const lv_font_fmt_txt_glyph_dsc_t* FindGlyphInFont(', 'int MeasureCodepointWidth('),
        '@@FONT_DRAW@@': section(display, 'void DrawGlyphFromFont(', 'void DrawCjkGlyph('),
        '@@FONT_MEASURE@@': section(display, 'int MeasureTextWithFont(', 'std::string TruncateUtf8TextToWidth('),
        '@@BATTERY@@': section(digits, 'const char* BatteryIconGlyph(', '\n}  // namespace\n'),
        '@@ICONS@@': section(digits, 'int DrawWifiStatusIcon(', '\n}  // namespace device_ui_internal'),
        '@@LISTS@@': section(page, 'constexpr char kTag[]', '// [transfer-progress]'),
        '@@SIGNATURE@@': section(refresh, '    if (frame.screen == wqn::UiScreen::kNote) {', '        // Problem browse layer (rides on the note screen).') + '\n    }',
    }
    fixture = (root/'scripts/testdata/ui_note.cpp').read_text()
    for marker, body in pieces.items():
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='wqn-note-host-') as temp:
        folder = Path(temp)
        (folder/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0;\n')
        (folder/'lvgl.h').write_text('#include "font_fmt.h"\n')
        inc = root/'components/display_service/include'
        objects = []
        fonts = [('primary',root/'components/display_service/fonts/SourceHanSansSC_Regular_slim.c'),
                 ('native',root/'main/ui/ui_fonts.c'),
                 ('icons',root/'components/display_service/fonts/font_zectrix_16_1.c')]
        for name, source in fonts:
            obj = folder/(name+'.o')
            subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I',str(folder),'-I',str(inc),
                            '-I',str(root/'main/ui'),'-c',str(source),'-o',str(obj)],check=True,timeout=60)
            objects.append(str(obj))
        unit,binary = folder/'note.cpp',folder/'note'
        unit.write_text(fixture)
        sanitize = ['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run(['g++','-std=c++17','-DWQN_HOST_TEST','-O1','-Wall','-Wextra','-Werror','-pthread',*sanitize,
                        '-I',str(folder),'-I',str(inc),'-I',str(root/'main'),'-I',str(root/'main/ui'),
                        str(unit),str(root/'main/ui/ai_text_layout.cpp'),str(root/'main/ui/markdown_layout.cpp'),*objects,'-o',str(binary)],check=True,timeout=60)
        command = [str(binary)]
        if args.render_dir:
            args.render_dir.mkdir(parents=True,exist_ok=True)
            command.append(str(args.render_dir.resolve()))
        subprocess.run(command,check=True,timeout=45)


if __name__ == '__main__':
    main()
