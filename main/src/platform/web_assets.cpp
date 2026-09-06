#include "printdeck/platform/web_assets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace printdeck::platform {

extern const std::uint8_t web_index_start[] asm("_binary_index_html_gz_start");
extern const std::uint8_t web_index_end[] asm("_binary_index_html_gz_end");
extern const std::uint8_t web_localizations_js_start[] asm("_binary_localizations_js_gz_start");
extern const std::uint8_t web_localizations_js_end[] asm("_binary_localizations_js_gz_end");
extern const std::uint8_t web_world_map_svg_start[] asm("_binary_world_map_svg_gz_start");
extern const std::uint8_t web_world_map_svg_end[] asm("_binary_world_map_svg_gz_end");
extern const std::uint8_t web_reactions_js_start[] asm("_binary_reactions_bundle_js_gz_start");
extern const std::uint8_t web_reactions_js_end[] asm("_binary_reactions_bundle_js_gz_end");
extern const std::uint8_t web_brand_logos_start[] asm("_binary_brand_logos_json_gz_start");
extern const std::uint8_t web_brand_logos_end[] asm("_binary_brand_logos_json_gz_end");
#define PRINTDECK_REACTION_PREVIEW(symbol, id)                                      \
  extern const std::uint8_t symbol##_preview_start[] asm("_binary_" #symbol       \
                                                          "_webp_start");          \
  extern const std::uint8_t symbol##_preview_end[] asm("_binary_" #symbol         \
                                                        "_webp_end");
#include "../../generated/reaction-catalog/reaction_previews.inc"
#undef PRINTDECK_REACTION_PREVIEW

namespace {

struct ReactionPreviewDefinition {
  std::string_view id;
  const std::uint8_t* start;
  const std::uint8_t* end;
};

constexpr std::array<ReactionPreviewDefinition, PRINTDECK_REACTION_PREVIEW_COUNT>
    kReactionPreviews = {{
#define PRINTDECK_REACTION_PREVIEW(symbol, id) \
  {id, symbol##_preview_start, symbol##_preview_end},
#include "../../generated/reaction-catalog/reaction_previews.inc"
#undef PRINTDECK_REACTION_PREVIEW
}};

std::string_view embedded_binary(const std::uint8_t* start, const std::uint8_t* end) {
  return {reinterpret_cast<const char*>(start), static_cast<std::size_t>(end - start)};
}

}  // namespace

std::string_view web_config_page() {
  return embedded_binary(web_index_start, web_index_end);
}

std::string_view web_localizations_script() {
  return embedded_binary(web_localizations_js_start, web_localizations_js_end);
}

std::string_view world_map_svg() {
  return embedded_binary(web_world_map_svg_start, web_world_map_svg_end);
}

std::string_view reactions_script() {
  return embedded_binary(web_reactions_js_start, web_reactions_js_end);
}

std::string_view web_brand_logos_json() {
  return embedded_binary(web_brand_logos_start, web_brand_logos_end);
}

std::string_view reaction_set_preview(std::string_view id) {
  for (const auto& preview : kReactionPreviews) {
    if (preview.id == id) return embedded_binary(preview.start, preview.end);
  }
  return {};
}

}  // namespace printdeck::platform
