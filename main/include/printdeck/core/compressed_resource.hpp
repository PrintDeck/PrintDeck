#pragma once

#include <cstddef>
#include <cstdint>

namespace printdeck::core {

// Decode one complete gzip member into an exactly sized, caller-owned buffer.
// The caller bounds both sizes and owns allocation policy (PSRAM on device).
// False means the output must not be used, including on CRC/length failure.
bool decompress_gzip_exact(const std::uint8_t* input, std::size_t input_size,
                           std::uint8_t* output, std::size_t output_size,
                           void* (*allocate)(std::size_t),
                           void (*release)(void*)) noexcept;

// Retain one range from a gzip resource without allocating its complete output.
// The complete member, total decoded length and CRC are still verified. The
// output is unusable on any failure, including corruption after the range.
bool decompress_gzip_slice(const std::uint8_t* input, std::size_t input_size,
                          std::size_t total_output_size, std::size_t offset,
                          std::uint8_t* output, std::size_t output_size,
                          void* (*allocate)(std::size_t),
                          void (*release)(void*)) noexcept;

}  // namespace printdeck::core
