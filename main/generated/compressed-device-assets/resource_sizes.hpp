#pragma once
#include <cstddef>
#include <array>
namespace printdeck::platform::embedded_assets {
inline constexpr std::size_t kLatinFontBytes = 45612;
inline constexpr std::size_t kCjkFontBytes = 161760;
inline constexpr std::size_t kTerminalFontBytes = 6784;
inline constexpr std::size_t kSmallLogoBytes = 1216;
inline constexpr std::size_t kSmallLogosBytes = 20672;
inline constexpr std::array<const char*, 17> kSmallLogoNames{{
    "ankermake",
    "anycubic",
    "bambu",
    "creality",
    "elegoo",
    "flashforge",
    "lulzbot",
    "makerbot",
    "prusa",
    "qidi",
    "ratrig",
    "snapmaker",
    "sovol",
    "ultimaker",
    "voron",
    "uniformation",
    "tinymaker",
}};
inline constexpr std::size_t kLargeLogoBytes = 6336;
inline constexpr std::size_t kLargeLogosBytes = 107712;
inline constexpr std::array<const char*, 17> kLargeLogoNames{{
    "ankermake",
    "anycubic",
    "bambu",
    "creality",
    "elegoo",
    "flashforge",
    "lulzbot",
    "makerbot",
    "prusa",
    "qidi",
    "ratrig",
    "snapmaker",
    "sovol",
    "ultimaker",
    "voron",
    "uniformation",
    "tinymaker",
}};
inline constexpr std::size_t kBootLogoWidth = 260;
inline constexpr std::size_t kBootLogoHeight = 62;
inline constexpr std::size_t kBootLogoStride = 130;
inline constexpr std::size_t kBootLogoBytes = 8124;
inline constexpr std::size_t kAmoledImagesBytes = 115836;
}  // namespace printdeck::platform::embedded_assets
