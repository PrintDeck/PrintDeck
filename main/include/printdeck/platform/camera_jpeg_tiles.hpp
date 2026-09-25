#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace printdeck::platform {

struct CameraJpegScale {
  unsigned shift = 0;
  unsigned width = 0;
  unsigned height = 0;
};

// The classic ESP32 ROM decoder can discard resolution during JPEG decode.
// Keep the largest dimension within 400 pixels, preserving the source aspect.
inline CameraJpegScale camera_jpeg_scale(unsigned width, unsigned height) {
  if (width == 0 || height == 0 || width > 4096 || height > 2160) return {};
  unsigned shift = 0;
  while (shift < 3 && ((width >> shift) > 400 || (height >> shift) > 400)) ++shift;
  width >>= shift;
  height >>= shift;
  if (width == 0 || height == 0 || width > 512 || height > 400) return {};
  return {shift, width, height};
}

inline bool write_camera_jpeg_tile(std::span<std::uint8_t> output,
                                   unsigned width, unsigned height,
                                   unsigned left, unsigned top,
                                   unsigned right, unsigned bottom,
                                   std::span<const std::uint8_t> rgb) {
  if (width == 0 || height == 0 || width > 512 || height > 400 ||
      left > right || top > bottom || right >= width || bottom >= height ||
      output.size() != static_cast<std::size_t>(width) * height * 2) return false;
  const auto tile_width = right - left + 1;
  const auto tile_height = bottom - top + 1;
  if (rgb.size() != static_cast<std::size_t>(tile_width) * tile_height * 3) return false;
  std::size_t source = 0;
  for (unsigned y = top; y <= bottom; ++y) {
    for (unsigned x = left; x <= right; ++x) {
      const std::uint16_t pixel = ((rgb[source] & 0xf8U) << 8) |
          ((rgb[source + 1] & 0xfcU) << 3) | (rgb[source + 2] >> 3);
      const auto destination = (static_cast<std::size_t>(y) * width + x) * 2;
      output[destination] = static_cast<std::uint8_t>(pixel);
      output[destination + 1] = static_cast<std::uint8_t>(pixel >> 8);
      source += 3;
    }
  }
  return true;
}

}  // namespace printdeck::platform
