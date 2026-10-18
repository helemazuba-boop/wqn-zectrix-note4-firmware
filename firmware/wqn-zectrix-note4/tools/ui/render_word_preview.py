#!/usr/bin/env python3
"""Package actual 400x300 word renderer PBMs without changing device pixels."""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

PROJECTS = Path(__file__).resolve().parents[2].parents[2]


def sheet(root, names, target, font):
    items = [(name, title) for name, title in names if (root/(name+'.png')).exists()]
    result = Image.new('RGB', (1280, 26 + 350*((len(items)+2)//3)), '#e9ece8')
    draw = ImageDraw.Draw(result)
    for i, (name, title) in enumerate(items):
        x, y = 20 + (i % 3)*420, 44 + (i // 3)*350
        draw.text((x, y-27), title, font=font, fill='#17201d')
        result.paste(Image.open(root/(name+'.png')).convert('RGB'), (x, y))
    result.save(root/target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    args = parser.parse_args()
    root = args.input.resolve()
    frames = list(root.glob('*.pbm'))
    if not frames:
        raise SystemExit('Run scripts/test_ui_word.py --render-dir DIR first')
    for path in frames:
        frame = Image.open(path)
        if frame.size != (400, 300) or frame.mode != '1':
            raise ValueError(f'Unexpected production frame: {path}')
        frame.save(path.with_suffix('.png'))
    source = PROJECTS / 'ref/vibecoding-voice/firmware/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf'
    font = ImageFont.truetype(str(source), 16)
    sheet(root, [('home','单词入口'),('front','回忆正面'),('back','释义背面'),
                 ('complete','本轮完成'),('no-due','没有到期'),('save-failed','保存失败'),
                 ('long-front','超长单词正面'),('long-back','超长单词背面'),('long-content','长内容省略')], 'overview.png', font)
    sheet(root, [('unknown','数量未知'),('zero','真实零值'),('unsynced','词库未同步'),
                 ('loading','正在准备'),('start-failed','准备失败'),('saving','正在保存'),
                 ('accented','扩展拉丁字母'),('cjk-headword','中文主词回退'),('focus-1','入口焦点')], 'states.png', font)
    print(f'Exported {len(frames)} native 1bpp frames and two contact sheets: {root}')


if __name__ == '__main__':
    main()
