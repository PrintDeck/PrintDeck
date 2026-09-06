#pragma once

#include <cstddef>
#include <cstdint>

namespace printdeck::core {

// At 16 kHz this permits just over eight seconds per embedded mono clip.
inline constexpr std::size_t kMaximumAudioSampleBytes = 65536;

class AudioSampleDecoder {
 public:
  bool open(const std::uint8_t* data, std::size_t size) noexcept;
  std::size_t read(std::int16_t* output, std::size_t capacity) noexcept;
  bool finished() const noexcept { return remaining_ == 0; }

 private:
  const std::uint8_t* payload_ = nullptr;
  std::uint32_t remaining_ = 0;
  std::uint32_t position_ = 0;
  int predictor_ = 0;
  int step_index_ = 0;
};

}  // namespace printdeck::core
