#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace printdeck::core {
// Immediately follows ESP-IDF's 256-byte app descriptor in the first segment.
struct FirmwareIdentity {
  char magic[16];
  char target[16];
  char layout[65];
  char reserved[3];
};
inline constexpr std::size_t kFirmwareIdentityOffset = 24 + 8 + 256;
inline constexpr std::size_t kFirmwareIdentityHeaderBytes =
    kFirmwareIdentityOffset + sizeof(FirmwareIdentity);
inline constexpr char kFirmwareIdentityMagic[] = "PrintDeck OTA 1";

inline bool compatible_firmware_header(std::span<const std::uint8_t> image,
                                      std::string_view target,
                                      std::string_view layout) {
  if (image.size() < kFirmwareIdentityHeaderBytes || image[0] != 0xe9 ||
      image[1] == 0 || image[1] > 16 || image[12] != 9 || image[13] != 0 ||
      image[32] != 0x32 || image[33] != 0x54 || image[34] != 0xcd || image[35] != 0xab)
    return false;
  std::uint32_t segment_size = 0;
  for (unsigned i = 0; i < 4; ++i) segment_size |= std::uint32_t(image[28 + i]) << (i * 8);
  if (segment_size < 256 + sizeof(FirmwareIdentity)) return false;
  FirmwareIdentity identity{};
  std::memcpy(&identity, image.data() + kFirmwareIdentityOffset, sizeof(identity));
  return std::string_view(identity.magic, strnlen(identity.magic, sizeof(identity.magic))) ==
             kFirmwareIdentityMagic &&
         std::string_view(identity.target, strnlen(identity.target, sizeof(identity.target))) == target &&
         std::string_view(identity.layout, strnlen(identity.layout, sizeof(identity.layout))) == layout;
}
}  // namespace printdeck::core
