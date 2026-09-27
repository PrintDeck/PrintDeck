#pragma once
#include <cstdint>
#include <string_view>
#include <atomic>
#include <vector>
#include "printdeck/core/set_catalog.hpp"
#include "lvgl.h"

namespace printdeck::platform {
// Runs only in the catalog's core-0 worker, under its transfer lease.
void refresh_reaction_covers(const std::vector<core::DownloadableSet>& sets,
                             const std::atomic<bool>& cancelled, bool online);
// Core-0 flash-safe worker only. Live images keep their PSRAM owners.
std::size_t reclaim_reaction_cover_cache();
// Display lock required. Each image owns its downloaded pixels until deletion.
lv_obj_t* create_reaction_cover(lv_obj_t* parent, std::string_view id);
}
