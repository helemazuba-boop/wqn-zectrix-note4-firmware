#!/usr/bin/env python3
"""Generate native 1bpp UI fonts and mono/gray logos (Pillow 11.3.0/fonttools 4.67.0).

The OTF/TTF sources stay outside the firmware; verify their recorded SHA-256.
No runtime scaling, font engine, SVG decoder or new display owner is added.
"""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from fontTools.pens.basePen import BasePen
from fontTools.svgLib.path import parse_path

ROOT = Path(__file__).resolve().parents[2]
PROJECTS = ROOT.parents[2]
SOURCE_HAN_SHA = '1ee89e1669362dee13851129c0a8a791a87521eb4148e5efbf5d26596738e25b'
NOTO_SHA = '13a813c49624ae3ba3c5c6e72c5ebffc4b9e1e6ea32f421c04069b037c6ad431'


def charset():
    chars = set(map(chr, range(32, 127))) | set('…·×—□')
    for high in range(0xa1, 0xf8):
        for low in range(0xa1, 0xff):
            try:
                chars.add(bytes([high, low]).decode('gb2312'))
            except UnicodeDecodeError:
                pass
    return sorted(map(ord, chars))


def bytes_c(values):
    return '\n'.join('    '+', '.join(f'0x{b:02x}' for b in values[i:i+16])+','
                     for i in range(0, len(values), 16))


def font_data(path, size, codepoints, name):
    face = ImageFont.truetype(str(path), size)
    cmap = TTFont(path).getBestCmap()
    missing = [cp for cp in codepoints if cp not in cmap]
    if missing:
        raise ValueError(f'{name}: missing {missing}')
    bitmap = bytearray()
    descriptors = ['    {0, 0, 0, 0, 0, 0},']
    for cp in codepoints:
        mask, offset = face.getmask2(chr(cp), mode='L', anchor='ls')
        width, height = mask.size
        image = Image.frombytes('L', mask.size, bytes(mask)) if width and height else None
        box = image.point(lambda a: 255 if a >= 96 else 0).getbbox() if image else None
        left, top, right, bottom = box or (0, 0, 0, 0)
        w, h = right-left, bottom-top
        index = len(bitmap)
        packed = 0
        bits = 0
        for yy in range(top, bottom):
            for xx in range(left, right):
                packed = (packed << 1) | int(image.getpixel((xx, yy)) >= 96)
                bits += 1
                if bits == 8:
                    bitmap.append(packed); packed = bits = 0
        if bits:
            bitmap.append(packed << (8-bits))
        advance = round(face.getlength(chr(cp))*16)
        descriptors.append(f'    {{{index}, {advance}, {w}, {h}, {offset[0]+left}, {-offset[1]-bottom}}},')
    assert len(bitmap) < (1 << 20)
    first = codepoints[0]
    offsets = ', '.join(str(cp-first) for cp in codepoints)
    result = f'''static const uint8_t {name}_bitmap[] = {{
{bytes_c(bitmap)}
}};
static const lv_font_fmt_txt_glyph_dsc_t {name}_glyphs[] = {{
{chr(10).join(descriptors)}
}};
static const uint16_t {name}_unicode[] = {{{offsets}}};
static const lv_font_fmt_txt_cmap_t {name}_cmaps[] = {{
    {{{first}, {codepoints[-1]-first+1}, 1, {name}_unicode, NULL, {len(codepoints)}, LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}},
}};
static const lv_font_fmt_txt_dsc_t {name}_data = {{
    .glyph_bitmap = {name}_bitmap, .glyph_dsc = {name}_glyphs, .cmaps = {name}_cmaps,
    .kern_dsc = NULL, .kern_scale = 0, .cmap_num = 1, .bpp = 1,
    .kern_classes = 0, .bitmap_format = 0, .cache = NULL,
}};
const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {size}, .base_line = 0, .subpx = 0,
    .underline_position = -1, .underline_thickness = 1,
    .dsc = &{name}_data, .fallback = NULL, .user_data = NULL,
}};
'''
    return result, {'name': name, 'size_px': size, 'glyphs': len(codepoints), 'bitmap_bytes': len(bitmap)}


class LogoPen(BasePen):
    def __init__(self, draw):
        super().__init__(None); self.draw = draw; self.points = []

    def _moveTo(self, p):
        self.points = [p]

    def _lineTo(self, p):
        self.points.append(p)

    def _curveToOne(self, b, c, d):
        a = self.points[-1]
        for step in range(1, 25):
            t = step/24; u = 1-t
            self.points.append(tuple(u*u*u*a[i]+3*u*u*t*b[i]+3*u*t*t*c[i]+t*t*t*d[i] for i in (0, 1)))

    def _closePath(self):
        # Match the browser's 28px square viewport and xMidYMid meet.
        scale = 28/252*4
        self.draw.polygon([((x-2)*scale, (y-40)*scale+(28-176*28/252)*2)
                           for x, y in self.points], fill=255)
        self.points = []

    def _endPath(self):
        self._closePath()


