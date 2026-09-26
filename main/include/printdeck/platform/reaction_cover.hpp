#pragma once
#include <cstdint>
#include <string_view>
#include "lvgl.h"

namespace printdeck::platform {
// Display lock required. A single visible cover owns the bounded preview slot.
lv_obj_t* create_reaction_cover(lv_obj_t* parent, std::string_view id);
void release_reaction_cover();
// Application core; inflate outside the display lock, publish while holding it.
bool reaction_cover_work_pending(std::uint64_t now_ms);
void prepare_reaction_cover(std::uint64_t now_ms);
void publish_reaction_cover();
}
