#include "printdeck/platform/live_view_png.hpp"

#include <cstring>
#include "png.h"
#include "zlib.h"

namespace printdeck::platform {
namespace {
struct PngOutput {
  std::uint8_t* data;
  std::size_t capacity;
  std::size_t written;
};

void write_bytes(png_structp encoder, png_bytep data, png_size_t size) {
  auto* output = static_cast<PngOutput*>(png_get_io_ptr(encoder));
  if (size > output->capacity - output->written) {
    png_error(encoder, "Live View PNG capacity exceeded");
    return;
  }
  std::memcpy(output->data + output->written, data, size);
  output->written += size;
}
}  // namespace

bool encode_live_view_png(const std::uint8_t* pixels, std::size_t bytes,
                         unsigned width, unsigned height, std::size_t stride,
                         std::vector<std::uint8_t>& output,
                         bool (*checkpoint)(void*), void* context) {
  output.clear();
  if (pixels == nullptr || width == 0 || height == 0 || width > 466 ||
      height > 466 || stride < width * 4U || stride > bytes / height) return false;
  if (checkpoint != nullptr && !checkpoint(context)) return false;

  png_image bounds{};
  bounds.version = PNG_IMAGE_VERSION;
  bounds.width = width;
  bounds.height = height;
  bounds.format = PNG_FORMAT_BGRA;
  // Allocate once, before libpng's error boundary. No C++ objects are created
  // between setjmp and calls which can longjmp from the library.
  output.resize(PNG_IMAGE_PNG_SIZE_MAX(bounds));
  PngOutput target{output.data(), output.size(), 0};
  auto* encoder = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  auto* info = encoder == nullptr ? nullptr : png_create_info_struct(encoder);
  if (encoder == nullptr || info == nullptr) {
    if (encoder != nullptr) png_destroy_write_struct(&encoder, nullptr);
    output.clear();
    return false;
  }
  if (setjmp(png_jmpbuf(encoder))) {
    png_destroy_write_struct(&encoder, &info);
    output.clear();
    return false;
  }
  png_set_write_fn(encoder, &target, write_bytes, nullptr);
  png_set_IHDR(encoder, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
  png_set_sRGB(encoder, info, PNG_sRGB_INTENT_PERCEPTUAL);
  // Default DEFLATE match searches can monopolize the classic ESP32 for tens
  // of seconds on detailed reactions. RLE does not search a sliding window;
  // every frame takes bounded work per pixel, at the cost of larger PNGs.
  png_set_compression_level(encoder, 1);
  png_set_compression_strategy(encoder, Z_RLE);
  png_set_filter(encoder, PNG_FILTER_TYPE_BASE, PNG_FILTER_SUB);
  png_write_info(encoder, info);
  png_set_bgr(encoder);
  for (unsigned row = 0; row < height; ++row) {
    if (row % 8 == 0 && checkpoint != nullptr && !checkpoint(context)) {
      png_destroy_write_struct(&encoder, &info);
      output.clear();
      return false;
    }
    png_write_row(encoder, pixels + row * stride);
  }
  png_write_end(encoder, info);
  png_destroy_write_struct(&encoder, &info);
  output.resize(target.written);
  return true;
}
}  // namespace printdeck::platform
