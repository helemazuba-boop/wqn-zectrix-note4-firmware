#!/usr/bin/env python3
"""Package the real host renderer's 400x300 PBM frames (Pillow 11.3.0).

Run scripts/test_ui_settings.py --render-dir DIR first. Only the contact-sheet
labels/background are added here; device pixels are never resized or redrawn.
"""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


def sheet(root, source, names, target):
    items = [name for name in names if (source/(name+'.png')).exists()]
    out = Image.new('RGB', (1280, 30+340*((len(items)+2)//3)), '#dededb')
    draw = ImageDraw.Draw(out)
    font = ImageFont.load_default(size=14)
    for i, name in enumerate(items):
        x, y = 20+i%3*420, 40+i//3*340
        out.paste(Image.open(source/(name+'.png')), (x, y))
        draw.text((x, y-24), name, fill='black', font=font)
    out.save(root/target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    root = parser.parse_args().input.resolve()
    frames = list(root.glob('dev-*/*.pbm'))
    if not frames:
        raise SystemExit('No host PBM frames; run scripts/test_ui_settings.py first')
    for path in frames:
        image = Image.open(path)
        if image.size != (400, 300) or image.mode != '1':
            raise ValueError(f'Unexpected host frame: {path}')
        image.save(path.with_suffix('.png'))
    source = root/'dev-1' if (root/'dev-1').exists() else root/'dev-0'
    sheet(root, source, ['root','wifi','image','follow','auto','volume','battery','storage','about','reset','power','sync'], 'overview.png')
    sheet(root, source, ['dev-menu','dev','deck','deck-scrolled','battery-raw','pairing','errors','sleep','provision'], 'details.png')
    sheet(root, source, ['battery-unknown','battery-charging','battery-full','provision-failed','deck-state-1','deck-state-2','deck-state-3','feedback'], 'states.png')
    print(f'Exported {len(frames)} native 1bpp PNGs and three contact sheets to {root}')


if __name__ == '__main__':
    main()
