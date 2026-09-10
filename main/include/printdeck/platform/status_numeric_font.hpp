#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>

#include "lvgl.h"

namespace printdeck::platform {

// Prefer the largest candidate that fits the whole numeric field. Reserving
// the widest digit keeps the size stable while a timer or layer counter ticks.
inline const lv_font_t* status_numeric_font(
    const char* text, int width, int height, int letter_space,
    std::initializer_list<const lv_font_t*> candidates) {
  const lv_font_t* selected = nullptr;
  for (const auto* font : candidates) {
    selected = font;
    std::uint16_t digit_width = 0;
    for (char digit = '0'; digit <= '9'; ++digit)
      digit_width = std::max(digit_width, lv_font_get_glyph_width(font, digit, 0));
    int text_width = 0;
    for (const char* c = text; *c; ++c) {
      text_width += (*c >= '0' && *c <= '9')
                        ? digit_width : lv_font_get_glyph_width(font, *c, 0);
      if (c != text) text_width += letter_space;
    }
    if (text_width <= width && font->line_height <= height) break;
  }
  return selected;
}

}  // namespace printdeck::platform
