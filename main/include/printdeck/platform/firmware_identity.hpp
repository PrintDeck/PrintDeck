#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <string>

namespace printdeck::platform {
std::string_view firmware_layout();
std::string firmware_asset_family();
std::string firmware_image_version(std::span<const std::uint8_t> header);
bool compatible_firmware_image(std::span<const std::uint8_t> header);
inline constexpr char kFactoryInstallDetail[] =
    "A USB factory installation is required. Back up your configuration and follow the instructions at printdeck.xyz/firmware/. Saved settings and custom images will be erased.";
}  // namespace printdeck::platform
