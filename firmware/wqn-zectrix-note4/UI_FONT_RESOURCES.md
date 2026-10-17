# Native UI fonts and brand resources

The UI assets are offline-generated, uncompressed 1bpp glyphs for the existing
display service. They do not introduce an LVGL runtime or change panel ownership.
Compiling a font does not migrate a page automatically; each page must select
the native font and measure, clip and draw with the same metrics.

| Resource | Mapped characters | Bitmap bytes |
| --- | ---: | ---: |
| Source Han Sans SC Normal 12px | 7542 | 112483 |
| Source Han Sans SC Normal 20px | 7542 | 309436 |
| Source Han Sans SC Normal 24px | 7542 | 446623 |
| Noto Sans Bold 24px | 95 ASCII | 2188 |
| Noto Sans Bold 32px | 95 ASCII | 3903 |
| Noto Sans Bold 48px | 95 ASCII | 8737 |

Each Chinese font includes all 6763 GB2312 Han characters, ASCII and supplementary
symbols. Bitmaps total 883370 bytes; descriptors and maps are additional. The
linker determines the application size. The existing native 16px primary font
remains available and is not assumed to have the same source or metrics.

The W logo is a 28px mask generated from the browser's eight SVG paths in
`main/ui/assets/brand/w-logo.svg`; it accompanies the text `rongNoteBook`.

## Sources and regeneration

Source fonts are already present in the reference checkout. No new source fonts
were downloaded. SHA-256 values are pinned in the generator and manifest:

- `SourceHanSansSC-Normal.otf`:
  `1ee89e1669362dee13851129c0a8a791a87521eb4148e5efbf5d26596738e25b`
- `NotoSans-Bold.ttf`:
  `13a813c49624ae3ba3c5c6e72c5ebffc4b9e1e6ea32f421c04069b037c6ad431`

OFL notices are retained under `tools/ui/font-licenses/`. Regeneration requires
Pillow 11.3.0 and fonttools 4.67.0. The generator uses a 96/255 coverage threshold,
stores each glyph's advance and offsets, and validates source hashes before use.

```sh
python3 tools/ui/generate_ui_fonts.py \
  --source-han /path/to/SourceHanSansSC-Normal.otf \
  --noto-bold /path/to/NotoSans-Bold.ttf --check
```

Omit `--check` to regenerate `main/ui/ui_fonts.c`, the JSON manifest and logo
header. The source file paths default to the sibling reference projects in the
shared workspace. `main/ui/ui_fonts.h` declares the compiled fonts.

## Outstanding character and font work

- Native 16px English IPA / Latin extension glyphs still need integration in the
  word page. Noto Sans Regular / Bold sources are already local. Drawing and
  measuring must use the same merge or fallback strategy; combining marks need
  separate positioning checks.
- The word page still scales the legacy 16px headline. The compiled 32px Latin
  resource does not change that call site; migrate it with long-word handling.
- Dynamic AI, note and problem text can need characters beyond the static UI
  corpus. Check actual content before extending glyph sets. Current drawing
  normalizes some punctuation and skips missing glyphs; static coverage is not
  proof of complete dynamic-text or mathematical layout support.
- Source Han Sans Medium remains an optional download candidate for a later
  title-weight comparison. Normal is a distinct weight, not a substitute file
  bearing a different name. No new icon font or full 48px Chinese font is needed
  by the approved pages.
- The Home design's former 14px use is consolidated into native 16px; 20px now
  has a native resource. Do not generate other sizes by runtime bitmap scaling.

The ongoing Chinese inventory, exact static character lists and scan snapshots
remain in `/home/unknow/projects/doc/1017-firmware-font-audit.md` and its linked
`assets/font-audit/scan.json`, outside the firmware Git repository. This file
keeps the resource status and remaining work reviewable in firmware history.
