// Font format type stubs: lv_font_fmt_txt_dsc_t and friends, used to parse the
// pre-baked SourceHanSansSC font blob. This is NOT LVGL -- the firmware does
// not use the LVGL widget toolkit; these structs only describe the font
// binary's layout (borrowed from LVGL's lv_font_fmt_txt format). Renamed from
// lvgl/lvgl.h to avoid implying an LVGL dependency.
// See docs/13-ui-design-language.md §13.1.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define LV_LVGL_H_INCLUDE_SIMPLE 1
#define LVGL_VERSION_MAJOR 8
#define LVGL_VERSION_MINOR 3
#define LV_VERSION_CHECK(major, minor, patch) 0
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_FONT_SUBPX_NONE 0
#define LV_ATTRIBUTE_LARGE_CONST

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _lv_font_t lv_font_t;

typedef struct {
    uint32_t bitmap_index : 20;
    uint32_t adv_w : 12;
    uint8_t box_w;
    uint8_t box_h;
    int8_t ofs_x;
    int8_t ofs_y;
} lv_font_fmt_txt_glyph_dsc_t;

typedef enum {
    LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL,
    LV_FONT_FMT_TXT_CMAP_SPARSE_FULL,
    LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY,
    LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,
} lv_font_fmt_txt_cmap_type_t;

typedef struct {
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    const uint16_t* unicode_list;
    const void* glyph_id_ofs_list;
    uint16_t list_length;
    lv_font_fmt_txt_cmap_type_t type;
} lv_font_fmt_txt_cmap_t;

typedef struct {
    uint32_t bitmap_index;
    uint32_t adv_w;
    uint16_t box_w;
    uint16_t box_h;
    int16_t ofs_x;
    int16_t ofs_y;
} lv_font_fmt_txt_glyph_dsc_large_t;

typedef struct {
    const uint8_t* glyph_bitmap;
    const lv_font_fmt_txt_glyph_dsc_t* glyph_dsc;
    const lv_font_fmt_txt_cmap_t* cmaps;
    const void* kern_dsc;
    uint16_t kern_scale;
    uint16_t cmap_num : 9;
    uint16_t bpp : 4;
    uint16_t kern_classes : 1;
    uint16_t bitmap_format : 2;
    void* cache;
} lv_font_fmt_txt_dsc_t;

typedef struct {
    uint32_t dummy;
} lv_font_fmt_txt_glyph_cache_t;

typedef struct {
    uint16_t adv_w;
    uint16_t box_w;
    uint16_t box_h;
    int16_t ofs_x;
    int16_t ofs_y;
} lv_font_glyph_dsc_t;

struct _lv_font_t {
    bool (*get_glyph_dsc)(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out, uint32_t unicode_letter,
                          uint32_t unicode_letter_next);
    const uint8_t* (*get_glyph_bitmap)(const lv_font_t* font, uint32_t unicode_letter);
    uint16_t line_height;
    uint16_t base_line;
    uint8_t subpx;
    int8_t underline_position;
    uint8_t underline_thickness;
    const lv_font_fmt_txt_dsc_t* dsc;
    const lv_font_t* fallback;
    void* user_data;
};

bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out, uint32_t unicode_letter,
                                   uint32_t unicode_letter_next);
const uint8_t* lv_font_get_bitmap_fmt_txt(const lv_font_t* font, uint32_t unicode_letter);

#ifdef __cplusplus
}
#endif
