#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace printdeck::platform {

// The checkpoint may yield to network/storage tasks or cancel an expired capture.
// Input is a composed BGRA framebuffer, independent of LVGL ownership.
bool encode_live_view_png(const std::uint8_t* pixels, std::size_t bytes,
                         unsigned width, unsigned height, std::size_t stride,
                         std::vector<std::uint8_t>& output,
                         bool (*checkpoint)(void*), void* context);

}  // namespace printdeck::platform
