#pragma once

#include <cstdint>

#include "lvgl.h"
#include "printdeck/core/job_state.hpp"

namespace printdeck::platform {

struct PrinterAnimationPalette {
  std::uint32_t background = 0;
  std::uint32_t primary = 0;
  std::uint32_t secondary = 0;
  std::uint32_t muted = 0;
  std::uint32_t filament = 0;
};

// Draws one complete frame into an RGB565 LVGL canvas.  The artwork is built
// from theme-coloured vector primitives so it stays crisp on both supported
// displays without carrying a separate set of bitmap animations per theme.
void render_printer_animation(lv_obj_t* canvas, core::PrinterActivity activity,
                              std::uint32_t frame,
                              const PrinterAnimationPalette& palette);

}  // namespace printdeck::platform
