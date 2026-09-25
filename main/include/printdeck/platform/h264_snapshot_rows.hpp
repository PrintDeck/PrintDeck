#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace printdeck::platform {

// Samples deblocked I420 rows directly into the display image. No full-size
// picture or intermediate RGB image is retained. One instance consumes one IDR.
template <std::uint16_t MaxWidth, std::uint16_t MaxHeight>
class H264SnapshotRows {
 public:
  explicit H264SnapshotRows(std::uint8_t* pixels) : pixels_(pixels) {}

  bool append(int row, int source_width, int source_height,
              const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v,
              int y_stride, int c_stride) {
    if (failed_) return false;
    if (!pixels_ || !y || !u || !v || row != next_row_ ||
        source_width < 2 || source_width > 1920 || source_height < 2 || source_height > 1088 ||
        (source_width & 1) || (source_height & 1) || y_stride < source_width ||
        c_stride < source_width / 2 || row >= (source_height + 15) / 16)
      return fail();
    if (row == 0) {
      source_width_ = source_width;
      source_height_ = source_height;
      width_ = MaxWidth;
      const auto scaled_height = static_cast<std::uint32_t>(MaxWidth) * source_height / source_width;
      height_ = std::min<std::uint32_t>(MaxHeight, scaled_height);
      if (scaled_height > MaxHeight) {
        height_ = MaxHeight;
        width_ = static_cast<std::uint32_t>(MaxHeight) * source_width / source_height;
      }
      width_ = std::max<std::uint16_t>(2, width_ & ~1U);
      height_ = std::max<std::uint16_t>(2, height_ & ~1U);
      for (unsigned x = 0; x < width_; ++x) source_x_[x] = x * source_width / width_;
    } else if (source_width != source_width_ || source_height != source_height_) {
      return fail();
    }
    while (next_y_ < height_) {
      const int sy = next_y_ * source_height_ / height_;
      if (sy >= (row + 1) * 16) break;
      if (sy < row * 16) return fail();
      const int local_y = sy - row * 16;
      for (unsigned x = 0; x < width_; ++x) {
        const unsigned sx = source_x_[x];
        const int c = std::max(0, static_cast<int>(y[local_y * y_stride + sx]) - 16);
        const int uu = static_cast<int>(u[(local_y / 2) * c_stride + sx / 2]) - 128;
        const int vv = static_cast<int>(v[(local_y / 2) * c_stride + sx / 2]) - 128;
        const int r = std::clamp((298 * c + 409 * vv + 128) >> 8, 0, 255);
        const int g = std::clamp((298 * c - 100 * uu - 208 * vv + 128) >> 8, 0, 255);
        const int b = std::clamp((298 * c + 516 * uu + 128) >> 8, 0, 255);
        const auto rgb = static_cast<std::uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
        const std::size_t offset = (next_y_ * width_ + x) * 2U;
        pixels_[offset] = rgb & 0xffU;
        pixels_[offset + 1] = rgb >> 8U;
      }
      ++next_y_;
    }
    ++next_row_;
    return true;
  }

  bool complete() const {
    return !failed_ && width_ && next_row_ == (source_height_ + 15) / 16 && next_y_ == height_;
  }
  std::uint16_t width() const { return width_; }
  std::uint16_t height() const { return height_; }
  std::size_t size() const { return static_cast<std::size_t>(width_) * height_ * 2; }

 private:
  bool fail() { failed_ = true; return false; }
  std::uint8_t* pixels_;
  std::array<std::uint16_t, MaxWidth> source_x_{};
  int source_width_ = 0, source_height_ = 0, next_row_ = 0;
  unsigned next_y_ = 0;
  std::uint16_t width_ = 0, height_ = 0;
  bool failed_ = false;
};

}  // namespace printdeck::platform
