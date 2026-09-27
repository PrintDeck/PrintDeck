#include "printdeck/platform/reaction_cover.hpp"
#include <array>

namespace printdeck::platform {
namespace {
struct CoverSource { const char* id; const std::uint8_t* data; std::size_t size; };
#include "../../generated/reaction-catalog/reaction_device_previews.inc"

// Immutable indexed pixels live in flash. Selecting a cover neither inflates
// data nor waits for camera/capture workspaces or the application worker.
constexpr auto kDescriptors = [] {
  std::array<lv_image_dsc_t, std::size(kReactionCovers)> images{};
  for (std::size_t i = 0; i < images.size(); ++i) {
    auto& image = images[i];
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_I4;
    image.header.w = image.header.h = kReactionCoverSize;
    image.header.stride = kReactionCoverSize / 2;
    image.data_size = kReactionCovers[i].size;
    image.data = kReactionCovers[i].data;
  }
  return images;
}();
}

lv_obj_t* create_reaction_cover(lv_obj_t* parent, std::string_view id) {
  if (id == "family:iris_palette" || id.starts_with("alloy_iris_")) id = "alloy_iris_green";
  for (std::size_t i = 0; i < std::size(kReactionCovers); ++i) {
    if (id != kReactionCovers[i].id) continue;
    auto* image = lv_image_create(parent);
    lv_image_set_src(image, &kDescriptors[i]);
    return image;
  }
  return nullptr;
}
}
