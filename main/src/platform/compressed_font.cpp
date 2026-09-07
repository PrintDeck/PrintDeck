#include "printdeck/platform/compressed_font.h"
#include <mutex>

namespace {
// LVGL 9.5 keeps one RLE cursor in lv_global. PrintDeck has two software draw
// workers: serialize just bitmap decoding, while retaining parallel drawing.
std::mutex decoder_mutex;
}

extern "C" const void* printdeck_compressed_glyph_bitmap(
    lv_font_glyph_dsc_t* glyph, lv_draw_buf_t* buffer) {
  const std::lock_guard<std::mutex> lock(decoder_mutex);
  return lv_font_get_bitmap_fmt_txt(glyph, buffer);
}
