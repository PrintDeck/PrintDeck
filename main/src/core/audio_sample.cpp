#include "printdeck/core/audio_sample.hpp"

#include <algorithm>
#include <array>

namespace printdeck::core {
namespace {

constexpr std::size_t kHeaderSize = 12;
constexpr std::array<std::int16_t, 89> kStepTable{
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
};
constexpr std::array<std::int8_t, 8> kIndexChange{-1, -1, -1, -1, 2, 4, 6, 8};

}  // namespace

bool AudioSampleDecoder::open(const std::uint8_t* data, std::size_t size) noexcept {
  remaining_ = 0;
  payload_ = nullptr;
  position_ = 0;
  if (data == nullptr || size < kHeaderSize || size > kMaximumAudioSampleBytes ||
      data[0] != 'P' || data[1] != 'D' || data[2] != 'I' || data[3] != 'A') {
    return false;
  }
  const std::uint32_t count = static_cast<std::uint32_t>(data[4]) |
      static_cast<std::uint32_t>(data[5]) << 8U |
      static_cast<std::uint32_t>(data[6]) << 16U |
      static_cast<std::uint32_t>(data[7]) << 24U;
  if (count == 0 || data[10] >= kStepTable.size() || data[11] != 0 ||
      size != kHeaderSize + static_cast<std::size_t>(count) / 2U) {
    return false;
  }
  const int initial = static_cast<int>(data[8]) | static_cast<int>(data[9]) << 8;
  predictor_ = initial >= 0x8000 ? initial - 0x10000 : initial;
  step_index_ = data[10];
  remaining_ = count;
  payload_ = data + kHeaderSize;
  return true;
}

std::size_t AudioSampleDecoder::read(std::int16_t* output, std::size_t capacity) noexcept {
  if (output == nullptr) return 0;
  const std::size_t count = std::min<std::size_t>(remaining_, capacity);
  for (std::size_t index = 0; index < count; ++index) {
    if (position_ != 0) {
      const std::uint32_t code_index = position_ - 1;
      const std::uint8_t packed = payload_[code_index / 2U];
      const int code = (code_index & 1U) == 0 ? packed & 0x0F : packed >> 4U;
      const int step = kStepTable[static_cast<std::size_t>(step_index_)];
      int delta = step >> 3;
      if ((code & 4) != 0) delta += step;
      if ((code & 2) != 0) delta += step >> 1;
      if ((code & 1) != 0) delta += step >> 2;
      predictor_ = std::clamp(predictor_ + ((code & 8) != 0 ? -delta : delta),
                              -32768, 32767);
      step_index_ = std::clamp(
          step_index_ + kIndexChange[static_cast<std::size_t>(code & 7)], 0, 88);
    }
    output[index] = static_cast<std::int16_t>(predictor_);
    ++position_;
  }
  remaining_ -= static_cast<std::uint32_t>(count);
  return count;
}

}  // namespace printdeck::core
