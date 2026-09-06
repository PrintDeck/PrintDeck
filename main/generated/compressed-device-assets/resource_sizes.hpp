#pragma once
#include <cstddef>
#include <array>
namespace printdeck::platform::embedded_assets {
inline constexpr std::size_t kLatinFontBytes = 48216;
inline constexpr std::size_t kCjkFontBytes = 146724;
inline constexpr std::size_t kTerminalFontBytes = 6784;
inline constexpr std::size_t kSmallLogoBytes = 1216;
inline constexpr std::size_t kSmallLogosBytes = 18240;
inline constexpr std::array<const char*, 15> kSmallLogoNames{{
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
}};
inline constexpr std::size_t kLargeLogoBytes = 6336;
inline constexpr std::size_t kLargeLogosBytes = 95040;
inline constexpr std::array<const char*, 15> kLargeLogoNames{{
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
}};
inline constexpr std::size_t kBootLogoWidth = 260;
inline constexpr std::size_t kBootLogoHeight = 62;
inline constexpr std::size_t kBootLogoStride = 130;
inline constexpr std::size_t kBootLogoBytes = 8124;
inline constexpr std::size_t kAmoledImagesBytes = 103164;
}  // namespace printdeck::platform::embedded_assets
