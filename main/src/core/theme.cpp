#include "printdeck/core/theme.hpp"

namespace printdeck::core {
namespace {

std::uint32_t mix(std::uint32_t first, std::uint32_t second, unsigned second_weight) {
  const unsigned first_weight = 100U - second_weight;
  const auto channel = [&](unsigned shift) {
    const unsigned a = (first >> shift) & 0xFFU;
    const unsigned b = (second >> shift) & 0xFFU;
    return (a * first_weight + b * second_weight) / 100U;
  };
  return (channel(16) << 16U) | (channel(8) << 8U) | channel(0);
}

bool light_color(std::uint32_t color) {
  const unsigned red = (color >> 16U) & 0xFFU;
  const unsigned green = (color >> 8U) & 0xFFU;
  const unsigned blue = color & 0xFFU;
  return red * 299U + green * 587U + blue * 114U > 145000U;
}

}  // namespace

bool supported_theme(std::string_view id) {
  return id == "green" || id == "banana" || id == "sunset" || id == "ice" ||
         id == "cyberpunk" || id == "ember" || id == "mono" || id == "red" ||
         id == "ios_glass" || id == "fluent_dark" || id == "retro_terminal" ||
         id == "custom";
}

ThemeColors resolved_theme(std::string_view id, const ThemeColors& custom) {
  if (id == "custom") return custom;
  if (id == "banana") return {.printing = 0xF4EE2A, .done = 0x52D273, .error = 0xFF5C5C,
                             .idle = 0x5F6D72, .preparing = 0x13A8D8, .paused = 0x5F8DFF,
                             .filament = 0x19C6D3, .setup = 0x13A8D8,
                             .offline = 0xB8C4C8, .unknown = 0x78878C,
                             .background = 0x050600,
                             .preview_background = 0x0B2328};
  if (id == "sunset") return {.printing = 0xFF7A3D, .done = 0xFFD23F, .error = 0xFF4D6D,
                               .idle = 0x6B5B73, .preparing = 0xFF6B35, .paused = 0xC084FC,
                               .filament = 0xFDE047, .setup = 0xFB7185,
                               .offline = 0xE9D5FF, .unknown = 0x8B6F91,
                               .background = 0x15050B,
                               .preview_background = 0x3B193F};
  if (id == "ice") return {.printing = 0x0E7490, .done = 0x155E75, .error = 0xDC2626,
                            .idle = 0x64748B, .preparing = 0xBAE6FD, .paused = 0x38BDF8,
                            .filament = 0xA5F3FC, .setup = 0x7DD3FC,
                            .offline = 0x475569, .unknown = 0x64748B,
                            .background = 0xEAFBFF,
                            .preview_background = 0xDDF4FA};
  if (id == "cyberpunk") return {.printing = 0xFF2BD6, .done = 0x55F7FF,
                                  .error = 0xFF1744, .idle = 0x6D3B8C,
                                  .preparing = 0xFFE600, .paused = 0x8B5CF6,
                                  .filament = 0xFFD60A, .setup = 0xE879F9,
                                  .offline = 0x5B3A70, .unknown = 0x8B6A9E,
                                  .background = 0x100016,
                                  .preview_background = 0x17051F};
  if (id == "ember") return {.printing = 0x76B947, .done = 0xB7D56B, .error = 0xD96C54,
                              .idle = 0x596553, .preparing = 0xD4A84F, .paused = 0x8A7A58,
                              .filament = 0xE0B45B, .setup = 0x8FBF65,
                              .offline = 0xA8A89A, .unknown = 0x707566,
                              .background = 0x070B04,
                              .preview_background = 0x26311D};
  if (id == "mono") return {.printing = 0xF2F2F2, .done = 0xA8A8A8, .error = 0xFF3B30,
                             .idle = 0x666666, .preparing = 0xD9D9D9, .paused = 0xC7C7C7,
                             .filament = 0xD0D0D0, .setup = 0xB8B8B8,
                             .offline = 0x555555, .unknown = 0x808080,
                             .background = 0x000000,
                             .preview_background = 0x0A0A0A};
  if (id == "red") return {.printing = 0xE56B8A, .done = 0xF2B5C4, .error = 0xFF453A,
                            .idle = 0x74505C, .preparing = 0xD7A66A, .paused = 0xB678A8,
                            .filament = 0xE6B879, .setup = 0xC76D91,
                            .offline = 0x8D737B, .unknown = 0x80646D,
                            .background = 0x12050B,
                            .preview_background = 0x2B0D1B};
  if (id == "ios_glass") return {.printing = 0x0A84FF, .done = 0x64D2FF,
                                   .error = 0xFF453A, .idle = 0x8E8E93,
                                   .preparing = 0x64D2FF, .paused = 0xBF5AF2,
                                   .filament = 0xFF9F0A, .setup = 0x5E5CE6,
                                   .offline = 0x636366, .unknown = 0x48484A,
                                   .background = 0x000000,
                                   .preview_background = 0x101722};
  // Keep the legacy storage id so existing devices retain their selection,
  // but give it the completely new Red Dragon visual identity.
  if (id == "fluent_dark") return {.printing = 0xFF1616, .done = 0xD90909,
                                     .error = 0xFF3B30, .idle = 0x7A1B1B,
                                     .preparing = 0xFF4D00, .paused = 0xB90F2F,
                                     .filament = 0xFF6A00, .setup = 0xE31919,
                                     .offline = 0x705050, .unknown = 0x873737,
                                     .background = 0x020000,
                                     .preview_background = 0x0C0202};
  if (id == "retro_terminal") return {.printing = 0xFFB000, .done = 0x39FF88,
                                        .error = 0xFF4D3D, .idle = 0x8A7448,
                                        .preparing = 0xFFD166, .paused = 0xF2C94C,
                                        .filament = 0xFFCA3A, .setup = 0x65FF9A,
                                        .offline = 0x665C43, .unknown = 0x756749,
                                        .background = 0x000000,
                                        .preview_background = 0x061009};
  return {.printing = 0x00E5D4, .done = 0x6BFF6B, .error = 0xFF334E,
          .idle = 0x4A6B70, .preparing = 0x00B8FF, .paused = 0x4D8DFF,
          .filament = 0xFFE066, .setup = 0x00C2FF, .offline = 0xFFFFFF,
          .unknown = 0x7B9195, .background = 0x000000,
          .preview_background = 0x123A4A};
}

ThemeStyle resolved_theme_style(std::string_view id, const ThemeColors& colors) {
  if (id == "green") {
    return {.background = 0x000000, .background_secondary = 0x001419,
            .surface = 0x041012, .surface_raised = 0x081C20,
            .surface_soft = 0x0C292D, .border = 0x176C70, .track = 0x29494C,
            .text_primary = 0xF4FFFF, .text_secondary = 0xC8E8E8,
            .text_muted = 0x78999A, .accent = 0x00E5D4,
            .accent_secondary = 0x6BFF6B, .on_accent = 0x001412};
  }
  if (id == "banana") {
    return {.background = 0x050600, .background_secondary = 0x0A2025,
            .surface = 0x0B1010, .surface_raised = 0x101B1D,
            .surface_soft = 0x17272A, .border = 0x2E7181, .track = 0x43575B,
            .text_primary = 0xFFFFE8, .text_secondary = 0xD5E8E8,
            .text_muted = 0x80989D, .accent = 0xF4EE2A,
            .accent_secondary = 0x13A8D8, .on_accent = 0x171700};
  }
  if (id == "sunset") {
    return {.background = 0x15050B, .background_secondary = 0x3A0E1B,
            .surface = 0x230A12, .surface_raised = 0x35101B,
            .surface_soft = 0x481724, .border = 0x8A3A2D, .track = 0x704239,
            .text_primary = 0xFFF8F1, .text_secondary = 0xF2D1C3,
            .text_muted = 0xB28A83, .accent = 0xFF7A3D,
            .accent_secondary = 0xFFD23F, .on_accent = 0x1A0902};
  }
  if (id == "ice") {
    return {.background = 0xEAFBFF, .background_secondary = 0xC9EEF7,
            .surface = 0xF7FDFF, .surface_raised = 0xDDF4FA,
            .surface_soft = 0xC9EAF2, .border = 0x79BDCC, .track = 0x9DCBD5,
            .text_primary = 0x092733, .text_secondary = 0x234B59,
            .text_muted = 0x557A86, .accent = 0x0E7490,
            .accent_secondary = 0x155E75, .on_accent = 0xF5FDFF};
  }
  if (id == "cyberpunk") {
    return {.background = 0x100016, .background_secondary = 0x35083F,
            .surface = 0x21052A, .surface_raised = 0x350840,
            .surface_soft = 0x490C57, .border = 0x7C2C82, .track = 0x63345F,
            .text_primary = 0xFFF4FE, .text_secondary = 0xEACDF1,
            .text_muted = 0xA17DA9, .accent = 0xFF2BD6,
            .accent_secondary = 0x55F7FF, .on_accent = 0x160012};
  }
  if (id == "ember") {
    return {.background = 0x070B04, .background_secondary = 0x15230D,
            .surface = 0x10170A, .surface_raised = 0x1A2511,
            .surface_soft = 0x253218, .border = 0x4D7039, .track = 0x425338,
            .text_primary = 0xF4F7E9, .text_secondary = 0xD2DCC0,
            .text_muted = 0x899677, .accent = 0x76B947,
            .accent_secondary = 0xB7D56B, .on_accent = 0x091204,
            .corner_radius = 18};
  }
  if (id == "mono") {
    return {.background = 0x000000, .background_secondary = 0x181818,
            .surface = 0x0B0B0B, .surface_raised = 0x151515,
            .surface_soft = 0x202020, .border = 0x484848, .track = 0x3A3A3A,
            .text_primary = 0xFFFFFF, .text_secondary = 0xD7D7D7,
            .text_muted = 0x8A8A8A, .accent = 0xF2F2F2,
            .accent_secondary = 0xA8A8A8, .on_accent = 0x050505};
  }
  if (id == "red") {
    return {.background = 0x12050B, .background_secondary = 0x340E1F,
            .surface = 0x200A14, .surface_raised = 0x31101F,
            .surface_soft = 0x43152B, .border = 0x8A3F58, .track = 0x653A49,
            .text_primary = 0xFFF5F8, .text_secondary = 0xF1CFD9,
            .text_muted = 0xB68493, .accent = 0xE56B8A,
            .accent_secondary = 0xF2B5C4, .on_accent = 0x21050C};
  }
  if (id == "ios_glass") {
    return {.background = 0x000000, .background_secondary = 0x07111E,
            .surface = 0x090B0F, .surface_raised = 0x141820,
            .surface_soft = 0x1C2230, .border = 0x3B4B63, .track = 0x263246,
            .text_primary = 0xFFFFFF, .text_secondary = 0xD8E2F0,
            .text_muted = 0x8794A8, .accent = 0x0A84FF,
            .accent_secondary = 0x64D2FF, .on_accent = 0xFFFFFF,
            .corner_radius = 18, .glass_effect = true};
  }
  if (id == "fluent_dark") {
    return {.background = 0x020000, .background_secondary = 0x180000,
            .surface = 0x070202, .surface_raised = 0x100304,
            .surface_soft = 0x1A0506, .border = 0x8E0B0B, .track = 0x3B0909,
            .text_primary = 0xFFF5F5, .text_secondary = 0xFFB3B3,
            .text_muted = 0xA65A5A, .accent = 0xFF1616,
            .accent_secondary = 0xD90909, .on_accent = 0x090000,
            .corner_radius = 2};
  }
  if (id == "retro_terminal") {
    return {.background = 0x000000, .background_secondary = 0x071008,
            .surface = 0x020602, .surface_raised = 0x071009,
            .surface_soft = 0x0C190E, .border = 0x8A6518, .track = 0x4B3A17,
            .text_primary = 0xFFD98A, .text_secondary = 0xC9A85E,
            .text_muted = 0x806D45, .accent = 0xFFB000,
            .accent_secondary = 0x39FF88, .on_accent = 0x090600,
            .corner_radius = 0, .terminal_typography = true};
  }
  if (id == "custom") {
    const std::uint32_t background = colors.background & 0xFFFFFFU;
    const bool light_background = light_color(background);
    const std::uint32_t contrast = light_background ? 0x050807U : 0xF8FAFCU;
    const std::uint32_t opposite = light_background ? 0x000000U : 0xFFFFFFU;
    return {.background = background,
            .background_secondary = mix(background, colors.printing,
                                        light_background ? 10U : 16U),
            .surface = mix(background, opposite, light_background ? 7U : 6U),
            .surface_raised = mix(background, opposite, light_background ? 13U : 11U),
            .surface_soft = mix(background, colors.printing, 18U),
            .border = mix(background, colors.printing, 42U),
            .track = mix(background, contrast, 28U),
            .text_primary = contrast,
            .text_secondary = mix(background, contrast, 82U),
            .text_muted = mix(background, contrast, 58U),
            .accent = colors.printing,
            .accent_secondary = colors.done,
            .on_accent = light_color(colors.printing) ? 0x050807U : 0xFFFFFFU};
  }
  return {.background = 0x000000, .background_secondary = 0x02120A};
}

std::uint32_t phase_color(const ThemeColors& colors, JobPhase phase, bool reachable) {
  if (!reachable) return colors.offline;
  switch (phase) {
    case JobPhase::printing: return colors.printing;
    case JobPhase::completed: return colors.done;
    case JobPhase::failed:
    case JobPhase::cancelled: return colors.error;
    case JobPhase::idle: return colors.idle;
    case JobPhase::preparing: return colors.preparing;
    case JobPhase::paused: return colors.paused;
    case JobPhase::unknown: return colors.unknown;
  }
  return colors.unknown;
}

}  // namespace printdeck::core
