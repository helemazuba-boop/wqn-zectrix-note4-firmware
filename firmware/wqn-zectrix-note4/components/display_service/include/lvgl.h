// LVGL font-format shim.
//
// The firmware does NOT use the LVGL widget toolkit. We only borrow LVGL's
// lv_font_fmt_txt binary layout to parse pre-baked font blobs. SourceHanSansSC
// includes font_fmt.h directly; the Zectrix icon fonts (font_zectrix_*.c) were
// emitted by LVGL's font converter with `#include "lvgl.h"`, which this shim
// redirects to our font_fmt.h struct definitions so they compile without LVGL.
//
// See font_fmt.h and docs/13-ui-design-language.md §13.1.
#pragma once
#include "font_fmt.h"
