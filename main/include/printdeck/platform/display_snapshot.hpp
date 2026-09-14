#pragma once

#include "lvgl.h"

namespace printdeck::platform {

// Caller holds the display lock. Composes the active, top and system layers
// directly into one ARGB buffer; caller destroys it with lv_draw_buf_destroy.
lv_draw_buf_t* capture_composed_display(lv_display_t* display);

}  // namespace printdeck::platform
