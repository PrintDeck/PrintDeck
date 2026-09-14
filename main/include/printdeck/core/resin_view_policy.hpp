#pragma once
#include <array>
#include <span>

namespace printdeck::core {
// Stable content indexes: status, details, bottom, normal, temperature, resin.
inline constexpr std::array<int, 6> kResinTelemetryPages{0, 1, 2, 3, 4, 5};
inline constexpr std::array<int, 4> kTinyMakerTelemetryPages{0, 2, 3, 5};
constexpr std::span<const int> resin_telemetry_pages(bool tinymaker) {
  return tinymaker ? std::span<const int>(kTinyMakerTelemetryPages)
                   : std::span<const int>(kResinTelemetryPages);
}
constexpr int resin_telemetry_index(bool tinymaker, int page) {
  const auto pages = resin_telemetry_pages(tinymaker);
  for (int i = 0; i < static_cast<int>(pages.size()); ++i)
    if (pages[i] == page) return i;
  return -1;
}
}  // namespace printdeck::core
