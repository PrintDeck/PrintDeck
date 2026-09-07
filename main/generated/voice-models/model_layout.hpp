#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace printdeck::generated {
struct VoiceModelRecord {
  const char* name;
  std::uint32_t offset, packed_bytes, raw_bytes, crc32;
  std::array<const char*, 3> files;
};
inline constexpr std::array<std::uint8_t, 8> kVoiceModelMagic{0x50, 0x44, 0x56, 0x4d, 1, 0, 0, 0};
inline constexpr std::size_t kVoiceModelImageBytes = 1726308;
inline constexpr std::array<VoiceModelRecord, 2> kVoiceModels{{
  {"wn9_hiesp", 16, 183741, 291168, 0x327c69d4U, {"_MODEL_INFO_", "wn9_data", "wn9_index"}},
  {"mn5q8_en", 183760, 1542548, 2177232, 0xbf66f256U, {"_MODEL_INFO_", "mn5q8_data", "mn5q8_index"}},
}};
}  // namespace printdeck::generated
