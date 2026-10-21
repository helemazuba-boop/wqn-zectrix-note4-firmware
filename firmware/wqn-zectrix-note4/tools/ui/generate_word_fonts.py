#!/usr/bin/env python3
"""Generate native 1bpp Latin/IPA word faces from the existing local Noto fonts.

Uses the same packing, threshold and pinned tools as generate_ui_fonts.py.
The existing UI fonts and their consumers are left byte-for-byte unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path

from fontTools.ttLib import TTFont
from generate_ui_fonts import ROOT, PROJECTS, NOTO_SHA, font_data

REGULAR_SHA = 'fe8c022f48d8dd29f17b744d16f9346f4357e16f7d4f7be58b000ae7c291b614'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = PROJECTS / 'ref/crosspoint-reader/lib/EpdFont/builtinFonts/source/NotoSans'
    parser.add_argument('--noto-regular', type=Path, default=source / 'NotoSans-Regular.ttf')
    parser.add_argument('--noto-bold', type=Path, default=source / 'NotoSans-Bold.ttf')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    for path, expected in [(args.noto_regular, REGULAR_SHA), (args.noto_bold, NOTO_SHA)]:
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Unexpected source font: {path}')
    ranges = [(0x20, 0x7f), (0xa0, 0x400), (0x1d00, 0x1dc0),
              (0x1e00, 0x1f00), (0x2000, 0x2070)]
    wanted = {cp for first, end in ranges for cp in range(first, end)}
    required = set(map(ord, 'æɑɒɔəɚɛɜɝɡɪʊʌʃʒŋθðɹɾʔˈˌːˑ̩̃͡'))
    generated = ['// Generated native 1bpp Latin/IPA; tools/ui/generate_word_fonts.py.',
                 '#include "word_fonts.h"', '']
    manifest = {'noto_regular_sha256': REGULAR_SHA, 'noto_bold_sha256': NOTO_SHA,
                'threshold': 96, 'fonts': []}
    for path, size, name in [(args.noto_regular, 16, 'wqn_word_regular_16'),
                             (args.noto_regular, 20, 'wqn_word_regular_20'),
                             (args.noto_bold, 24, 'wqn_word_bold_24'),
                             (args.noto_bold, 32, 'wqn_word_bold_32')]:
        cmap = TTFont(path).getBestCmap()
        if not required <= cmap.keys():
            raise ValueError(f'{name}: missing required IPA {sorted(required - cmap.keys())}')
        codepoints = sorted(wanted & cmap.keys())
        code, record = font_data(path, size, codepoints, name)
        generated.append(code)
        manifest['fonts'].append(record)
    for target, content in {
        ROOT / 'main/ui/word_fonts.c': '\n'.join(generated),
        ROOT / 'main/ui/assets/word_fonts_manifest.json': json.dumps(manifest, indent=2) + '\n',
    }.items():
        if args.check:
            if not target.exists() or target.read_text() != content:
                raise SystemExit(f'FAIL: {target} differs; regenerate')
        else:
            target.write_text(content)
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
