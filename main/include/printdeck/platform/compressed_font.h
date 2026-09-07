#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
const void* printdeck_compressed_glyph_bitmap(lv_font_glyph_dsc_t* glyph,
                                            lv_draw_buf_t* buffer);
#ifdef __cplusplus
}
#endif
