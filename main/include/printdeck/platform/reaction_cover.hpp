#pragma once
#include <cstdint>
#include <string_view>
#include "lvgl.h"

namespace printdeck::platform {
// Display lock required. The descriptor and indexed cover remain in flash.
lv_obj_t* create_reaction_cover(lv_obj_t* parent, std::string_view id);
}
