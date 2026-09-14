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

// Application core, before creating display objects: Latin, terminal, small
// logos and the boot wordmark only. CJK is retained after its first actual use.
void initialize_embedded_resources();
// Safe in glyph callbacks: membership lookup and an atomic request only.
void request_embedded_cjk_glyph(std::uint32_t codepoint);
// Single application-core caller. Neither check touches LVGL objects or takes
// locks; failed preparation and blocked image stages are not publication work.
bool embedded_resources_need_preparation(std::uint64_t now_ms);
bool embedded_resources_need_publication();
// Single application-core caller, outside the display lock. Decode at most one
// large image and one requested CJK font; retain staged data if publication waits.
void prepare_requested_embedded_resources(std::uint64_t now_ms);
// Same application caller, now holding the display lock. Retire unused image
// cache entries and publish only complete verified resources. Never inflates.
void publish_prepared_embedded_resources();
EmbeddedFont embedded_latin_font();
EmbeddedFont embedded_cjk_font();
EmbeddedFont embedded_terminal_font();
const lv_image_dsc_t* embedded_small_brand_logo(std::string_view name);
// LVGL lock required. Images own bounded leases until deletion/source change;
// a small brand mask preserves geometry while its large version is requested.
// A null result permits the caller's text fallback. Only AMOLED uses large images.
lv_obj_t* create_embedded_large_brand_logo(lv_obj_t* parent, std::string_view name);
lv_obj_t* create_embedded_boot_logo(lv_obj_t* parent);
struct EmbeddedResourceUsage {
  std::size_t cjk_bytes = 0;
  std::size_t large_image_bytes = 0;
  std::size_t staged_bytes = 0;
  std::size_t image_leases = 0;
};
// Application core with display lock, intended for diagnostics/verification.
EmbeddedResourceUsage embedded_resource_usage();
}  // namespace printdeck::platform