def brand_mask():
    image = Image.new('L', (112, 112))
    pen = LogoPen(ImageDraw.Draw(image))
    svg = ET.parse(ROOT/'main/ui/assets/brand/w-logo.svg').getroot()
    paths = list(svg.findall('{http://www.w3.org/2000/svg}path'))
    assert svg.attrib['viewBox'] == '2 40 252 176' and len(paths) == 8
    for path in paths:
        parse_path(path.attrib['d'], pen)
    return image.resize((28, 28), Image.Resampling.BOX)


def brand_header():
    image = brand_mask()
    data = bytearray()
    for y in range(28):
        for x in range(0, 32, 8):
            data.append(sum(0x80 >> bit for bit in range(8)
                            if x+bit < 28 and image.getpixel((x+bit, y)) >= 96))
    return '''#pragma once
// Generated from brand/w-logo.svg by tools/ui/generate_ui_fonts.py.
#include "wqn_bitmap_asset.h"
namespace device_ui_internal {
inline constexpr uint8_t kWLogoBits[] = {
'''+bytes_c(data)+'''
};
inline constexpr WqnBitmapAsset kWLogo = {28, 28, 4, sizeof(kWLogoBits), kWLogoBits};
}  // namespace device_ui_internal
'''


def about_brand_header():
    # Measured palette in the demo's ssd2683_waveform.h. Quantise the intended
    # luminance against it rather than assume evenly spaced physical levels.
    palette = (0, 56, 134, 204, 225, 359, 394, 465,
               563, 585, 690, 761, 789, 866, 930, 1000)
    mask = brand_mask()
    levels = []
    for y in range(28):
        for x in range(28):
            # Dark-to-mid gray across the original silhouette; coverage gives
            # antialiased edges on white paper without changing the geometry.
            ink = 56 + (465 - 56) * (x + y) / 54
            luminance = 1000 - mask.getpixel((x, y)) / 255 * (1000 - ink)
            levels.append(min(range(16), key=lambda i: abs(palette[i] - luminance)))
    data = bytes((levels[i] << 4) | levels[i+1] for i in range(0, len(levels), 2))
    return '''#pragma once
// Generated from brand/w-logo.svg by tools/ui/generate_ui_fonts.py --brand-only.
// About-page 4bpp: high nibble first, 0=black, 15=white. Same 28px silhouette.
#include "brand_logo.h"
namespace device_ui_internal {
inline constexpr uint8_t kWLogoGray4[] = {
'''+bytes_c(data)+'''
};
static_assert(sizeof(kWLogoGray4) == kWLogo.width * kWLogo.height / 2);
}  // namespace device_ui_internal
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-han', type=Path, default=PROJECTS/'ref/vibecoding-voice/firmware/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf')
    parser.add_argument('--noto-bold', type=Path, default=PROJECTS/'ref/crosspoint-reader/lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Bold.ttf')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--brand-only', action='store_true', help='generate/check logos without loading font sources')
    args = parser.parse_args()
    if args.brand_only:
        for name, content in [('brand_logo.h', brand_header()), ('about_logo.h', about_brand_header())]:
            target = ROOT/'main/ui/assets'/name
            if args.check:
                if not target.exists() or target.read_text() != content:
                    raise SystemExit(f'FAIL: {target} differs; regenerate')
            else:
                target.write_text(content)
        print('Brand assets match' if args.check else 'Generated brand assets')
        return
    for path, expected in [(args.source_han, SOURCE_HAN_SHA), (args.noto_bold, NOTO_SHA)]:
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Unexpected source font: {path}')
    c = ['// Generated native 1bpp fonts; see tools/ui/generate_ui_fonts.py and font-licenses/.', '#include "ui_fonts.h"', '']
    manifest = {'source_han_sha256': SOURCE_HAN_SHA, 'noto_bold_sha256': NOTO_SHA, 'threshold': 96, 'fonts': []}
    for size in (12, 20, 24):
        source, row = font_data(args.source_han, size, charset(), f'wqn_ui_sans_{size}')
        c.append(source); manifest['fonts'].append(row)
    for size in (24, 32, 48):
        source, row = font_data(args.noto_bold, size, list(range(32, 127)), f'wqn_ui_latin_{size}')
        c.append(source); manifest['fonts'].append(row)
    outputs = {
        ROOT/'main/ui/ui_fonts.c': '\n'.join(c),
        ROOT/'main/ui/assets/ui_fonts_manifest.json': json.dumps(manifest, indent=2)+'\n',
        ROOT/'main/ui/assets/brand_logo.h': brand_header(),
        ROOT/'main/ui/assets/about_logo.h': about_brand_header(),
    }
    for target, content in outputs.items():
        if args.check:
            if not target.exists() or target.read_text() != content:
                raise SystemExit(f'FAIL: {target} differs; regenerate')
        else:
            target.write_text(content)
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
