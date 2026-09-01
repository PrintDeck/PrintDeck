#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace printdeck::core {

struct FirmwareVersion {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;

  auto operator<=>(const FirmwareVersion&) const = default;
};

struct FirmwareChannel {
  std::string version;
  std::string url;
  std::array<std::uint8_t, 32> sha256{};
};

std::optional<FirmwareVersion> parse_firmware_version(std::string_view value);
std::optional<FirmwareChannel> parse_firmware_channel(
    std::string_view json, std::string_view expected_target);

}  // namespace printdeck::core
