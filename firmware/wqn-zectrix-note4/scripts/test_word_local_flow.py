#!/usr/bin/env python3
"""Real local worker + descriptor/session files + 7014 journal events.
Only cached-pack loading, queue transport, NVS and StorageService scheduling are
host seams. No network, flash, RTOS timing or physical power-loss HIL claim.
"""
from pathlib import Path
import test_word_local_descriptor as store

def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]

def main():
    root = Path(__file__).resolve().parents[1]
    cloud = (root/'main/ui/word_cloud.cpp').read_text()
    header = (root/'main/ui/ui_internal.h').read_text()
    app = (root/'main/word_app.h').read_text()
    source = (root/'scripts/testdata/word_local_flow.cpp').read_text()
    source = source.replace('@@BASE@@', str(root/'scripts/testdata/word_local_descriptor.cpp'))
    pieces = {
        '@@DECK@@': section(app, 'struct WordDeckInfo {', '\nstruct WordCardPrefetchState'),
        '@@WIRE@@': section(header, 'enum class WordCloudOp {', '\nenum class NoteCloudOp'),
        '@@QUEUE@@': section(cloud, 'std::string MakeStableLocalUuid(', '\nvoid PumpWordSessionStart('),
        '@@WORKER@@': section(cloud, 'void ExecuteLocalWordStart(', '\n}  // namespace\n\nvoid ExecuteWordCloudRequest'),
    }
    for marker, body in pieces.items():
        source = source.replace(marker, body)
    return store.main(source)

if __name__ == '__main__':
    raise SystemExit(main())
