#pragma once

#include <cstdint>
#include <string_view>

#include "printdeck/core/job_state.hpp"

namespace printdeck::core {

struct ThemeColors {
  std::uint32_t printing = 0xD946EF;
  std::uint32_t done = 0xA3E635;
  std::uint32_t error = 0xFF3366;
  std::uint32_t idle = 0x5B416B;
  std::uint32_t preparing = 0xFDE047;
  std::uint32_t paused = 0x8B5CF6;
  std::uint32_t filament = 0xFACC15;
  std::uint32_t setup = 0xE879F9;
  std::uint32_t offline = 0xE9D5FF;
  std::uint32_t unknown = 0x7E6A89;
  std::uint32_t background = 0x050807;
  std::uint32_t preview_background = 0x24102F;
};

// Non-persistent visual tokens derived from the selected theme. ThemeColors is
// intentionally kept stable because it is part of the settings schema; these
// tokens give the complete UI a coherent palette without expanding NVS data.
struct ThemeStyle {
  std::uint32_t background = 0x020604;
  std::uint32_t background_secondary = 0x02120A;
  std::uint32_t surface = 0x07110D;
  std::uint32_t surface_raised = 0x0B1A14;
  std::uint32_t surface_soft = 0x10231A;
  std::uint32_t border = 0x23483A;
  std::uint32_t track = 0x335A4B;
  std::uint32_t text_primary = 0xF5FFF9;
  std::uint32_t text_secondary = 0xCDE7D9;
  std::uint32_t text_muted = 0x7F9D8E;
  std::uint32_t accent = 0x00FF66;
  std::uint32_t accent_secondary = 0x19F7E6;
  std::uint32_t on_accent = 0x02120A;
  // Geometry and typography are non-persistent theme traits. They let a
  // theme change the product's visual character without changing layout or
  // the information hierarchy.
  std::uint8_t corner_radius = 12;
  bool terminal_typography = false;
  bool glass_effect = false;
};

bool supported_theme(std::string_view id);
ThemeColors resolved_theme(std::string_view id, const ThemeColors& custom);
ThemeStyle resolved_theme_style(std::string_view id, const ThemeColors& colors);
std::uint32_t phase_color(const ThemeColors& colors, JobPhase phase, bool reachable);

}  // namespace printdeck::core
