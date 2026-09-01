#include "printdeck/platform/web_assets.hpp"

#include <cstddef>
#include <cstdint>

namespace printdeck::platform {

extern const std::uint8_t web_index_start[] asm("_binary_index_html_start");
extern const std::uint8_t web_index_end[] asm("_binary_index_html_end");
extern const std::uint8_t web_localizations_js_start[] asm("_binary_localizations_js_start");
extern const std::uint8_t web_localizations_js_end[] asm("_binary_localizations_js_end");
extern const std::uint8_t web_world_map_svg_start[] asm("_binary_world_map_svg_start");
extern const std::uint8_t web_world_map_svg_end[] asm("_binary_world_map_svg_end");
extern const std::uint8_t web_reactions_js_start[] asm("_binary_reactions_bundle_js_start");
extern const std::uint8_t web_reactions_js_end[] asm("_binary_reactions_bundle_js_end");
extern const std::uint8_t green_preview_start[] asm("_binary_alloy_iris_green_webp_start");
extern const std::uint8_t green_preview_end[] asm("_binary_alloy_iris_green_webp_end");
extern const std::uint8_t blue_preview_start[] asm("_binary_alloy_iris_blue_webp_start");
extern const std::uint8_t blue_preview_end[] asm("_binary_alloy_iris_blue_webp_end");
extern const std::uint8_t brown_preview_start[] asm("_binary_alloy_iris_brown_webp_start");
extern const std::uint8_t brown_preview_end[] asm("_binary_alloy_iris_brown_webp_end");
extern const std::uint8_t amber_preview_start[] asm("_binary_alloy_iris_amber_webp_start");
extern const std::uint8_t amber_preview_end[] asm("_binary_alloy_iris_amber_webp_end");
extern const std::uint8_t gray_preview_start[] asm("_binary_alloy_iris_gray_webp_start");
extern const std::uint8_t gray_preview_end[] asm("_binary_alloy_iris_gray_webp_end");
extern const std::uint8_t hazel_preview_start[] asm("_binary_alloy_iris_hazel_webp_start");
extern const std::uint8_t hazel_preview_end[] asm("_binary_alloy_iris_hazel_webp_end");
extern const std::uint8_t red_preview_start[] asm("_binary_alloy_iris_red_webp_start");
extern const std::uint8_t red_preview_end[] asm("_binary_alloy_iris_red_webp_end");
extern const std::uint8_t violet_preview_start[] asm("_binary_alloy_iris_violet_webp_start");
extern const std::uint8_t violet_preview_end[] asm("_binary_alloy_iris_violet_webp_end");
extern const std::uint8_t cyan_preview_start[] asm("_binary_alloy_iris_cyan_webp_start");
extern const std::uint8_t cyan_preview_end[] asm("_binary_alloy_iris_cyan_webp_end");

namespace {

std::string_view embedded_text(const std::uint8_t* start, const std::uint8_t* end) {
  std::size_t size = static_cast<std::size_t>(end - start);
  if (size > 0 && start[size - 1] == 0) {
    --size;
  }
  return {reinterpret_cast<const char*>(start), size};
}

std::string_view embedded_binary(const std::uint8_t* start, const std::uint8_t* end) {
  return {reinterpret_cast<const char*>(start), static_cast<std::size_t>(end - start)};
}

}  // namespace

std::string_view web_config_page() {
  return embedded_text(web_index_start, web_index_end);
}

std::string_view web_localizations_script() {
  return embedded_text(web_localizations_js_start, web_localizations_js_end);
}

std::string_view world_map_svg() {
  return embedded_text(web_world_map_svg_start, web_world_map_svg_end);
}

std::string_view reactions_script() {
  return embedded_binary(web_reactions_js_start, web_reactions_js_end);
}

std::string_view reaction_set_preview(std::string_view id) {
  if (id == "alloy_iris_green") return embedded_binary(green_preview_start, green_preview_end);
  if (id == "alloy_iris_blue") return embedded_binary(blue_preview_start, blue_preview_end);
  if (id == "alloy_iris_brown") return embedded_binary(brown_preview_start, brown_preview_end);
  if (id == "alloy_iris_amber") return embedded_binary(amber_preview_start, amber_preview_end);
  if (id == "alloy_iris_gray") return embedded_binary(gray_preview_start, gray_preview_end);
  if (id == "alloy_iris_hazel") return embedded_binary(hazel_preview_start, hazel_preview_end);
  if (id == "alloy_iris_red") return embedded_binary(red_preview_start, red_preview_end);
  if (id == "alloy_iris_violet") return embedded_binary(violet_preview_start, violet_preview_end);
  if (id == "alloy_iris_cyan") return embedded_binary(cyan_preview_start, cyan_preview_end);
  return {};
}

}  // namespace printdeck::platform
