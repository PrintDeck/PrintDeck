#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include "lvgl.h"

namespace printdeck::platform {
struct EmbeddedFont {
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

// Called on the application core before display objects are created.
// Successfully decoded resources live for the lifetime of the application.
void initialize_embedded_resources();
EmbeddedFont embedded_latin_font();
EmbeddedFont embedded_cjk_font();
EmbeddedFont embedded_terminal_font();
const lv_image_dsc_t* embedded_small_brand_logo(std::string_view name);
// Large images are available only on AMOLED, after successful initialization.
// A null result selects the caller's text fallback.
const lv_image_dsc_t* embedded_large_brand_logo(std::string_view name);
const lv_image_dsc_t* embedded_boot_logo();
}  // namespace printdeck::platform
