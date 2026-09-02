#include "printdeck/platform/display_shell.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "printdeck/core/localization.hpp"
#include "printdeck/core/printer_driver.hpp"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/firmware_update_service.hpp"
#include "printdeck/platform/network_service.hpp"
#include "printdeck/platform/power_service.hpp"
#include "sdkconfig.h"

extern "C" {
extern const lv_font_t mdi_40;
}

namespace printdeck::platform {
namespace {

constexpr std::uint32_t kNozzleTemperatureColor = 0xFF7043;
constexpr std::uint32_t kBedTemperatureColor = 0xFFB020;
constexpr std::uint32_t kChamberTemperatureColor = 0xA78BFA;
constexpr char kMdiClock[] = "\xF3\xB1\x91\x8E";
constexpr char kMdiNozzle[] = "\xF3\xB0\xB9\x9B";
constexpr char kMdiBed[] = "\xF3\xB1\xA1\x9B";
constexpr int kDisplayBrightnessMinimum =
    kDisplayUsesCompactRoundLayout ? 5 : 10;

const char* theme_display_name(std::string_view id) {
  struct ThemeName {
    std::string_view id;
    const char* label;
  };
  static constexpr ThemeName names[]{
      {"green", "SIGNAL"}, {"banana", "BANANA"}, {"sunset", "SOLSTICE"},
      {"ice", "GLACIER"}, {"cyberpunk", "AURORA"}, {"ember", "GROVE"},
      {"mono", "GRAPHITE"}, {"red", "GARNET"},
      {"ios_glass", "MIDNIGHT HALO"}, {"fluent_dark", "DRAGON"},
      {"retro_terminal", "TERMINAL"}, {"custom", "CUSTOM"}};
  for (const ThemeName& name : names) {
    if (name.id == id) return name.label;
  }
  return "CUSTOM";
}

void square_route_screen_gestures(lv_obj_t* object, bool keep_clickable = false) {
  if (object == nullptr) return;
  if (!keep_clickable) lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

lv_obj_t* square_layout_box(lv_obj_t* parent, int width, int height) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_set_size(box, width, height);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(box, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
  square_route_screen_gestures(box);
  return box;
}

void square_create_battery_icon(lv_obj_t* parent, std::uint8_t percent,
                                std::uint32_t color, lv_obj_t*& outline,
                                lv_obj_t*& fill) {
  // Match the round icon's visual proportion, not its raw pixel dimensions:
  // the square panel is roughly half the resolution in each axis.
  lv_obj_t* root = square_layout_box(parent, 10, 16);
  lv_obj_align(root, LV_ALIGN_TOP_RIGHT, -45, 7);

  outline = lv_obj_create(root);
  lv_obj_set_size(outline, 7, 12);
  lv_obj_align(outline, LV_ALIGN_CENTER, 0, 1);
  lv_obj_set_style_radius(outline, 1, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(outline, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_border_width(outline, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_all(outline, 1, LV_PART_MAIN);
  square_route_screen_gestures(outline);

  lv_obj_t* terminal = lv_obj_create(root);
  lv_obj_set_size(terminal, 4, 2);
  lv_obj_align(terminal, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_radius(terminal, 1, LV_PART_MAIN);
  lv_obj_set_style_bg_color(terminal, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(terminal, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(terminal, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(terminal, 0, LV_PART_MAIN);
  square_route_screen_gestures(terminal);

  const int fill_height = std::max(1, 8 * static_cast<int>(percent) / 100);
  fill = lv_obj_create(outline);
  lv_obj_set_size(fill, 3, fill_height);
  lv_obj_align(fill, LV_ALIGN_CENTER, 0, 3 - fill_height / 2);
  lv_obj_set_style_radius(fill, 1, LV_PART_MAIN);
  lv_obj_set_style_bg_color(fill, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(fill, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(fill, 0, LV_PART_MAIN);
  square_route_screen_gestures(fill);
}

void square_align_battery_icon(lv_obj_t* outline, lv_obj_t* power_label) {
  if (outline == nullptr || power_label == nullptr ||
      !lv_obj_is_valid(outline) || !lv_obj_is_valid(power_label)) {
    return;
  }
  lv_obj_t* root = lv_obj_get_parent(outline);
  if (root == nullptr || !lv_obj_is_valid(root)) return;
  // The power label grows to the left at 100%. Keep the battery attached to
  // that moving edge so the complete battery/charge/percentage group moves as
  // one unit and its internal spacing never collapses.
  lv_obj_update_layout(power_label);
  lv_obj_align_to(root, power_label, LV_ALIGN_OUT_LEFT_MID, -4, -1);
}

void square_align_audio_icon(lv_obj_t* audio, lv_obj_t* outline,
                             lv_obj_t* power_label) {
  if (audio == nullptr || power_label == nullptr ||
      !lv_obj_is_valid(audio) || !lv_obj_is_valid(power_label)) {
    return;
  }
  lv_obj_update_layout(power_label);
  lv_obj_t* target = power_label;
  int gap = -5;
  int y = 0;
  if (outline != nullptr && lv_obj_is_valid(outline)) {
    lv_obj_t* root = lv_obj_get_parent(outline);
    if (root != nullptr && lv_obj_is_valid(root)) {
      target = root;
      y = 1;
    }
  }
  lv_obj_align_to(audio, target, LV_ALIGN_OUT_LEFT_MID, gap, y);
}

lv_obj_t* square_mdi_icon_slot(lv_obj_t* parent, const char* glyph,
                               int width, int height, std::uint32_t color,
                               lv_obj_t** icon_out = nullptr) {
  lv_obj_t* slot = square_layout_box(parent, width, height);
  // MDI glyphs are rendered from a 40 px font and scaled down. Allow their
  // transformed pixels to extend past the small layout slot instead of being
  // clipped by it.
  lv_obj_add_flag(slot, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_t* icon = lv_label_create(slot);
  lv_label_set_text(icon, glyph);
  lv_obj_set_style_text_font(icon, &mdi_40, LV_PART_MAIN);
  lv_obj_set_style_text_color(icon, lv_color_hex(color), LV_PART_MAIN);
  // The default transform pivot is the label's top-left corner. With a 40 px
  // source font that makes a downscaled glyph move outside its small slot and
  // leaves only one clipped edge visible. Resolve the label's natural size and
  // scale around its actual center instead.
  lv_obj_update_layout(icon);
  lv_obj_set_style_transform_pivot_x(icon, lv_obj_get_width(icon) / 2, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(icon, lv_obj_get_height(icon) / 2, LV_PART_MAIN);
  lv_obj_set_style_transform_scale(icon, 112, LV_PART_MAIN);
  lv_obj_center(icon);
  square_route_screen_gestures(icon);
  if (icon_out != nullptr) *icon_out = icon;
  return slot;
}

lv_obj_t* square_thermometer_slot(lv_obj_t* parent, int width, int height,
                                  std::uint32_t color) {
  lv_obj_t* slot = square_layout_box(parent, width, height);
  lv_obj_t* root = square_layout_box(slot, 14, 20);
  lv_obj_center(root);

  lv_obj_t* stem = lv_obj_create(root);
  lv_obj_set_size(stem, 6, 13);
  lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_radius(stem, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(stem, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(stem, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_border_width(stem, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(stem, 0, LV_PART_MAIN);
  square_route_screen_gestures(stem);

  lv_obj_t* bulb = lv_obj_create(root);
  lv_obj_set_size(bulb, 9, 9);
  lv_obj_align(bulb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_radius(bulb, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bulb, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bulb, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(bulb, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(bulb, 0, LV_PART_MAIN);
  square_route_screen_gestures(bulb);
  return slot;
}

std::string square_compact_update_text(std::string text) {
  std::size_t newline = 0;
  while ((newline = text.find('\n', newline)) != std::string::npos) {
    text.replace(newline, 1, " - ");
    newline += 3;
  }
  return text;
}

std::string short_duration(std::uint32_t seconds) {
  char text[24]{};
  const unsigned hours = seconds / 3600U;
  const unsigned minutes = (seconds % 3600U) / 60U;
  if (hours > 0) std::snprintf(text, sizeof(text), "%uh %02um", hours, minutes);
  else std::snprintf(text, sizeof(text), "%um", minutes);
  return text;
}

const char* square_link_label(core::LinkState state) {
  switch (state) {
    case core::LinkState::stopped: return "Not selected";
    case core::LinkState::waiting_for_network: return "Waiting for network";
    case core::LinkState::connecting: return "Connecting";
    case core::LinkState::online: return "Online";
    case core::LinkState::failed: return "Connection failed";
  }
  return "Unavailable";
}

}  // namespace

void DisplayShell::square_create_initial_screen() {
  prepare_active_screen("boot-status");
  lv_obj_t* screen = lv_screen_active();
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_PRESSED, this);
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_PRESSING, this);
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_RELEASED, this);
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_PRESS_LOST, this);
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_LONG_PRESSED, this);
  last_activity_ms_ = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  lv_obj_t* frame = lv_obj_create(screen);
  lv_obj_set_size(frame, kDisplayUsesCompactRoundLayout ? 188 : 214,
                         kDisplayUsesCompactRoundLayout ? 188 : 174);
  lv_obj_center(frame);
  lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(frame,
                          kDisplayUsesCompactRoundLayout ? LV_RADIUS_CIRCLE
                                                        : themed_radius(22),
                          LV_PART_MAIN);
  lv_obj_set_style_bg_color(frame, lv_color_hex(theme_style_.surface), LV_PART_MAIN);
  lv_obj_set_style_border_width(frame, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(frame, lv_color_hex(accent_color_), LV_PART_MAIN);
  lv_obj_set_style_pad_all(frame, 0, LV_PART_MAIN);
  apply_surface_effect(frame);
  lv_obj_t* title = lv_label_create(frame);
  lv_label_set_text(title, "PrintDeck");
  apply_text_style(title, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_24);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 30 : 26);
  status_label_ = lv_label_create(frame);
  lv_label_set_text(status_label_, tr("Starting device services"));
  apply_text_style(status_label_, lv_color_hex(accent_color_),
                   kDisplayUsesCompactRoundLayout ? &lv_font_montserrat_12
                                                  : &lv_font_montserrat_14);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    lv_obj_set_size(status_label_, 158, 48);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 11);
  } else {
    lv_obj_set_width(status_label_, 190);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 16);
  }
  lv_obj_t* version = lv_label_create(frame);
  lv_label_set_text_fmt(version, "%s %s", tr("Version"), PRINTDECK_VERSION);
  apply_text_style(version, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
  lv_obj_align(version, LV_ALIGN_BOTTOM_MID, 0,
               kDisplayUsesCompactRoundLayout ? -27 : -22);
  lv_obj_invalidate(screen);
}

void DisplayShell::square_create_header(const char* title, const PowerSnapshot* power) {
  lv_obj_t* heading = lv_label_create(lv_screen_active());
  lv_label_set_text(heading, tr(title));
  apply_text_style(heading, lv_color_hex(accent_color_),
                   kDisplayUsesCompactRoundLayout ? &lv_font_montserrat_14
                                                  : &lv_font_montserrat_16);
  lv_obj_set_size(heading, kDisplayUsesCompactRoundLayout ? 140 : 150, 20);
  lv_obj_set_style_text_align(
      heading, kDisplayUsesCompactRoundLayout ? LV_TEXT_ALIGN_CENTER
                                              : LV_TEXT_ALIGN_LEFT,
      LV_PART_MAIN);
  lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 14);
  } else {
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 7, 7);
  }
  active_accent_label_ = heading;
  active_accent_text_objects_.push_back(heading);

  // KNOMI2 has no battery/power telemetry or audio hardware. Keeping those
  // placeholders in the narrow top corners only pushes useful titles outside
  // the circular panel's safe area.
  if constexpr (kDisplayUsesCompactRoundLayout) {
    header_audio_label_ = nullptr;
    header_power_label_ = nullptr;
    header_battery_outline_ = nullptr;
    header_battery_fill_ = nullptr;
    return;
  }

  header_audio_label_ = lv_label_create(lv_screen_active());
  lv_label_set_text(header_audio_label_,
                    audio_enabled_ ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
  apply_icon_text_style(header_audio_label_, lv_color_hex(theme_style_.text_muted),
                        &lv_font_montserrat_12);

  header_power_label_ = lv_label_create(lv_screen_active());
  apply_text_style(header_power_label_, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_12);
  // Keep the longest state (charge symbol + 100%) on one line.
  lv_obj_set_width(header_power_label_, LV_SIZE_CONTENT);
  lv_obj_set_style_text_align(header_power_label_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_align(header_power_label_, LV_ALIGN_TOP_RIGHT, -5, 9);
  header_battery_outline_ = nullptr;
  header_battery_fill_ = nullptr;
  if (power != nullptr && power->available && power->battery_present) {
    square_create_battery_icon(lv_screen_active(), power->battery_percent,
                               theme_style_.text_primary,
                               header_battery_outline_, header_battery_fill_);
    square_update_power_header(*power);
  } else {
    lv_label_set_text(header_power_label_, LV_SYMBOL_USB);
    square_align_audio_icon(header_audio_label_, nullptr, header_power_label_);
  }
}

void DisplayShell::square_update_power_header(const PowerSnapshot& power) {
  if (header_power_label_ == nullptr || !power.available) return;
  if (!power.battery_present) {
    if (header_battery_outline_ != nullptr && lv_obj_is_valid(header_battery_outline_)) {
      lv_obj_t* root = lv_obj_get_parent(header_battery_outline_);
      if (root != nullptr && lv_obj_is_valid(root)) lv_obj_delete(root);
    }
    header_battery_outline_ = nullptr;
    header_battery_fill_ = nullptr;
    lv_label_set_text(header_power_label_, LV_SYMBOL_USB);
    lv_obj_set_style_text_color(header_power_label_, lv_color_hex(theme_style_.text_primary), LV_PART_MAIN);
    square_align_audio_icon(header_audio_label_, nullptr, header_power_label_);
    return;
  }
  if (header_battery_outline_ == nullptr || !lv_obj_is_valid(header_battery_outline_)) {
    square_create_battery_icon(lv_screen_active(), power.battery_percent,
                               theme_style_.text_primary,
                               header_battery_outline_, header_battery_fill_);
  }
  const bool externally_powered = power.usb_present || power.charging;
  const std::uint32_t color = theme_style_.text_primary;
  if (externally_powered) {
    lv_label_set_text_fmt(header_power_label_, LV_SYMBOL_CHARGE " %u%%",
                          static_cast<unsigned>(power.battery_percent));
  } else {
    lv_label_set_text_fmt(header_power_label_, "%u%%",
                          static_cast<unsigned>(power.battery_percent));
  }
  lv_obj_set_style_text_color(header_power_label_, lv_color_hex(color), LV_PART_MAIN);
  square_align_battery_icon(header_battery_outline_, header_power_label_);
  square_align_audio_icon(header_audio_label_, header_battery_outline_,
                          header_power_label_);
  if (header_battery_outline_ != nullptr) {
    lv_obj_set_style_border_color(header_battery_outline_, lv_color_hex(color), LV_PART_MAIN);
  }
  if (header_battery_fill_ != nullptr) {
    const int fill_height =
        std::max(1, 8 * static_cast<int>(power.battery_percent) / 100);
    lv_obj_set_size(header_battery_fill_, 3, fill_height);
    lv_obj_align(header_battery_fill_, LV_ALIGN_CENTER, 0, 3 - fill_height / 2);
    lv_obj_set_style_bg_color(header_battery_fill_, lv_color_hex(color), LV_PART_MAIN);
  }
}

void DisplayShell::square_create_printer_chrome(const core::PrinterProfile& profile,
                                                 const core::PrinterSnapshot& snapshot,
                                                 const PowerSnapshot* power) {
  lv_obj_t* screen = lv_screen_active();
  if constexpr (!kDisplayUsesCompactRoundLayout) {
    header_audio_label_ = lv_label_create(screen);
    lv_label_set_text(header_audio_label_,
                      audio_enabled_ ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    apply_icon_text_style(header_audio_label_, lv_color_hex(theme_style_.text_muted),
                          &lv_font_montserrat_12);
  }

  progress_label_ = lv_label_create(screen);
  lv_label_set_text_fmt(progress_label_, "%d%%",
                        std::clamp(static_cast<int>(snapshot.job.completion), 0, 100));
  apply_text_style(progress_label_, lv_color_hex(accent_color_), &lv_font_montserrat_12);
  lv_obj_set_width(progress_label_, kDisplayUsesCompactRoundLayout ? 32 : 40);
  lv_obj_set_style_text_align(progress_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(progress_label_, LV_ALIGN_TOP_LEFT,
               kDisplayUsesCompactRoundLayout ? 38 : 2,
               kDisplayUsesCompactRoundLayout ? 31 : 27);

  title_label_ = lv_label_create(screen);
  lv_label_set_text(title_label_, profile.display_name.c_str());
  apply_text_style(title_label_, lv_color_hex(theme_style_.text_secondary),
                   kDisplayUsesCompactRoundLayout ? &lv_font_montserrat_12
                                                  : &lv_font_montserrat_14);
  lv_obj_set_size(title_label_, kDisplayUsesCompactRoundLayout ? 132 : 150, 18);
  lv_obj_set_style_text_align(
      title_label_, kDisplayUsesCompactRoundLayout ? LV_TEXT_ALIGN_CENTER
                                                   : LV_TEXT_ALIGN_LEFT,
      LV_PART_MAIN);
  lv_label_set_long_mode(title_label_, LV_LABEL_LONG_DOT);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, 13);
  } else {
    lv_obj_align(title_label_, LV_ALIGN_TOP_LEFT, 7, 7);
  }

  header_battery_outline_ = nullptr;
  header_battery_fill_ = nullptr;
  if constexpr (kDisplayUsesCompactRoundLayout) {
    header_power_label_ = nullptr;
  } else {
    header_power_label_ = lv_label_create(screen);
    apply_text_style(header_power_label_, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_12);
    lv_obj_set_width(header_power_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(header_power_label_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(header_power_label_, LV_ALIGN_TOP_RIGHT, -5, 8);
    if (power != nullptr && power->available && power->battery_present) {
      square_create_battery_icon(screen, power->battery_percent,
                                 theme_style_.text_primary,
                                 header_battery_outline_, header_battery_fill_);
      square_update_power_header(*power);
    } else {
      lv_label_set_text(header_power_label_, LV_SYMBOL_USB);
      square_align_audio_icon(header_audio_label_, nullptr, header_power_label_);
    }
  }

  progress_arc_ = lv_bar_create(screen);
  lv_obj_set_size(progress_arc_, kDisplayUsesCompactRoundLayout ? 132 : 190,
                  kDisplayUsesCompactRoundLayout ? 4 : 6);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    lv_obj_align(progress_arc_, LV_ALIGN_TOP_MID, 17, 38);
  } else {
    lv_obj_align(progress_arc_, LV_ALIGN_TOP_RIGHT, -6, 31);
  }
  lv_bar_set_range(progress_arc_, 0, 100);
  const int progress = std::clamp(static_cast<int>(snapshot.job.completion), 0, 100);
  lv_bar_set_value(progress_arc_, progress, LV_ANIM_OFF);
  lv_obj_set_style_radius(progress_arc_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_colors_.printing),
                            LV_PART_INDICATOR);
  lv_obj_set_style_text_color(progress_label_, lv_color_hex(theme_colors_.printing),
                              LV_PART_MAIN);
  create_printer_view_dots(1);
  create_depth_dots(4);
}

void DisplayShell::square_show_quick_menu() {
  set_capture_overlay_name("quick-menu");
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) {
    quick_overlay_ = lv_obj_create(lv_layer_top());
  } else {
    lv_obj_clean(quick_overlay_);
  }
  lv_obj_remove_flag(quick_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(quick_overlay_, LV_PCT(100), LV_PCT(100));
  lv_obj_center(quick_overlay_);
  lv_obj_remove_flag(quick_overlay_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(quick_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(quick_overlay_, lv_color_hex(theme_style_.background), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(quick_overlay_, LV_GRAD_DIR_NONE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(quick_overlay_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(quick_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(quick_overlay_, 0, LV_PART_MAIN);
  lv_obj_t* title = lv_label_create(quick_overlay_);
  lv_label_set_text(title, tr("QUICK MENU"));
  apply_text_style(title, lv_color_hex(theme_style_.text_primary),
                   kDisplayUsesCompactRoundLayout ? &lv_font_montserrat_16
                                                  : &lv_font_montserrat_24);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 16 : 12);
  static constexpr const char* icons[]{LV_SYMBOL_EYE_OPEN, LV_SYMBOL_VOLUME_MAX, "Aa"};
  static constexpr const char* labels[]{"DISPLAY & BRIGHTNESS", "SOUNDS", "LANGUAGE"};
  const std::uint32_t colors[]{theme_style_.accent_secondary,
                               theme_colors_.preparing, theme_colors_.done};
  for (int index = 0; index < 3; ++index) {
    if constexpr (!kBoardHasAudio) {
      if (index == 1) continue;
    }
    lv_obj_t* button = lv_button_create(quick_overlay_);
    if (index == 0) {
      lv_obj_set_size(button, kDisplayUsesCompactRoundLayout ? 180 : 208, 66);
      lv_obj_align(button, LV_ALIGN_TOP_MID, 0,
                   kDisplayUsesCompactRoundLayout ? 47 : 43);
    } else if constexpr (!kBoardHasAudio) {
      lv_obj_set_size(button, 180, 60);
      lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 124);
    } else {
      lv_obj_set_size(button, kDisplayUsesCompactRoundLayout ? 86 : 100, 60);
      lv_obj_align(button, LV_ALIGN_CENTER,
                   index == 1 ? (kDisplayUsesCompactRoundLayout ? -47 : -54)
                              : (kDisplayUsesCompactRoundLayout ? 47 : 54),
                   40);
    }
    lv_obj_set_style_radius(button, themed_radius(16), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(colors[index]), LV_PART_MAIN);
    apply_surface_effect(button);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<std::intptr_t>(index)));
    lv_obj_t* icon = lv_label_create(button);
    lv_label_set_text(icon, icons[index]);
    apply_icon_text_style(icon, lv_color_hex(theme_style_.text_primary),
                          index == 0 ? &lv_font_montserrat_16
                                     : &lv_font_montserrat_24);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, index == 0 ? -19 : -12);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, tr(labels[index]));
    apply_text_style(label, lv_color_hex(colors[index]), &lv_font_montserrat_12);
    if (index == 0) {
      lv_obj_set_size(label, 190, 14);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
      lv_obj_t* detail = lv_label_create(button);
      lv_label_set_text_fmt(detail, "%d%% / %s",
                            std::clamp(board_display_brightness_get(),
                                       kDisplayBrightnessMinimum, 100),
                            tr(theme_display_name(active_theme_)));
      apply_text_style(detail, lv_color_hex(theme_style_.text_secondary),
                       &lv_font_montserrat_12);
      lv_obj_set_size(detail, 190, 14);
      lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_align(detail, LV_ALIGN_CENTER, 0, 20);
    } else {
      lv_obj_set_width(label, 92);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -3);
    }
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      const int action = static_cast<int>(reinterpret_cast<std::intptr_t>(
          lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
      shell->pending_quick_menu_action_ = action;
      if (lv_async_call(quick_menu_action_async, shell) != LV_RESULT_OK) {
        shell->pending_quick_menu_action_ = -1;
      }
    }, LV_EVENT_CLICKED, this);
  }
  create_quick_overlay_close_button();
  lv_obj_move_foreground(quick_overlay_);
}

void DisplayShell::square_show_brightness_overlay() {
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) return;
  set_capture_overlay_name("brightness");
  lv_obj_clean(quick_overlay_);
  lv_obj_set_style_bg_color(quick_overlay_, lv_color_hex(theme_style_.background), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(quick_overlay_, LV_GRAD_DIR_NONE, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(quick_overlay_);
  lv_label_set_text(title, tr("DISPLAY & BRIGHTNESS"));
  apply_text_style(title, lv_color_hex(theme_style_.text_primary),
                   kDisplayUsesCompactRoundLayout ? &lv_font_montserrat_12
                                                  : &lv_font_montserrat_14);
  lv_obj_set_size(title, kDisplayUsesCompactRoundLayout ? 108 : 184,
                  kDisplayUsesCompactRoundLayout ? 34 : 18);
  lv_label_set_long_mode(title, kDisplayUsesCompactRoundLayout
                                   ? LV_LABEL_LONG_MODE_WRAP
                                   : LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 9 : 12);

  lv_obj_t* slider = lv_slider_create(quick_overlay_);
  lv_obj_set_size(slider, kDisplayUsesCompactRoundLayout ? 180 : 208, 14);
  lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 62);
  lv_slider_set_range(slider, kDisplayBrightnessMinimum, 100);
  lv_slider_set_value(
      slider,
      std::clamp(board_display_brightness_get(), kDisplayBrightnessMinimum, 100),
      LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.accent_secondary), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.text_primary), LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
  lv_obj_set_ext_click_area(slider, 24);
  lv_obj_add_event_cb(slider, [](lv_event_t* event) {
    auto* slider = lv_event_get_target_obj(event);
    const int next = std::clamp<int>(lv_slider_get_value(slider),
                                     kDisplayBrightnessMinimum, 100);
    board_display_brightness_set(next);
    lv_event_stop_bubbling(event);
  }, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(slider, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    const int next = std::clamp<int>(
        lv_slider_get_value(lv_event_get_target_obj(event)),
        kDisplayBrightnessMinimum, 100);
    shell->applied_brightness_ = next;
    if (shell->brightness_changed_ != nullptr) {
      shell->brightness_changed_(shell->brightness_changed_context_, next);
    }
    lv_event_stop_bubbling(event);
    shell->close_quick_overlay();
  }, LV_EVENT_RELEASED, this);

  lv_obj_t* theme = lv_button_create(quick_overlay_);
  lv_obj_set_size(theme, kDisplayUsesCompactRoundLayout ? 190 : 208, 48);
  lv_obj_align(theme, LV_ALIGN_TOP_MID, 0, 102);
  lv_obj_set_style_radius(theme, themed_radius(14), LV_PART_MAIN);
  lv_obj_set_style_bg_color(theme, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
  lv_obj_set_style_border_width(theme, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(theme, lv_color_hex(theme_style_.accent), LV_PART_MAIN);
  lv_obj_t* theme_icon = lv_label_create(theme);
  lv_label_set_text(theme_icon, LV_SYMBOL_IMAGE);
  apply_icon_text_style(theme_icon, lv_color_hex(theme_style_.accent),
                        &lv_font_montserrat_16);
  lv_obj_set_size(theme_icon, 22, 22);
  lv_obj_align(theme_icon, LV_ALIGN_LEFT_MID, 8, 0);
  lv_obj_t* theme_label = lv_label_create(theme);
  lv_label_set_text_fmt(theme_label, "%s: %s", tr("THEME"),
                        tr(theme_display_name(active_theme_)));
  apply_text_style(theme_label, lv_color_hex(theme_style_.text_primary),
                   &lv_font_montserrat_12);
  lv_obj_set_size(theme_label, 140, 18);
  lv_label_set_long_mode(theme_label, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(theme_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_align(theme_label, LV_ALIGN_LEFT_MID, 38, 0);
  lv_obj_t* theme_next = lv_label_create(theme);
  lv_label_set_text(theme_next, LV_SYMBOL_RIGHT);
  apply_icon_text_style(theme_next, lv_color_hex(theme_style_.text_muted),
                        &lv_font_montserrat_12);
  lv_obj_align(theme_next, LV_ALIGN_RIGHT_MID, -9, 0);
  lv_obj_add_event_cb(theme, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_event_stop_bubbling(event);
    shell->pending_quick_menu_action_ = 3;
    if (lv_async_call(quick_menu_action_async, shell) != LV_RESULT_OK) {
      shell->pending_quick_menu_action_ = -1;
    }
  }, LV_EVENT_CLICKED, this);

  lv_obj_t* animation_row = lv_obj_create(quick_overlay_);
  lv_obj_set_size(animation_row, kDisplayUsesCompactRoundLayout ? 190 : 208, 40);
  lv_obj_align(animation_row, LV_ALIGN_TOP_MID, 0, 154);
  lv_obj_add_flag(animation_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(animation_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(animation_row, themed_radius(14), LV_PART_MAIN);
  lv_obj_set_style_bg_color(animation_row, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
  lv_obj_set_style_border_width(animation_row, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(animation_row, lv_color_hex(theme_colors_.printing), LV_PART_MAIN);
  lv_obj_set_style_pad_all(animation_row, 0, LV_PART_MAIN);
  lv_obj_t* animation_icon = create_printer_animation_icon(
      animation_row, 20, theme_colors_.printing);
  lv_obj_align(animation_icon, LV_ALIGN_LEFT_MID, 9, 0);
  lv_obj_t* animation_label = lv_label_create(animation_row);
  lv_label_set_text(animation_label, tr("PRINTER ANIMATIONS"));
  apply_text_style(animation_label, lv_color_hex(theme_style_.text_primary),
                   &lv_font_montserrat_12);
  lv_obj_set_size(animation_label, 110, 30);
  lv_label_set_long_mode(animation_label, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(animation_label, LV_ALIGN_LEFT_MID, 37, 0);
  lv_obj_t* animation_switch = lv_switch_create(animation_row);
  lv_obj_set_size(animation_switch, 48, 28);
  lv_obj_align(animation_switch, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_set_style_bg_color(animation_switch, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(animation_switch, lv_color_hex(theme_colors_.printing),
                            static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                                static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
  lv_obj_set_style_bg_color(animation_switch, lv_color_hex(theme_style_.text_primary),
                            LV_PART_KNOB);
  lv_obj_set_ext_click_area(animation_switch, 7);
  if (printer_animations_enabled_) {
    lv_obj_add_state(animation_switch, LV_STATE_CHECKED);
  }
  lv_obj_set_user_data(animation_row, animation_switch);
  lv_obj_add_event_cb(animation_row, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_event_stop_bubbling(event);
    auto* animation_switch = static_cast<lv_obj_t*>(
        lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
    if (animation_switch == nullptr || !lv_obj_is_valid(animation_switch)) return;
    const bool enabled = !lv_obj_has_state(animation_switch, LV_STATE_CHECKED);
    if (enabled) lv_obj_add_state(animation_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(animation_switch, LV_STATE_CHECKED);
    if (shell->printer_animations_enabled_ == enabled) return;
    shell->apply_printer_animations_enabled(enabled);
    if (shell->printer_animations_changed_ != nullptr) {
      shell->printer_animations_changed_(shell->printer_animations_changed_context_, enabled);
    }
    shell->close_quick_overlay();
  }, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(animation_switch, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    lv_event_stop_bubbling(event);
    const bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    if (shell->printer_animations_enabled_ == enabled) return;
    shell->apply_printer_animations_enabled(enabled);
    if (shell->printer_animations_changed_ != nullptr) {
      shell->printer_animations_changed_(shell->printer_animations_changed_context_, enabled);
    }
    shell->close_quick_overlay();
  }, LV_EVENT_VALUE_CHANGED, this);

  create_quick_overlay_close_button();
}

void DisplayShell::square_show_audio_overlay() {
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) return;
  set_capture_overlay_name("audio");
  lv_obj_clean(quick_overlay_);
  lv_obj_t* title = lv_label_create(quick_overlay_);
  lv_label_set_text(title, tr("AUDIO"));
  apply_text_style(title, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_16);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 13 : 5);
  const int current = std::clamp(audio_volume_ > 0 ? audio_volume_ : 80, 1, 100);
  lv_obj_t* preset_title = lv_label_create(quick_overlay_);
  lv_label_set_text(preset_title, tr("SOUND SET"));
  apply_text_style(preset_title, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    lv_obj_align(preset_title, LV_ALIGN_TOP_MID, 0, 37);
  } else {
    lv_obj_align(preset_title, LV_ALIGN_TOP_LEFT, 8, 30);
  }
  static constexpr const char* preset_ids[]{"modern", "soft", "oldschool",
                                             "arcade", "scifi", "clean"};
  static constexpr const char* preset_names[]{"MODERN", "SOFT", "RETRO",
                                               "ARCADE", "SCI-FI", "VOICE"};
  for (int index = 0; index < 6; ++index) {
    const bool active = audio_preset_ == preset_ids[index];
    lv_obj_t* button = lv_button_create(quick_overlay_);
    lv_obj_set_size(button, kDisplayUsesCompactRoundLayout ? 58 : 70, 26);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT,
                 (kDisplayUsesCompactRoundLayout ? 29 : 6) +
                     (index % 3) * (kDisplayUsesCompactRoundLayout ? 62 : 77),
                 (kDisplayUsesCompactRoundLayout ? 55 : 46) +
                     (index / 3) * 30);
    lv_obj_set_ext_click_area(button, 2);
    lv_obj_set_style_radius(button, themed_radius(10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, active ? 3 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        button, lv_color_hex(active ? theme_style_.accent_secondary : theme_style_.border), LV_PART_MAIN);
    lv_obj_set_user_data(button,
                         reinterpret_cast<void*>(static_cast<std::intptr_t>(index)));
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, tr(preset_names[index]));
    apply_text_style(label, lv_color_hex(active ? theme_style_.accent_secondary : theme_style_.text_secondary),
                     &lv_font_montserrat_12);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      static constexpr const char* ids[]{"modern", "soft", "oldschool",
                                          "arcade", "scifi", "clean"};
      const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(
          lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
      if (index < 0 || index >= 6) return;
      shell->audio_preset_ = ids[index];
      if (shell->audio_preset_changed_ != nullptr) {
        shell->audio_preset_changed_(shell->audio_preset_changed_context_, ids[index]);
      }
      shell->close_quick_overlay();
    }, LV_EVENT_CLICKED, this);
  }
  lv_obj_t* mute_button = lv_button_create(quick_overlay_);
  lv_obj_set_size(mute_button, 34, 34);
  lv_obj_align(mute_button, LV_ALIGN_TOP_LEFT, 8, 117);
  lv_obj_set_style_radius(mute_button, themed_radius(11), LV_PART_MAIN);
  lv_obj_set_style_bg_color(mute_button, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
  lv_obj_set_style_border_width(mute_button, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(mute_button, lv_color_hex(theme_style_.accent_secondary), LV_PART_MAIN);
  lv_obj_set_ext_click_area(mute_button, 8);
  lv_obj_t* mute_icon = lv_label_create(mute_button);
  lv_label_set_text(mute_icon, audio_enabled_ ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
  apply_icon_text_style(mute_icon, lv_color_hex(theme_style_.accent_secondary),
                        &lv_font_montserrat_16);
  lv_obj_center(mute_icon);
  lv_obj_add_flag(mute_icon, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(mute_button, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_event_stop_bubbling(event);
    shell->audio_enabled_ = !shell->audio_enabled_;
    if (shell->audio_enabled_ && shell->audio_volume_ <= 0) shell->audio_volume_ = 80;
    if (shell->audio_changed_ != nullptr) {
      shell->audio_changed_(shell->audio_changed_context_, shell->audio_enabled_,
                            shell->audio_volume_);
    }
    shell->close_quick_overlay();
  }, LV_EVENT_CLICKED, this);

  lv_obj_t* slider = lv_slider_create(quick_overlay_);
  lv_obj_set_size(slider, 164, 14);
  lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 54, 127);
  lv_slider_set_range(slider, 1, 100);
  lv_slider_set_value(slider, current, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.accent_secondary), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.text_primary), LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
  lv_obj_set_ext_click_area(slider, 24);
  lv_obj_add_event_cb(slider, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    shell->audio_volume_ = std::clamp<int>(lv_slider_get_value(lv_event_get_target_obj(event)), 1, 100);
    shell->audio_enabled_ = true;
    if (shell->audio_changed_ != nullptr) {
      shell->audio_changed_(shell->audio_changed_context_, shell->audio_enabled_,
                            shell->audio_volume_);
    }
    lv_event_stop_bubbling(event);
    shell->close_quick_overlay();
  }, LV_EVENT_RELEASED, this);
  create_quick_overlay_close_button();
}

void DisplayShell::square_show_theme_overlay() {
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) return;
  set_capture_overlay_name("theme");
  lv_obj_clean(quick_overlay_);
  lv_obj_t* title = lv_label_create(quick_overlay_);
  lv_label_set_text(title, tr("CHOOSE THEME"));
  apply_text_style(title, lv_color_hex(theme_style_.text_primary),
                   kDisplayUsesCompactRoundLayout ? &lv_font_montserrat_14
                                                  : &lv_font_montserrat_16);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 16 : 8);
  static constexpr const char* ids[]{"green", "banana", "sunset", "ice",
                                      "cyberpunk", "ember", "mono", "red",
                                      "ios_glass", "fluent_dark", "retro_terminal", "custom"};
  static constexpr const char* names[]{"SIGNAL", "BANANA", "SOLSTICE", "GLACIER",
                                        "AURORA", "GROVE", "GRAPHITE", "GARNET",
                                        "HALO", "DRAGON", "TERM", "CUSTOM"};
  for (int index = 0; index < 12; ++index) {
    const core::ThemeColors colors = core::resolved_theme(ids[index], custom_theme_colors_);
    const core::ThemeStyle style = core::resolved_theme_style(ids[index], colors);
    lv_obj_t* button = lv_button_create(quick_overlay_);
    lv_obj_set_size(button, kDisplayUsesCompactRoundLayout ? 58 : 64, 33);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT,
                 (kDisplayUsesCompactRoundLayout ? 29 : 16) +
                     (index % 3) * (kDisplayUsesCompactRoundLayout ? 62 : 72),
                 (kDisplayUsesCompactRoundLayout ? 44 : 39) +
                     (index / 3) * 38);
    const int card_radius = style.corner_radius == 0
                                ? 0
                                : (style.corner_radius < 11 ? style.corner_radius : 11);
    lv_obj_set_style_radius(button, card_radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(style.surface_raised), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, active_theme_ == ids[index] ? 3 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(colors.printing), LV_PART_MAIN);
    lv_obj_set_user_data(button, const_cast<char*>(ids[index]));
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, tr(names[index]));
    apply_text_style(label, lv_color_hex(style.text_primary), &lv_font_montserrat_12);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      const char* id = static_cast<const char*>(
          lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
      if (shell == nullptr || id == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      shell->request_theme_selection(id);
    }, LV_EVENT_CLICKED, this);
  }
  create_quick_overlay_close_button();
}

void DisplayShell::square_show_wifi_error(const char* network_name) {
  if (view_ == 8 || board_display_lock(1000) != ESP_OK) return;
  prepare_active_screen("wifi-error");
  lv_obj_t* heading = lv_label_create(lv_screen_active());
  lv_label_set_text(heading, tr("WI-FI ERROR"));
  apply_text_style(heading, lv_color_hex(theme_colors_.error), &lv_font_montserrat_24);
  lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_t* detail = lv_label_create(lv_screen_active());
  const char* target = network_name != nullptr && network_name[0] != '\0'
                           ? network_name : tr("saved network");
  lv_label_set_text_fmt(detail, "%s:\n%s\n\n%s", tr("Could not connect to Wi-Fi"),
                        target, tr("Opening Wi-Fi Setup..."));
  apply_text_style(detail, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_14);
  lv_obj_set_width(detail, 214);
  lv_obj_align(detail, LV_ALIGN_CENTER, 0, 30);
  status_label_ = nullptr;
  view_ = 8;
  board_display_unlock();
}

void DisplayShell::square_show_wifi_setup(const char* network_name,
                                          const char* local_hostname) {
  const std::string primary_host = local_hostname != nullptr && local_hostname[0] != '\0'
      ? local_hostname : "192.168.4.1";
  if (network_name == nullptr ||
      (view_ == 1 && visible_web_config_host_ == primary_host) ||
      board_display_lock(1000) != ESP_OK) return;
  prepare_active_screen("wifi-setup");
  if constexpr (kDisplayUsesCompactRoundLayout) {
    lv_obj_t* heading = lv_label_create(lv_screen_active());
    lv_label_set_text(heading, tr("WI-FI SETUP"));
    apply_text_style(heading, lv_color_hex(accent_color_), &lv_font_montserrat_16);
    lv_obj_set_width(heading, 150);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 11);
    active_accent_label_ = heading;
    active_accent_text_objects_.push_back(heading);
  } else {
    square_create_header("WI-FI SETUP");
    if (header_power_label_ != nullptr && lv_obj_is_valid(header_power_label_)) {
      lv_obj_delete(header_power_label_);
      header_power_label_ = nullptr;
    }
    if (header_audio_label_ != nullptr && lv_obj_is_valid(header_audio_label_)) {
      lv_obj_delete(header_audio_label_);
      header_audio_label_ = nullptr;
    }
  }
  const char* language_code = "EN";
  if (language_ == "pl") language_code = "PL";
  else if (language_ == "es") language_code = "ES";
  else if (language_ == "fr") language_code = "FR";
  else if (language_ == "de") language_code = "DE";
  else if (language_ == "zh-CN") language_code = "中文";
  const char* language_name = "English";
  for (const auto& language : core::kLanguages) {
    if (language.code == language_) {
      language_name = language.native_name.data();
      break;
    }
  }
  if constexpr (!kDisplayUsesCompactRoundLayout) {
    if (active_accent_label_ != nullptr) lv_obj_set_width(active_accent_label_, 110);
    lv_obj_t* language_button = lv_button_create(lv_screen_active());
    lv_obj_set_size(language_button, 56, 25);
    lv_obj_align(language_button, LV_ALIGN_TOP_RIGHT, -3, 2);
    lv_obj_set_style_radius(language_button, themed_radius(12), LV_PART_MAIN);
    lv_obj_set_style_bg_color(language_button, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
    lv_obj_set_style_border_width(language_button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(language_button, lv_color_hex(accent_color_), LV_PART_MAIN);
    lv_obj_add_event_cb(language_button, wifi_setup_language_event, LV_EVENT_CLICKED, this);
    lv_obj_t* language_label = lv_label_create(language_button);
    lv_label_set_text_fmt(language_label, "%s >", language_code);
    apply_text_style(language_label, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_12);
    lv_obj_center(language_label);
  }
  std::string payload = "WIFI:T:nopass;S:";
  for (const char* cursor = network_name; *cursor != '\0'; ++cursor) {
    if (std::strchr("\\;,:\"", *cursor) != nullptr) payload.push_back('\\');
    payload.push_back(*cursor);
  }
  payload += ";;";

  lv_obj_t* screen = lv_screen_active();
  wifi_setup_pager_ = lv_tileview_create(screen);
  lv_obj_set_size(wifi_setup_pager_, 240,
                  kDisplayUsesCompactRoundLayout ? 194 : 198);
  lv_obj_align(wifi_setup_pager_, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 32 : 29);
  lv_obj_set_scrollbar_mode(wifi_setup_pager_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_radius(wifi_setup_pager_, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(wifi_setup_pager_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(wifi_setup_pager_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_pad_all(wifi_setup_pager_, 0, LV_PART_MAIN);
  lv_obj_remove_flag(wifi_setup_pager_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(wifi_setup_pager_, wifi_setup_pager_event, LV_EVENT_ALL, this);

  auto create_tile = [&](std::uint8_t column, lv_dir_t direction, std::uintptr_t step) {
    lv_obj_t* tile = lv_tileview_add_tile(wifi_setup_pager_, column, 0, direction);
    lv_obj_set_user_data(tile, reinterpret_cast<void*>(step));
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_radius(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    return tile;
  };
  auto add_label = [&](lv_obj_t* tile, const char* text, const lv_font_t* font,
                       lv_color_t color, int y, int width = 224) {
    lv_obj_t* label = lv_label_create(tile);
    lv_label_set_text(label, text);
    apply_text_style(label, color, font);
    lv_obj_set_width(label, width);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    square_route_screen_gestures(label);
    return label;
  };
  auto add_qr = [&](lv_obj_t* tile, const char* qr_payload) {
    lv_obj_t* qr = lv_qrcode_create(tile);
    lv_qrcode_set_size(qr, kDisplayUsesCompactRoundLayout ? 104 : 112);
    lv_qrcode_set_dark_color(qr, lv_color_hex(theme_style_.surface));
    lv_qrcode_set_light_color(qr, lv_color_hex(theme_style_.text_primary));
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_set_data(qr, qr_payload);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0,
                 kDisplayUsesCompactRoundLayout ? 24 : 33);
    square_route_screen_gestures(qr);
  };
  auto add_round_language_button = [&](lv_obj_t* tile) {
    lv_obj_t* button = lv_button_create(tile);
    lv_obj_set_size(button, 76, 22);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 171);
    lv_obj_set_style_radius(button, themed_radius(11), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(accent_color_), LV_PART_MAIN);
    lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(button, wifi_setup_language_event, LV_EVENT_CLICKED, this);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text_fmt(label, "%s >", language_name);
    apply_text_style(label, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_12);
    lv_obj_center(label);
  };

  lv_obj_t* join_tile = create_tile(0, LV_DIR_RIGHT, 1U);
  lv_obj_t* join_title = add_label(
      join_tile, tr("Connect to PrintDeck Wi-Fi"), &lv_font_montserrat_12,
      lv_color_hex(theme_style_.text_primary), 1,
      kDisplayUsesCompactRoundLayout ? 172 : 224);
  lv_obj_set_height(join_title, kDisplayUsesCompactRoundLayout ? 22 : 30);
  lv_label_set_long_mode(join_title, LV_LABEL_LONG_WRAP);
  add_qr(join_tile, payload.c_str());
  const std::string network_label = std::string("Wi-Fi: ") + network_name;
  lv_obj_t* network = add_label(join_tile, network_label.c_str(), &lv_font_montserrat_12,
                                lv_color_hex(accent_color_),
                                kDisplayUsesCompactRoundLayout ? 132 : 151,
                                kDisplayUsesCompactRoundLayout ? 208 : 224);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    lv_label_set_long_mode(network, LV_LABEL_LONG_DOT);
  }
  active_accent_text_objects_.push_back(network);
  add_label(join_tile, tr("Setup opens automatically"), &lv_font_montserrat_12,
            lv_color_hex(theme_style_.text_muted),
            kDisplayUsesCompactRoundLayout ? 152 : 171,
            kDisplayUsesCompactRoundLayout ? 184 : 224);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    add_round_language_button(join_tile);
  }

  lv_obj_t* web_tile = create_tile(1, LV_DIR_LEFT, 2U);
  lv_obj_t* web_title = add_label(
      web_tile, tr("If setup does not open"), &lv_font_montserrat_12,
      lv_color_hex(theme_style_.text_primary), 1,
      kDisplayUsesCompactRoundLayout ? 172 : 224);
  lv_obj_set_height(web_title, kDisplayUsesCompactRoundLayout ? 22 : 30);
  lv_label_set_long_mode(web_title, LV_LABEL_LONG_WRAP);
  const std::string web_url = std::string("http://") + primary_host + "/";
  add_qr(web_tile, web_url.c_str());
  lv_obj_t* address = add_label(web_tile, primary_host.c_str(), &lv_font_montserrat_12,
                                lv_color_hex(accent_color_),
                                kDisplayUsesCompactRoundLayout ? 132 : 151,
                                kDisplayUsesCompactRoundLayout ? 208 : 224);
  active_accent_text_objects_.push_back(address);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    add_round_language_button(web_tile);
  }

  create_wifi_setup_navigation(screen);
  lv_tileview_set_tile(wifi_setup_pager_, join_tile, LV_ANIM_OFF);
  status_label_ = nullptr;
  visible_web_config_host_ = primary_host;
  view_ = 1;
  board_display_unlock();
}

void DisplayShell::square_show_my_printers(
    const char* ipv4, const char* local_hostname,
    const std::vector<core::PrinterProfile>& profiles,
    std::uint32_t selected_profile, const InactivePrinterSnapshot& inactive,
    const PowerSnapshot& power, const core::PrinterSnapshot* selected_snapshot) {
  if (ipv4 == nullptr || board_display_lock(1000) != ESP_OK) return;
  const std::string primary_host = local_hostname != nullptr && local_hostname[0] != '\0'
      ? local_hostname : ipv4;
  const core::LinkState selected_link = selected_snapshot != nullptr
      ? selected_snapshot->link : core::LinkState::stopped;
  if (view_ != 2 || visible_inactive_revision_ != inactive.revision ||
      selected_profile_ != selected_profile || visible_selected_link_ != selected_link ||
      (profiles.empty() && visible_web_config_host_ != primary_host)) {
    prepare_active_screen("my-printers");
    square_create_header("MY PRINTERS", &power);
    if (profiles.empty()) {
      lv_obj_t* empty = lv_label_create(lv_screen_active());
      lv_label_set_text(empty, tr("No printers added yet"));
      apply_text_style(empty, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_16);
      lv_obj_set_width(empty, kDisplayUsesCompactRoundLayout ? 164 : 220);
      lv_obj_align(empty, LV_ALIGN_TOP_MID, 0,
                   kDisplayUsesCompactRoundLayout ? 46 : 42);
      lv_obj_t* qr = lv_qrcode_create(lv_screen_active());
      lv_qrcode_set_size(qr, kDisplayUsesCompactRoundLayout ? 108 : 116);
      lv_qrcode_set_dark_color(qr, lv_color_hex(theme_style_.background));
      lv_qrcode_set_light_color(qr, lv_color_hex(theme_style_.text_primary));
      lv_qrcode_set_quiet_zone(qr, true);
      const std::string url = std::string("http://") + primary_host;
      lv_qrcode_set_data(qr, url.c_str());
      lv_obj_align(qr, LV_ALIGN_CENTER, 0, 17);
      lv_obj_t* caption = lv_label_create(lv_screen_active());
      lv_label_set_text(caption, url.c_str());
      apply_text_style(caption, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
      lv_obj_set_width(caption, kDisplayUsesCompactRoundLayout ? 164 : 220);
      lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
      lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -14);
    } else {
      lv_obj_t* list = lv_obj_create(lv_screen_active());
      lv_obj_set_size(list, kDisplayUsesCompactRoundLayout ? 190 : 218,
                      kDisplayUsesCompactRoundLayout ? 120 : 176);
      lv_obj_align(list, LV_ALIGN_TOP_MID, 0,
                   kDisplayUsesCompactRoundLayout ? 45 : 32);
      lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_all(list, kDisplayUsesCompactRoundLayout ? 3 : 2,
                               LV_PART_MAIN);
      lv_obj_set_style_pad_row(list, kDisplayUsesCompactRoundLayout ? 6 : 5,
                               LV_PART_MAIN);
      lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
      // Card pointer events bubble through the list to the screen-level
      // gesture router. The large layout already applies these flags; the
      // shared 240 x 240 layout needs them as well, especially while the list
      // itself owns a vertical scroll gesture.
      lv_obj_add_flag(list, LV_OBJ_FLAG_EVENT_BUBBLE);
      lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
      constexpr std::size_t kVisiblePrinterCards =
          kDisplayUsesCompactRoundLayout ? 2 : 3;
      const bool list_overflow = profiles.size() > kVisiblePrinterCards;
      if (list_overflow) {
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_START);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
      } else {
        lv_obj_set_scroll_dir(list, LV_DIR_NONE);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
        square_route_screen_gestures(list);
      }
      for (const auto& profile : profiles) {
        const auto inactive_status = std::find_if(
            inactive.printers.begin(), inactive.printers.end(),
            [&profile](const InactivePrinterStatus& status) {
              return status.profile_id == profile.id;
            });
        const bool is_selected = profile.id == selected_profile;
        const bool selected_online = is_selected && selected_snapshot != nullptr &&
                                     selected_snapshot->link == core::LinkState::online;
        const bool has_status = inactive_status != inactive.printers.end() &&
                                inactive_status->available;
        const bool checking = has_status && inactive_status->checking;
        const bool connected = is_selected ? selected_online
                                           : has_status && inactive_status->connected;
        const bool selectable = !checking && core::printer_selectable(
            is_selected, connected ? core::PrinterReachability::online
                                   : has_status ? core::PrinterReachability::offline
                                                : core::PrinterReachability::unknown);
        lv_obj_t* card = lv_obj_create(list);
        lv_obj_set_size(card, kDisplayUsesCompactRoundLayout ? 176 : 202, 54);
        lv_obj_set_flex_grow(card, 0);
        lv_obj_set_style_radius(card, themed_radius(12), LV_PART_MAIN);
        lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, is_selected ? 2 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_hex(is_selected ? accent_color_ : theme_style_.border),
                                      LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 7, LV_PART_MAIN);
        apply_surface_effect(card);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        if (list_overflow) lv_obj_add_flag(card, LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_user_data(card,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(profile.id)));
        if (selectable) {
          lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
          lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_soft), LV_STATE_PRESSED);
          lv_obj_set_style_border_color(card, lv_color_hex(accent_color_),
                                        LV_STATE_PRESSED);
          lv_obj_set_style_border_width(card, 2, LV_STATE_PRESSED);
        } else {
          lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, profile.display_name.c_str());
        apply_text_style(name, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_14);
        // LV_LABEL_LONG_DOT only ellipsizes when both dimensions are bounded.
        // Keep printer names to one line so they can never cover the endpoint.
        lv_obj_set_size(name, kDisplayUsesCompactRoundLayout ? 104 : 126, 18);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, -3);
        lv_obj_add_flag(name, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(name, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_t* detail = lv_label_create(card);
        std::string endpoint = profile.endpoint;
        if (endpoint.rfind("http://", 0) == 0) endpoint.erase(0, 7);
        else if (endpoint.rfind("https://", 0) == 0) endpoint.erase(0, 8);
        while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
        lv_label_set_text(detail, endpoint.empty() ? "--" : endpoint.c_str());
        apply_text_style(detail, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
        lv_obj_set_width(detail, kDisplayUsesCompactRoundLayout ? 104 : 126);
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
        lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 0, 3);
        lv_obj_add_flag(detail, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(detail, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_t* state = lv_label_create(card);
        const std::uint64_t now_ms =
            static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
        const bool retry_waiting = profile.id == printer_retry_wait_profile_ &&
                                   now_ms < printer_retry_wait_until_ms_;
        const bool selected_failed =
            is_selected && selected_snapshot != nullptr &&
            selected_snapshot->link == core::LinkState::failed;
        const bool selected_connecting =
            is_selected && !selected_failed && !selected_online;
        const char* state_text = retry_waiting ? "WAIT"
            : selected_failed ? "OFFLINE"
            : selected_connecting || checking ? "CONNECTING"
            : connected ? "ONLINE" : has_status ? "OFFLINE" : "UNKNOWN";
        const std::uint32_t state_color = retry_waiting ? theme_colors_.preparing
            : selected_failed ? theme_colors_.offline
            : selected_connecting || checking ? theme_colors_.preparing
            : connected ? theme_colors_.done
            : has_status ? theme_colors_.offline : theme_colors_.unknown;
        lv_label_set_text(state, tr(state_text));
        apply_text_style(state, lv_color_hex(state_color), &lv_font_montserrat_12);
        lv_obj_set_width(state, kDisplayUsesCompactRoundLayout ? 58 : 65);
        lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_align(state, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(state, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(state, LV_OBJ_FLAG_GESTURE_BUBBLE);
      }
      configure_printer_list_scroll(list, profiles.size(), kVisiblePrinterCards,
                                    kDisplayUsesCompactRoundLayout ? 54 + 6
                                                                   : 54 + 5);
    }
    // Keep the list pager in the unused edge gutter instead of overlaying the
    // right side of printer cards on the 240x240 display.
    create_page_dots(1);
    view_ = 2;
    visible_profile_ = 0;
    visible_inactive_revision_ = inactive.revision;
    visible_web_config_host_ = primary_host;
    visible_selected_link_ = selected_link;
  }
  selected_profile_ = selected_profile;
  selected_online_.store(core::dashboard_available(selected_profile, selected_snapshot));
  square_update_power_header(power);
  board_display_unlock();
}

void DisplayShell::square_show_printer_status(const core::PrinterProfile& profile,
                                               const core::PrinterSnapshot& snapshot,
                                               const PowerSnapshot& power) {
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 3 || visible_profile_ != profile.id) {
    prepare_active_screen("printer-status");
    square_create_printer_chrome(profile, snapshot, &power);
    // Keep the summary structurally symmetric: a fixed thumbnail column and a
    // fixed details column. Children are positioned inside their own section,
    // so text length cannot move neighboring content.
    lv_obj_t* summary = square_layout_box(lv_screen_active(), 222, 80);
    lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 67);

    lv_obj_t* media_frame = lv_obj_create(summary);
    const bool has_preview = preview_pixels_ && !preview_pixels_->empty();
    lv_obj_set_size(media_frame, 70, 70);
    lv_obj_align(media_frame, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(media_frame, themed_radius(10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(media_frame,
                              lv_color_hex(theme_colors_.preview_background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(media_frame, has_preview ? LV_OPA_COVER : LV_OPA_TRANSP,
                            LV_PART_MAIN);
    lv_obj_set_style_border_width(media_frame, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(media_frame, lv_color_hex(theme_colors_.done),
                                  LV_PART_MAIN);
    lv_obj_set_style_pad_all(media_frame, 0, LV_PART_MAIN);
    square_route_screen_gestures(media_frame);
    if (has_preview) {
      media_image_ = lv_image_create(media_frame);
      lv_image_set_src(media_image_, &preview_image_dsc_);
      lv_obj_set_size(media_image_, 64, 64);
      lv_image_set_inner_align(media_image_, LV_IMAGE_ALIGN_CONTAIN);
      lv_obj_center(media_image_);
      square_route_screen_gestures(media_image_);
    } else if (const lv_image_dsc_t* logo = brand_logo_small(profile); logo != nullptr) {
      lv_obj_t* mark = lv_image_create(media_frame);
      lv_image_set_src(mark, logo);
      lv_obj_set_style_image_recolor(
          mark, lv_color_hex(brand_logo_color(profile, theme_style_.background)),
          LV_PART_MAIN);
      lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_center(mark);
      square_route_screen_gestures(mark);
    } else {
      lv_obj_t* mark = lv_label_create(media_frame);
      lv_label_set_text(mark, brand_mark(profile));
      apply_text_style(mark, lv_color_hex(brand_color(profile)), &lv_font_montserrat_24);
      lv_obj_center(mark);
      square_route_screen_gestures(mark);
    }
    detail_label_ = lv_label_create(lv_screen_active());
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_14);
    // Leave the right-side page indicator its own gutter.  Bounding both
    // dimensions makes LVGL ellipsize long job names instead of clipping them.
    lv_obj_set_size(detail_label_, kDisplayUsesCompactRoundLayout ? 168 : 200, 18);
    lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0,
                 kDisplayUsesCompactRoundLayout ? 47 : 43);
    status_label_ = lv_label_create(lv_screen_active());
    apply_text_style(status_label_, lv_color_hex(accent_color_), &lv_font_montserrat_16);
    lv_obj_set_size(status_label_, 218, 22);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 198);

    lv_obj_t* details = square_layout_box(summary, 138, 80);
    lv_obj_align(details, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t* timers = square_layout_box(details, 138, 44);
    lv_obj_align(timers, LV_ALIGN_TOP_MID, 0,
                 kDisplayUsesCompactRoundLayout ? 6 : 0);
    lv_obj_t* clock_slot = square_mdi_icon_slot(
        timers, kMdiClock, 26, 44, theme_style_.accent_secondary);
    lv_obj_align(clock_slot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* timer_values = square_layout_box(timers, 112, 44);
    lv_obj_align(timer_values, LV_ALIGN_RIGHT_MID, 0, 0);

    remaining_label_ = lv_label_create(timer_values);
    apply_text_style(remaining_label_, lv_color_hex(theme_style_.accent_secondary),
                     &lv_font_montserrat_16);
    lv_obj_set_size(remaining_label_, 108, 22);
    lv_obj_set_style_text_align(remaining_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(remaining_label_, LV_ALIGN_TOP_LEFT, 2, 0);
    total_time_label_ = lv_label_create(timer_values);
    apply_text_style(total_time_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_12);
    lv_obj_set_size(total_time_label_, 108, 18);
    lv_obj_set_style_text_align(total_time_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(total_time_label_, LV_ALIGN_BOTTOM_LEFT, 2, 0);
    layer_label_ = lv_label_create(details);
    apply_text_style(layer_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
    lv_obj_set_size(layer_label_, 138, 20);
    lv_obj_set_style_text_align(layer_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(layer_label_, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Divide the temperature band into three identical 70 px cells. Each cell
    // centers the complete icon + value group, rather than centering the value
    // alone, which keeps all three visual centers exactly evenly spaced. The
    // narrower 210 px band leaves a dedicated gutter for the vertical pager.
    lv_obj_t* temperature_row = square_layout_box(lv_screen_active(), 210, 24);
    lv_obj_align(temperature_row, LV_ALIGN_TOP_MID, 0, 154);
    lv_obj_add_flag(temperature_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t* temperature_cells[3]{};
    for (auto*& cell : temperature_cells) {
      cell = square_layout_box(temperature_row, 70, 24);
      lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }
    lv_obj_align(temperature_cells[0], LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(temperature_cells[1], LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(temperature_cells[2], LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* nozzle_icon = square_mdi_icon_slot(temperature_cells[0], kMdiNozzle, 18, 24,
                                                 kNozzleTemperatureColor);
    lv_obj_t* bed_icon = square_mdi_icon_slot(temperature_cells[1], kMdiBed, 18, 24,
                                              kBedTemperatureColor);
    lv_obj_t* chamber_icon = square_thermometer_slot(temperature_cells[2], 18, 24,
                                                     kChamberTemperatureColor);
    for (auto* icon : {nozzle_icon, bed_icon, chamber_icon}) {
      lv_obj_align(icon, LV_ALIGN_LEFT_MID, 1, 0);
    }

    nozzle_temperature_label_ = lv_label_create(temperature_cells[0]);
    bed_temperature_label_ = lv_label_create(temperature_cells[1]);
    chamber_temperature_label_ = lv_label_create(temperature_cells[2]);
    apply_text_style(nozzle_temperature_label_, lv_color_hex(kNozzleTemperatureColor),
                     &lv_font_montserrat_14);
    apply_text_style(bed_temperature_label_, lv_color_hex(kBedTemperatureColor),
                     &lv_font_montserrat_14);
    apply_text_style(chamber_temperature_label_, lv_color_hex(kChamberTemperatureColor),
                     &lv_font_montserrat_14);
    for (auto* label : {nozzle_temperature_label_, bed_temperature_label_,
                        chamber_temperature_label_}) {
      lv_obj_set_size(label, 48, 18);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
      lv_obj_align(label, LV_ALIGN_LEFT_MID, 22, 0);
      square_route_screen_gestures(label);
    }
    metrics_label_ = lv_label_create(lv_screen_active());
    apply_text_style(metrics_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
    lv_obj_set_width(metrics_label_, 220);
    lv_obj_set_style_text_align(metrics_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(metrics_label_, LV_ALIGN_TOP_MID, 0, 181);
    view_ = 3;
    visible_profile_ = profile.id;
  }
  const int progress = std::clamp(static_cast<int>(snapshot.job.completion), 0, 100);
  const std::uint32_t color = core::phase_color(theme_colors_, snapshot.job.phase,
                                                snapshot.job.reachable);
  const bool active = snapshot.job.phase == core::JobPhase::printing ||
                      snapshot.job.phase == core::JobPhase::preparing ||
                      snapshot.job.phase == core::JobPhase::paused;
  lv_bar_set_value(progress_arc_, progress, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_colors_.printing),
                            LV_PART_INDICATOR);
  lv_label_set_text_fmt(progress_label_, "%d%%", progress);
  lv_obj_set_style_text_color(progress_label_, lv_color_hex(theme_colors_.printing),
                              LV_PART_MAIN);
  const std::string display_job_name = core::job_name_for_display(snapshot.job.name);
  lv_label_set_text(detail_label_, snapshot.job.kind == core::JobKind::calibration
                                       ? tr("Printer calibration")
                                       : display_job_name.empty()
                                             ? tr("No active print")
                                             : display_job_name.c_str());
  lv_label_set_text(status_label_, tr(core::job_status_label(snapshot.job)));
  lv_obj_set_style_text_color(status_label_, lv_color_hex(color), LV_PART_MAIN);
  lv_label_set_text(remaining_label_, active ? short_duration(snapshot.job.remaining_seconds).c_str()
                                             : "--");
  const std::uint32_t total_seconds =
      snapshot.job.elapsed_seconds + snapshot.job.remaining_seconds;
  lv_label_set_text(total_time_label_,
                    active && total_seconds > 0 ? short_duration(total_seconds).c_str() : "--");
  lv_label_set_text_fmt(layer_label_, "%s: %u/%u", tr("Layer"), snapshot.job.current_layer,
                        snapshot.job.total_layers);
  lv_label_set_text_fmt(nozzle_temperature_label_, "%.0f°C",
                        snapshot.job.temperatures.nozzle_c);
  lv_label_set_text_fmt(bed_temperature_label_, "%.0f°C", snapshot.job.temperatures.bed_c);
  if (snapshot.job.temperatures.chamber_known) {
    lv_label_set_text_fmt(chamber_temperature_label_, "%.0f°C",
                          snapshot.job.temperatures.chamber_c);
  } else lv_label_set_text(chamber_temperature_label_, "--°C");
  lv_label_set_text(metrics_label_, snapshot.link == core::LinkState::online
                                      ? tr("Printer ready") : tr(square_link_label(snapshot.link)));
  square_update_power_header(power);
  board_display_unlock();
}

void DisplayShell::square_show_printer_nozzles(const core::PrinterProfile& profile,
                                                const core::PrinterSnapshot& snapshot,
                                                const PowerSnapshot& power) {
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 19 || visible_profile_ != profile.id) {
    prepare_active_screen("nozzles");
    square_create_printer_chrome(profile, snapshot, &power);
    nozzle_scroll_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(nozzle_scroll_, kDisplayUsesCompactRoundLayout ? 204 : 210,
                                    kDisplayUsesCompactRoundLayout ? 142 : 158);
    lv_obj_align(nozzle_scroll_, LV_ALIGN_TOP_MID, 0,
                 kDisplayUsesCompactRoundLayout ? 53 : 43);
    lv_obj_set_scroll_dir(nozzle_scroll_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(nozzle_scroll_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(nozzle_scroll_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(nozzle_scroll_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nozzle_scroll_, 0, LV_PART_MAIN);
    lv_obj_set_style_width(nozzle_scroll_, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(nozzle_scroll_, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(nozzle_scroll_, lv_color_hex(accent_color_),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(nozzle_scroll_, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(nozzle_scroll_, nozzle_scroll_event, LV_EVENT_ALL, this);

    // Only this viewport scrolls. Keeping the row and every card static avoids
    // nested card scrollbars and lets vertical screen gestures bubble through.
    nozzle_row_ = square_layout_box(nozzle_scroll_,
                                    kDisplayUsesCompactRoundLayout ? 204 : 210,
                                    kDisplayUsesCompactRoundLayout ? 128 : 144);
    lv_obj_set_pos(nozzle_row_, 0, 0);
    for (std::size_t index = 0; index < core::kMaximumToolheads; ++index) {
      nozzle_cards_[index] = lv_obj_create(nozzle_row_);
      lv_obj_set_size(nozzle_cards_[index],
                      kDisplayUsesCompactRoundLayout ? 60 : 64,
                      kDisplayUsesCompactRoundLayout ? 128 : 144);
      lv_obj_set_pos(nozzle_cards_[index],
                     static_cast<int>(index) *
                         (kDisplayUsesCompactRoundLayout ? 66 : 70),
                     0);
      lv_obj_set_style_radius(nozzle_cards_[index], themed_radius(14), LV_PART_MAIN);
      lv_obj_set_style_bg_color(nozzle_cards_[index], lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_cards_[index], LV_OPA_80, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_cards_[index], 1, LV_PART_MAIN);
      lv_obj_set_style_border_color(nozzle_cards_[index], lv_color_hex(theme_style_.border), LV_PART_MAIN);
      lv_obj_set_style_pad_all(nozzle_cards_[index], 0, LV_PART_MAIN);
      apply_surface_effect(nozzle_cards_[index]);
      square_route_screen_gestures(nozzle_cards_[index]);

      nozzle_tool_labels_[index] = lv_label_create(nozzle_cards_[index]);
      nozzle_target_labels_[index] = lv_label_create(nozzle_cards_[index]);
      lv_obj_t* nozzle_icon_slot = square_mdi_icon_slot(
          nozzle_cards_[index], kMdiNozzle, 22, 24, theme_style_.text_muted,
          &nozzle_icons_[index]);
      nozzle_temperature_labels_[index] = lv_label_create(nozzle_cards_[index]);
      nozzle_material_labels_[index] = lv_label_create(nozzle_cards_[index]);
      lv_label_set_text_fmt(nozzle_tool_labels_[index], "T%d", index);
      apply_text_style(nozzle_tool_labels_[index], lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_14);
      apply_text_style(nozzle_target_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_12);
      apply_text_style(nozzle_temperature_labels_[index], lv_color_hex(theme_style_.text_primary),
                       &lv_font_montserrat_16);
      apply_text_style(nozzle_material_labels_[index], lv_color_hex(theme_style_.text_secondary),
                       &lv_font_montserrat_12);

      for (auto* label : {nozzle_tool_labels_[index], nozzle_target_labels_[index],
                          nozzle_temperature_labels_[index],
                          nozzle_material_labels_[index]}) {
        lv_obj_set_width(label, 58);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        square_route_screen_gestures(label);
      }
      lv_obj_align(nozzle_tool_labels_[index], LV_ALIGN_TOP_MID, 0, 3);
      lv_obj_align(nozzle_target_labels_[index], LV_ALIGN_TOP_MID, 0,
                   kDisplayUsesCompactRoundLayout ? 22 : 25);
      lv_obj_align(nozzle_icon_slot, LV_ALIGN_TOP_MID, 0,
                   kDisplayUsesCompactRoundLayout ? 39 : 43);
      lv_obj_align(nozzle_temperature_labels_[index], LV_ALIGN_TOP_MID, 0,
                   kDisplayUsesCompactRoundLayout ? 64 : 72);
      lv_label_set_long_mode(nozzle_material_labels_[index], LV_LABEL_LONG_DOT);
      lv_obj_align(nozzle_material_labels_[index], LV_ALIGN_TOP_MID, 0,
                   kDisplayUsesCompactRoundLayout ? 88 : 99);

      nozzle_material_dots_[index] = lv_obj_create(nozzle_cards_[index]);
      lv_obj_set_size(nozzle_material_dots_[index], 10, 10);
      lv_obj_set_style_radius(nozzle_material_dots_[index], LV_RADIUS_CIRCLE,
                              LV_PART_MAIN);
      lv_obj_set_style_bg_color(nozzle_material_dots_[index], lv_color_hex(theme_style_.text_muted),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(nozzle_material_dots_[index], 0, LV_PART_MAIN);
      lv_obj_align(nozzle_material_dots_[index], LV_ALIGN_BOTTOM_MID, 0,
                   kDisplayUsesCompactRoundLayout ? -7 : -8);
      square_route_screen_gestures(nozzle_material_dots_[index]);
    }
    view_ = 19;
    visible_profile_ = profile.id;
  }
  int count = snapshot.job.toolhead_count;
  if (count <= 0) count = 1;
  count = std::clamp(count, 1, static_cast<int>(core::kMaximumToolheads));
  constexpr int kViewportWidth = kDisplayUsesCompactRoundLayout ? 204 : 210;
  constexpr int kOverflowCardWidth = kDisplayUsesCompactRoundLayout ? 60 : 64;
  constexpr int kOverflowGap = 6;
  constexpr int kOverflowStride = kOverflowCardWidth + kOverflowGap;
  const bool overflow = count > 3;
  const int content_width = overflow ? count * kOverflowStride - kOverflowGap
                                     : kViewportWidth;
  lv_obj_set_width(nozzle_row_, content_width);
  if (overflow) lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_SCROLLABLE);
  else lv_obj_remove_flag(nozzle_scroll_, LV_OBJ_FLAG_SCROLLABLE);
  // Keep bubbling enabled even for an overflowing strip. The event callback
  // consumes horizontal drags and deliberately passes vertical page swipes.
  lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_scrollbar_mode(nozzle_scroll_, overflow ? LV_SCROLLBAR_MODE_ON
                                                     : LV_SCROLLBAR_MODE_OFF);
  if (!overflow) lv_obj_scroll_to_x(nozzle_scroll_, 0, LV_ANIM_OFF);

  for (int index = 0; index < static_cast<int>(core::kMaximumToolheads); ++index) {
    const bool present = index < count;
    if (present) lv_obj_remove_flag(nozzle_cards_[index], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(nozzle_cards_[index], LV_OBJ_FLAG_HIDDEN);
    if (!present) continue;

    core::ToolheadState fallback;
    const core::ToolheadState* tool = nullptr;
    if (index < snapshot.job.toolhead_count && snapshot.job.toolheads[index].present) {
      tool = &snapshot.job.toolheads[index];
    } else {
      fallback.present = true;
      fallback.active = index == 0;
      fallback.temperature_known = index == 0;
      fallback.temperature_c = index == 0 ? snapshot.job.temperatures.nozzle_c : 0.0F;
      fallback.target_c = index == 0 ? snapshot.job.temperatures.nozzle_target_c : 0.0F;
      tool = &fallback;
    }

    const int card_width = overflow ? kOverflowCardWidth
        : count == 1 ? (kDisplayUsesCompactRoundLayout ? 102 : 108)
        : count == 2 ? (kDisplayUsesCompactRoundLayout ? 86 : 92)
                     : (kDisplayUsesCompactRoundLayout ? 60 : 64);
    const int gap = count == 2 ? (kDisplayUsesCompactRoundLayout ? 8 : 10)
                               : kOverflowGap;
    const int stride = card_width + gap;
    const int occupied_width = count * card_width + (count - 1) * gap;
    const int x = overflow ? index * kOverflowStride
                           : (kViewportWidth - occupied_width) / 2 + index * stride;
    lv_obj_set_width(nozzle_cards_[index], card_width);
    for (auto* label : {nozzle_tool_labels_[index], nozzle_target_labels_[index],
                        nozzle_temperature_labels_[index],
                        nozzle_material_labels_[index]}) {
      lv_obj_set_width(label, card_width - 6);
    }
    lv_obj_set_pos(nozzle_cards_[index], x, 0);
    lv_label_set_text_fmt(nozzle_tool_labels_[index], "T%d", index);
    if (tool->temperature_known) {
      lv_label_set_text_fmt(nozzle_temperature_labels_[index], "%.0f°C",
                            tool->temperature_c);
    } else {
      lv_label_set_text(nozzle_temperature_labels_[index], "--°C");
    }
    lv_label_set_text_fmt(nozzle_target_labels_[index], "%s %.0f°", tr("Target"),
                          tool->target_c);
    const bool empty = tool->filament_state_known && !tool->filament_detected;
    lv_label_set_text(nozzle_material_labels_[index],
                      empty ? "---" : (tool->material.empty() ? "--"
                                                                  : tool->material.c_str()));

    const std::uint32_t filament_color = tool->material_rgba != 0
        ? (tool->material_rgba >> 8U) & 0x00FFFFFFU
        : (tool->active ? theme_colors_.done : theme_style_.text_muted);
    const std::uint32_t icon_color = filament_color == 0 ? theme_style_.track : filament_color;
    const bool loaded = !empty && (tool->filament_detected || !tool->material.empty());
    const std::uint32_t muted = theme_style_.text_muted;
    const std::uint32_t card_border = loaded ? icon_color
                                             : (tool->active ? theme_colors_.done : theme_style_.track);
    lv_obj_set_style_bg_color(nozzle_cards_[index],
                              lv_color_hex(empty ? theme_style_.surface_soft : theme_style_.surface_raised),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nozzle_cards_[index], empty ? LV_OPA_50 : LV_OPA_80,
                            LV_PART_MAIN);
    lv_obj_set_style_border_width(nozzle_cards_[index], tool->active ? 2 : 1,
                                  LV_PART_MAIN);
    lv_obj_set_style_border_color(nozzle_cards_[index], lv_color_hex(card_border),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(nozzle_cards_[index],
                                tool->active ? LV_OPA_COVER
                                             : (loaded ? LV_OPA_60 : LV_OPA_40),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_tool_labels_[index],
                                lv_color_hex(empty ? muted : theme_style_.text_secondary), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_target_labels_[index],
                                lv_color_hex(empty ? muted : theme_style_.text_muted), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_icons_[index],
                                lv_color_hex(empty ? muted : icon_color), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_temperature_labels_[index],
                                lv_color_hex(empty ? muted : theme_style_.text_primary), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_material_labels_[index],
                                lv_color_hex(empty ? muted : theme_style_.text_secondary), LV_PART_MAIN);
    if (empty) {
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(nozzle_material_dots_[index], lv_color_hex(muted),
                                    LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(nozzle_material_dots_[index], lv_color_hex(filament_color),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 0, LV_PART_MAIN);
    }
  }
  lv_bar_set_value(progress_arc_, std::clamp(static_cast<int>(snapshot.job.completion), 0, 100),
                   LV_ANIM_OFF);
  square_update_power_header(power);
  board_display_unlock();
}

void DisplayShell::square_show_printer_compact(const core::PrinterProfile& profile,
                                                const core::PrinterSnapshot& snapshot,
                                                const PowerSnapshot& power) {
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 18 || visible_profile_ != profile.id) {
    prepare_active_screen("compact-details");
    lv_obj_t* screen = lv_screen_active();
    square_create_printer_chrome(profile, snapshot, &power);
    detail_label_ = lv_label_create(screen);
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_14);
    lv_obj_set_size(detail_label_, kDisplayUsesCompactRoundLayout ? 168 : 200, 18);
    lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 43);

    // Compact tool strip: three fixed-width cards fit exactly; a fourth tool
    // enables the one outer horizontal scroller. Cards never scroll alone.
    nozzle_scroll_ = lv_obj_create(screen);
    lv_obj_set_size(nozzle_scroll_, 210, 58);
    lv_obj_align(nozzle_scroll_, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_scroll_dir(nozzle_scroll_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(nozzle_scroll_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(nozzle_scroll_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(nozzle_scroll_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nozzle_scroll_, 0, LV_PART_MAIN);
    lv_obj_set_style_width(nozzle_scroll_, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(nozzle_scroll_, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(nozzle_scroll_, lv_color_hex(accent_color_),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(nozzle_scroll_, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(nozzle_scroll_, nozzle_scroll_event, LV_EVENT_ALL, this);

    nozzle_row_ = square_layout_box(nozzle_scroll_, 210, 50);
    lv_obj_set_pos(nozzle_row_, 0, 0);
    for (std::size_t index = 0; index < core::kMaximumToolheads; ++index) {
      lv_obj_t* card = lv_obj_create(nozzle_row_);
      nozzle_cards_[index] = card;
      lv_obj_set_size(card, 64, 50);
      lv_obj_set_pos(card, static_cast<int>(index) * 70, 0);
      lv_obj_set_style_radius(card, themed_radius(10), LV_PART_MAIN);
      lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_80, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, lv_color_hex(theme_style_.border), LV_PART_MAIN);
      lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
      apply_surface_effect(card);
      square_route_screen_gestures(card);

      nozzle_tool_labels_[index] = lv_label_create(card);
      apply_text_style(nozzle_tool_labels_[index], lv_color_hex(theme_style_.text_secondary),
                       &lv_font_montserrat_12);
      lv_obj_set_width(nozzle_tool_labels_[index], 24);
      lv_obj_set_style_text_align(nozzle_tool_labels_[index], LV_TEXT_ALIGN_LEFT,
                                  LV_PART_MAIN);
      lv_obj_align(nozzle_tool_labels_[index], LV_ALIGN_TOP_LEFT, 4, 1);
      square_route_screen_gestures(nozzle_tool_labels_[index]);

      nozzle_material_dots_[index] = lv_obj_create(card);
      lv_obj_set_size(nozzle_material_dots_[index], 7, 7);
      lv_obj_set_style_radius(nozzle_material_dots_[index], LV_RADIUS_CIRCLE,
                              LV_PART_MAIN);
      lv_obj_set_style_bg_color(nozzle_material_dots_[index], lv_color_hex(theme_style_.text_muted),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(nozzle_material_dots_[index], 0, LV_PART_MAIN);
      lv_obj_align(nozzle_material_dots_[index], LV_ALIGN_TOP_RIGHT, -5, 5);
      square_route_screen_gestures(nozzle_material_dots_[index]);

      lv_obj_t* nozzle_icon_slot = square_mdi_icon_slot(
          card, kMdiNozzle, 18, 22, theme_style_.text_muted, &nozzle_icons_[index]);
      lv_obj_align(nozzle_icon_slot, LV_ALIGN_BOTTOM_LEFT, 4, -2);

      nozzle_temperature_labels_[index] = lv_label_create(card);
      apply_text_style(nozzle_temperature_labels_[index], lv_color_hex(theme_style_.text_primary),
                       &lv_font_montserrat_14);
      lv_obj_set_width(nozzle_temperature_labels_[index], 38);
      lv_obj_set_style_text_align(nozzle_temperature_labels_[index], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_align(nozzle_temperature_labels_[index], LV_ALIGN_TOP_RIGHT, -1, 17);
      square_route_screen_gestures(nozzle_temperature_labels_[index]);

      nozzle_material_labels_[index] = lv_label_create(card);
      apply_text_style(nozzle_material_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_12);
      lv_obj_set_size(nozzle_material_labels_[index], 38, 14);
      lv_label_set_long_mode(nozzle_material_labels_[index], LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_align(nozzle_material_labels_[index], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_align(nozzle_material_labels_[index], LV_ALIGN_BOTTOM_RIGHT, -1, -1);
      square_route_screen_gestures(nozzle_material_labels_[index]);
    }

    layer_label_ = lv_label_create(screen);
    apply_text_style(layer_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_12);
    lv_obj_set_size(layer_label_, 210, 16);
    lv_obj_set_style_text_align(layer_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(layer_label_, LV_ALIGN_TOP_MID, 0, 121);

    // Same three-section temperature band as the primary status page.
    lv_obj_t* temperature_row = square_layout_box(screen, 210, 24);
    lv_obj_align(temperature_row, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_add_flag(temperature_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t* temperature_cells[3]{};
    for (auto*& cell : temperature_cells) {
      cell = square_layout_box(temperature_row, 70, 24);
      lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }
    lv_obj_align(temperature_cells[0], LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(temperature_cells[1], LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(temperature_cells[2], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t* nozzle_icon = square_mdi_icon_slot(temperature_cells[0], kMdiNozzle, 18, 24,
                                                 kNozzleTemperatureColor);
    lv_obj_t* bed_icon = square_mdi_icon_slot(temperature_cells[1], kMdiBed, 18, 24,
                                              kBedTemperatureColor);
    lv_obj_t* chamber_icon = square_thermometer_slot(temperature_cells[2], 18, 24,
                                                     kChamberTemperatureColor);
    for (auto* icon : {nozzle_icon, bed_icon, chamber_icon}) {
      lv_obj_align(icon, LV_ALIGN_LEFT_MID, 1, 0);
    }
    nozzle_temperature_label_ = lv_label_create(temperature_cells[0]);
    bed_temperature_label_ = lv_label_create(temperature_cells[1]);
    chamber_temperature_label_ = lv_label_create(temperature_cells[2]);
    apply_text_style(nozzle_temperature_label_, lv_color_hex(kNozzleTemperatureColor),
                     &lv_font_montserrat_14);
    apply_text_style(bed_temperature_label_, lv_color_hex(kBedTemperatureColor),
                     &lv_font_montserrat_14);
    apply_text_style(chamber_temperature_label_, lv_color_hex(kChamberTemperatureColor),
                     &lv_font_montserrat_14);
    for (auto* label : {nozzle_temperature_label_, bed_temperature_label_,
                        chamber_temperature_label_}) {
      lv_obj_set_size(label, 48, 18);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
      lv_obj_align(label, LV_ALIGN_LEFT_MID, 22, 0);
      square_route_screen_gestures(label);
    }

    // Three timers share one compact two-line band: captions on top and values
    // below. This preserves all useful timing data without a separate clock row.
    lv_obj_t* timers = square_layout_box(screen, 210, 30);
    lv_obj_align(timers, LV_ALIGN_TOP_MID, 0, 165);
    lv_obj_t* timer_columns[3]{};
    for (auto*& column : timer_columns) {
      column = square_layout_box(timers, 70, 30);
    }
    lv_obj_align(timer_columns[0], LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(timer_columns[1], LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(timer_columns[2], LV_ALIGN_RIGHT_MID, 0, 0);

    const char* timer_captions[]{"LEFT", "PRINT", "TOTAL"};
    lv_obj_t* timer_values[3]{};
    remaining_label_ = timer_values[0] = lv_label_create(timer_columns[0]);
    total_time_label_ = timer_values[1] = lv_label_create(timer_columns[1]);
    metrics_label_ = timer_values[2] = lv_label_create(timer_columns[2]);
    for (int index = 0; index < 3; ++index) {
      lv_obj_t* caption = lv_label_create(timer_columns[index]);
      lv_label_set_text(caption, tr(timer_captions[index]));
      apply_text_style(caption, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
      lv_obj_set_width(caption, 70);
      lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 0);
      square_route_screen_gestures(caption);

      apply_text_style(timer_values[index],
                       lv_color_hex(index == 0 ? theme_style_.accent_secondary : theme_style_.text_secondary),
                       &lv_font_montserrat_12);
      lv_obj_set_width(timer_values[index], 70);
      lv_obj_set_style_text_align(timer_values[index], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_align(timer_values[index], LV_ALIGN_BOTTOM_MID, 0, 0);
      square_route_screen_gestures(timer_values[index]);
    }

    status_label_ = lv_label_create(screen);
    apply_text_style(status_label_, lv_color_hex(accent_color_), &lv_font_montserrat_16);
    lv_obj_set_size(status_label_, 210, 20);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 198);
    view_ = 18;
    visible_profile_ = profile.id;
  }
  const std::string display_job_name = core::job_name_for_display(snapshot.job.name);
  lv_label_set_text(detail_label_, display_job_name.empty() ? tr("No active print")
                                                            : display_job_name.c_str());
  int count = snapshot.job.toolhead_count;
  if (count <= 0) count = 1;
  count = std::clamp(count, 1, static_cast<int>(core::kMaximumToolheads));
  constexpr int kViewportWidth = 210;
  constexpr int kCardWidth = 64;
  constexpr int kCardGap = 6;
  constexpr int kStride = kCardWidth + kCardGap;
  const bool overflow = count > 3;
  const int content_width = overflow ? count * kStride - kCardGap : kViewportWidth;
  lv_obj_set_width(nozzle_row_, content_width);
  if (overflow) lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_SCROLLABLE);
  else lv_obj_remove_flag(nozzle_scroll_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_scrollbar_mode(nozzle_scroll_, overflow ? LV_SCROLLBAR_MODE_ON
                                                     : LV_SCROLLBAR_MODE_OFF);
  if (!overflow) lv_obj_scroll_to_x(nozzle_scroll_, 0, LV_ANIM_OFF);
  const std::uint32_t state_color =
      core::phase_color(theme_colors_, snapshot.job.phase, snapshot.job.reachable);
  for (int index = 0; index < static_cast<int>(core::kMaximumToolheads); ++index) {
    const bool present = index < count;
    if (present) lv_obj_remove_flag(nozzle_cards_[index], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(nozzle_cards_[index], LV_OBJ_FLAG_HIDDEN);
    if (!present) continue;

    core::ToolheadState fallback;
    const core::ToolheadState* tool = nullptr;
    if (index < snapshot.job.toolhead_count && snapshot.job.toolheads[index].present) {
      tool = &snapshot.job.toolheads[index];
    } else {
      fallback.present = true;
      fallback.active = index == 0;
      fallback.temperature_known = index == 0;
      fallback.temperature_c = index == 0 ? snapshot.job.temperatures.nozzle_c : 0.0F;
      fallback.target_c = index == 0 ? snapshot.job.temperatures.nozzle_target_c : 0.0F;
      tool = &fallback;
    }
    const int occupied_width = count * kCardWidth + (count - 1) * kCardGap;
    const int x = overflow ? index * kStride
                           : (kViewportWidth - occupied_width) / 2 + index * kStride;
    lv_obj_set_pos(nozzle_cards_[index], x, 0);
    lv_label_set_text_fmt(nozzle_tool_labels_[index], "T%d", index);
    if (tool->temperature_known) {
      lv_label_set_text_fmt(nozzle_temperature_labels_[index], "%.0f°", tool->temperature_c);
    } else {
      lv_label_set_text(nozzle_temperature_labels_[index], "--°");
    }
    const bool empty = tool->filament_state_known && !tool->filament_detected;
    lv_label_set_text(nozzle_material_labels_[index],
                      empty ? "---" : (tool->material.empty() ? "--"
                                                               : tool->material.c_str()));
    const std::uint32_t filament_color = tool->material_rgba != 0
        ? (tool->material_rgba >> 8U) & 0x00FFFFFFU
        : (tool->active ? theme_colors_.done : theme_style_.text_muted);
    const std::uint32_t muted = theme_style_.text_muted;
    lv_obj_set_style_bg_opa(nozzle_cards_[index], empty ? LV_OPA_50 : LV_OPA_80,
                            LV_PART_MAIN);
    lv_obj_set_style_border_width(nozzle_cards_[index], tool->active ? 2 : 1,
                                  LV_PART_MAIN);
    lv_obj_set_style_border_color(nozzle_cards_[index],
                                  lv_color_hex(tool->active ? state_color : theme_style_.border),
                                  LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_tool_labels_[index],
                                lv_color_hex(empty ? muted : theme_style_.text_secondary), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_icons_[index],
                                lv_color_hex(empty ? muted : filament_color), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_temperature_labels_[index],
                                lv_color_hex(empty ? muted : theme_style_.text_primary), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_material_labels_[index],
                                lv_color_hex(empty ? muted : theme_style_.text_muted), LV_PART_MAIN);
    if (empty) {
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 1, LV_PART_MAIN);
      lv_obj_set_style_border_color(nozzle_material_dots_[index], lv_color_hex(muted),
                                    LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(nozzle_material_dots_[index], lv_color_hex(filament_color),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 0, LV_PART_MAIN);
    }
  }

  lv_label_set_text_fmt(nozzle_temperature_label_, "%.0f°C",
                        snapshot.job.temperatures.nozzle_c);
  lv_label_set_text_fmt(bed_temperature_label_, "%.0f°C", snapshot.job.temperatures.bed_c);
  if (snapshot.job.temperatures.chamber_known) {
    lv_label_set_text_fmt(chamber_temperature_label_, "%.0f°C",
                          snapshot.job.temperatures.chamber_c);
  } else {
    lv_label_set_text(chamber_temperature_label_, "--°C");
  }
  lv_label_set_text_fmt(layer_label_, "%s: %u / %u", tr("Layer"),
                        snapshot.job.current_layer, snapshot.job.total_layers);
  const bool active_job = snapshot.job.phase == core::JobPhase::printing ||
                          snapshot.job.phase == core::JobPhase::preparing ||
                          snapshot.job.phase == core::JobPhase::paused;
  lv_label_set_text(remaining_label_, active_job
        ? short_duration(snapshot.job.remaining_seconds).c_str() : "--");
  lv_label_set_text(total_time_label_, snapshot.job.elapsed_seconds > 0
        ? short_duration(snapshot.job.elapsed_seconds).c_str() : "--");
  const std::uint32_t total_seconds =
      snapshot.job.elapsed_seconds + snapshot.job.remaining_seconds;
  lv_label_set_text(metrics_label_, total_seconds > 0
        ? short_duration(total_seconds).c_str() : "--");
  lv_label_set_text(status_label_, tr(core::job_status_label(snapshot.job)));
  lv_obj_set_style_text_color(status_label_, lv_color_hex(state_color), LV_PART_MAIN);
  const int progress = std::clamp(static_cast<int>(snapshot.job.completion), 0, 100);
  lv_bar_set_value(progress_arc_, progress, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_colors_.printing),
                            LV_PART_INDICATOR);
  lv_label_set_text_fmt(progress_label_, "%d%%", progress);
  lv_obj_set_style_text_color(progress_label_, lv_color_hex(theme_colors_.printing),
                              LV_PART_MAIN);
  square_update_power_header(power);
  board_display_unlock();
}

void DisplayShell::square_show_printer_telemetry(const core::PrinterProfile& profile,
                                                  const core::PrinterSnapshot& snapshot,
                                                  const PowerSnapshot& power) {
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 20 || visible_profile_ != profile.id) {
    prepare_active_screen("speeds");
    lv_obj_t* screen = lv_screen_active();
    square_create_printer_chrome(profile, snapshot, &power);
    detail_label_ = lv_label_create(screen);
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
    // Reserve a real gutter for the vertical page indicator.  Keeping every
    // section on the same 200 px content grid also makes their centres line up.
    lv_obj_set_size(detail_label_, kDisplayUsesCompactRoundLayout ? 168 : 200, 18);
    lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0,
                 kDisplayUsesCompactRoundLayout ? 47 : 43);
    constexpr const char* captions[]{"SPEED", "FLOW", "FAN"};
    const std::uint32_t colors[]{theme_style_.accent_secondary, theme_colors_.done,
                                 theme_colors_.preparing};
    constexpr int kMetricCardWidth =
        kDisplayUsesCompactRoundLayout ? 62 : 66;
    constexpr int kMetricCardSpacing =
        kDisplayUsesCompactRoundLayout ? 65 : 69;
    for (int index = 0; index < 3; ++index) {
      telemetry_metric_cards_[index] = lv_obj_create(screen);
      lv_obj_set_size(telemetry_metric_cards_[index], kMetricCardWidth, 62);
      lv_obj_align(telemetry_metric_cards_[index], LV_ALIGN_TOP_MID,
                   -kMetricCardSpacing + index * kMetricCardSpacing, 64);
      lv_obj_set_style_radius(telemetry_metric_cards_[index], themed_radius(12), LV_PART_MAIN);
      lv_obj_set_style_bg_color(telemetry_metric_cards_[index], lv_color_hex(theme_style_.surface_raised),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(telemetry_metric_cards_[index], LV_OPA_80, LV_PART_MAIN);
      lv_obj_set_style_border_width(telemetry_metric_cards_[index], 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(telemetry_metric_cards_[index], lv_color_hex(colors[index]),
                                    LV_PART_MAIN);
      lv_obj_set_style_pad_all(telemetry_metric_cards_[index], 0, LV_PART_MAIN);
      apply_surface_effect(telemetry_metric_cards_[index]);
      square_route_screen_gestures(telemetry_metric_cards_[index]);
      telemetry_metric_caption_labels_[index] = lv_label_create(telemetry_metric_cards_[index]);
      telemetry_metric_value_labels_[index] = lv_label_create(telemetry_metric_cards_[index]);
      lv_label_set_text(telemetry_metric_caption_labels_[index], tr(captions[index]));
      apply_text_style(telemetry_metric_caption_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_12);
      apply_text_style(telemetry_metric_value_labels_[index], lv_color_hex(colors[index]),
                       &lv_font_montserrat_16);
      for (auto* label : {telemetry_metric_caption_labels_[index],
                          telemetry_metric_value_labels_[index]}) {
        lv_obj_set_width(label, kMetricCardWidth - 2);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        square_route_screen_gestures(label);
      }
      lv_label_set_long_mode(telemetry_metric_caption_labels_[index], LV_LABEL_LONG_DOT);
      lv_obj_align(telemetry_metric_caption_labels_[index], LV_ALIGN_TOP_MID, 0, 7);
      lv_obj_align(telemetry_metric_value_labels_[index], LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    temperature_label_ = lv_label_create(screen);
    apply_text_style(temperature_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_14);
    lv_obj_set_size(temperature_label_, 200, 18);
    lv_obj_set_style_text_align(temperature_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(temperature_label_, LV_ALIGN_TOP_MID, 0,
                 kDisplayUsesCompactRoundLayout ? 132 : 128);

    lv_obj_t* details = square_layout_box(screen, 200, 34);
    lv_obj_align(details, LV_ALIGN_TOP_MID, 0, 146);
    lv_obj_t* detail_columns[2]{};
    for (auto*& column : detail_columns) {
      column = square_layout_box(details, 96, 34);
    }
    lv_obj_align(detail_columns[0], LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(detail_columns[1], LV_ALIGN_RIGHT_MID, 0, 0);
    for (int index = 0; index < 2; ++index) {
      telemetry_detail_caption_labels_[index] = lv_label_create(detail_columns[index]);
      apply_text_style(telemetry_detail_caption_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_12);
      // Power captions must remain a single visual row.  A slightly condensed
      // 100 px label still fits inside the shared 200 px grid and leaves the
      // pager gutter untouched; longer translations are ellipsized, not wrapped.
      lv_obj_set_size(telemetry_detail_caption_labels_[index], 100, 14);
      lv_obj_set_style_text_letter_space(telemetry_detail_caption_labels_[index], -1,
                                         LV_PART_MAIN);
      lv_obj_set_style_text_align(telemetry_detail_caption_labels_[index],
                                  LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_label_set_long_mode(telemetry_detail_caption_labels_[index], LV_LABEL_LONG_DOT);
      lv_obj_align(telemetry_detail_caption_labels_[index], LV_ALIGN_TOP_MID, 0, 0);
      square_route_screen_gestures(telemetry_detail_caption_labels_[index]);
    }
    nozzle_temperature_label_ = lv_label_create(detail_columns[0]);
    bed_temperature_label_ = lv_label_create(detail_columns[1]);
    apply_text_style(nozzle_temperature_label_, lv_color_hex(kNozzleTemperatureColor),
                     &lv_font_montserrat_16);
    apply_text_style(bed_temperature_label_, lv_color_hex(kBedTemperatureColor),
                     &lv_font_montserrat_16);
    for (auto* label : {nozzle_temperature_label_, bed_temperature_label_}) {
      lv_obj_set_width(label, 96);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0,
                   kDisplayUsesCompactRoundLayout ? -3 : 0);
      square_route_screen_gestures(label);
    }

    layer_label_ = lv_label_create(screen);
    apply_text_style(layer_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
    lv_obj_set_size(layer_label_, 200, 16);
    lv_obj_set_style_text_align(layer_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(layer_label_, LV_ALIGN_TOP_MID, 0, 180);

    status_label_ = lv_label_create(screen);
    apply_text_style(status_label_, lv_color_hex(accent_color_), &lv_font_montserrat_14);
    lv_obj_set_size(status_label_, 200, 18);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 198);
    view_ = 20;
    visible_profile_ = profile.id;
  }
  const auto& motion = snapshot.job.motion;
  const int active_tool = snapshot.job.active_toolhead >= 0
                              ? snapshot.job.active_toolhead : 0;
  const bool nozzle_power_known = active_tool < snapshot.job.toolhead_count &&
      snapshot.job.toolheads[active_tool].present &&
      snapshot.job.toolheads[active_tool].heater_power_known;
  const bool detailed_motion = motion.velocity_known || motion.position_known ||
                               nozzle_power_known || snapshot.job.bed_heater_power_known;
  const bool bambu_summary = profile.protocol == core::PrinterProtocol::bambu_lan &&
                             !detailed_motion;
  if (bambu_summary) {
    const std::string summary = snapshot.job.remaining_seconds > 0
        ? short_duration(snapshot.job.remaining_seconds) + " " + tr("remaining")
        : tr("Local printer telemetry");
    lv_label_set_text(detail_label_, summary.c_str());
    lv_label_set_text(telemetry_metric_caption_labels_[0], tr("SPEED"));
    lv_label_set_text(telemetry_metric_caption_labels_[1], tr("FAN"));
    lv_label_set_text(telemetry_metric_caption_labels_[2], tr("PROGRESS"));
    if (motion.speed_multiplier_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[0], "%.0f%%",
                            motion.speed_multiplier);
    } else lv_label_set_text(telemetry_metric_value_labels_[0], "--%");
    if (motion.fan_percent_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[1], "%.0f%%", motion.fan_percent);
    } else lv_label_set_text(telemetry_metric_value_labels_[1], "--%");
    lv_label_set_text_fmt(telemetry_metric_value_labels_[2], "%.0f%%",
                          snapshot.job.completion);
    if (snapshot.job.total_layers > 0) {
      lv_label_set_text_fmt(temperature_label_, "%s: %u / %u", tr("LAYER"),
                            snapshot.job.current_layer, snapshot.job.total_layers);
    } else {
      lv_label_set_text_fmt(temperature_label_, "%s: -- / --", tr("LAYER"));
    }
    lv_label_set_text(telemetry_detail_caption_labels_[0], tr("NOZZLE TEMP"));
    lv_label_set_text(telemetry_detail_caption_labels_[1], tr("BED TEMP"));
    lv_label_set_text_fmt(nozzle_temperature_label_, "%.0f° / %.0f°",
                          snapshot.job.temperatures.nozzle_c,
                          snapshot.job.temperatures.nozzle_target_c);
    lv_label_set_text_fmt(bed_temperature_label_, "%.0f° / %.0f°",
                          snapshot.job.temperatures.bed_c,
                          snapshot.job.temperatures.bed_target_c);
    if (snapshot.job.temperatures.chamber_known) {
      lv_label_set_text_fmt(layer_label_, "%s: %.0f°C", tr("CHAMBER"),
                            snapshot.job.temperatures.chamber_c);
    } else {
      lv_label_set_text(layer_label_, tr("ACTUAL / TARGET"));
    }
  } else {
    lv_label_set_text(telemetry_metric_caption_labels_[0], tr("SPEED"));
    lv_label_set_text(telemetry_metric_caption_labels_[1], tr("FLOW"));
    lv_label_set_text(telemetry_metric_caption_labels_[2], tr("FAN"));
    if (motion.velocity_known) {
      lv_label_set_text_fmt(detail_label_, "%.1f mm/s", motion.velocity_mm_s);
    } else lv_label_set_text(detail_label_, "-- mm/s");
    if (motion.speed_multiplier_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[0], "%.0f%%",
                            motion.speed_multiplier);
    } else lv_label_set_text(telemetry_metric_value_labels_[0], "--%");
    if (motion.extrusion_multiplier_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[1], "%.0f%%",
                            motion.extrusion_multiplier);
    } else lv_label_set_text(telemetry_metric_value_labels_[1], "--%");
    if (motion.fan_percent_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[2], "%.0f%%", motion.fan_percent);
    } else lv_label_set_text(telemetry_metric_value_labels_[2], "--%");
    if (motion.position_known) {
      lv_label_set_text_fmt(temperature_label_, "X %.1f   Y %.1f   Z %.2f",
                            motion.x_mm, motion.y_mm, motion.z_mm);
    } else lv_label_set_text(temperature_label_, "X --   Y --   Z --");
    lv_label_set_text(telemetry_detail_caption_labels_[0], tr("NOZZLE POWER"));
    lv_label_set_text(telemetry_detail_caption_labels_[1], tr("BED POWER"));
    if (nozzle_power_known) {
      lv_label_set_text_fmt(nozzle_temperature_label_, "%.0f%%",
                            snapshot.job.toolheads[active_tool].heater_power * 100.0F);
    } else lv_label_set_text(nozzle_temperature_label_, "--%");
    if (snapshot.job.bed_heater_power_known) {
      lv_label_set_text_fmt(bed_temperature_label_, "%.0f%%",
                            snapshot.job.bed_heater_power * 100.0F);
    } else lv_label_set_text(bed_temperature_label_, "--%");
    std::string homed = motion.homed_axes;
    for (char& character : homed) {
      if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
    }
    lv_label_set_text_fmt(layer_label_, "%s: %s", tr("HOMED"),
                          homed.empty() ? "--" : homed.c_str());
  }
  lv_label_set_text(status_label_, tr(core::job_status_label(snapshot.job)));
  const std::uint32_t state_color =
      core::phase_color(theme_colors_, snapshot.job.phase, snapshot.job.reachable);
  lv_obj_set_style_text_color(status_label_, lv_color_hex(state_color), LV_PART_MAIN);
  const int progress = std::clamp(static_cast<int>(snapshot.job.completion), 0, 100);
  lv_bar_set_value(progress_arc_, progress, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_colors_.printing),
                            LV_PART_INDICATOR);
  lv_label_set_text_fmt(progress_label_, "%d%%", progress);
  lv_obj_set_style_text_color(progress_label_, lv_color_hex(theme_colors_.printing),
                              LV_PART_MAIN);
  square_update_power_header(power);
  board_display_unlock();
}

void DisplayShell::square_show_printer_materials(const core::PrinterProfile& profile,
                                                  const core::PrinterSnapshot& snapshot) {
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 21 || visible_profile_ != profile.id) {
    prepare_active_screen("ams-lite");
    square_create_header("AMS LITE");
    lv_obj_t* screen = lv_screen_active();

    // Four equal material cells on a 202 px grid.  The right edge stops before
    // the navigation gutter and the bottom row leaves room for one symmetric
    // external-spool section plus the depth indicator.
    for (int index = 0; index < 4; ++index) {
      lv_obj_t* card = lv_obj_create(screen);
      material_cards_[index] = card;
      lv_obj_set_size(card, kDisplayUsesCompactRoundLayout ? 86 : 96, 64);
      lv_obj_align(card, LV_ALIGN_TOP_MID,
                   (index % 2 == 0
                        ? (kDisplayUsesCompactRoundLayout ? -46 : -53)
                        : (kDisplayUsesCompactRoundLayout ? 46 : 53)),
                   (kDisplayUsesCompactRoundLayout ? 48 : 42) +
                       (index / 2) * 68);
      lv_obj_set_style_radius(card, themed_radius(14), LV_PART_MAIN);
      lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_70, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, lv_color_hex(theme_style_.track), LV_PART_MAIN);
      lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
      apply_surface_effect(card);
      square_route_screen_gestures(card);

      material_slot_labels_[index] = lv_label_create(card);
      material_name_labels_[index] = lv_label_create(card);
      material_percent_labels_[index] = lv_label_create(card);
      material_feed_labels_[index] = lv_label_create(card);
      lv_label_set_text_fmt(material_slot_labels_[index], "%d", index + 1);
      apply_text_style(material_slot_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_12);
      apply_text_style(material_name_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_14);
      apply_text_style(material_percent_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_12);
      apply_text_style(material_feed_labels_[index], lv_color_hex(accent_color_),
                       &lv_font_montserrat_14);

      lv_obj_set_width(material_slot_labels_[index], 24);
      lv_obj_set_style_text_align(material_slot_labels_[index], LV_TEXT_ALIGN_LEFT,
                                  LV_PART_MAIN);
      lv_obj_align(material_slot_labels_[index], LV_ALIGN_TOP_LEFT, 8, 5);

      lv_obj_set_width(material_feed_labels_[index], 24);
      lv_obj_set_style_text_align(material_feed_labels_[index], LV_TEXT_ALIGN_RIGHT,
                                  LV_PART_MAIN);
      lv_obj_align(material_feed_labels_[index], LV_ALIGN_TOP_RIGHT, -8, 4);

      lv_obj_set_width(material_name_labels_[index],
                       kDisplayUsesCompactRoundLayout ? 74 : 82);
      lv_label_set_long_mode(material_name_labels_[index], LV_LABEL_LONG_DOT);
      lv_obj_align(material_name_labels_[index], LV_ALIGN_CENTER, 0, -1);

      lv_obj_set_width(material_percent_labels_[index],
                       kDisplayUsesCompactRoundLayout ? 74 : 82);
      lv_obj_align(material_percent_labels_[index], LV_ALIGN_BOTTOM_MID, 0, -5);

      for (auto* label : {material_slot_labels_[index], material_name_labels_[index],
                          material_percent_labels_[index], material_feed_labels_[index]}) {
        square_route_screen_gestures(label);
      }
    }

    external_material_card_ = lv_obj_create(screen);
    lv_obj_set_size(external_material_card_,
                    kDisplayUsesCompactRoundLayout ? 158 : 202,
                    kDisplayUsesCompactRoundLayout ? 28 : 30);
    lv_obj_align(external_material_card_, LV_ALIGN_TOP_MID, 0,
                 kDisplayUsesCompactRoundLayout ? 184 : 185);
    lv_obj_set_style_radius(external_material_card_, themed_radius(15), LV_PART_MAIN);
    lv_obj_set_style_bg_color(external_material_card_, lv_color_hex(theme_style_.surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(external_material_card_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(external_material_card_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(external_material_card_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_pad_all(external_material_card_, 0, LV_PART_MAIN);
    apply_surface_effect(external_material_card_);
    square_route_screen_gestures(external_material_card_);

    external_material_dot_ = lv_obj_create(external_material_card_);
    lv_obj_set_size(external_material_dot_, 12, 12);
    lv_obj_align(external_material_dot_, LV_ALIGN_LEFT_MID, 11, 0);
    lv_obj_set_style_radius(external_material_dot_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(external_material_dot_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_border_width(external_material_dot_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(external_material_dot_, 0, LV_PART_MAIN);
    square_route_screen_gestures(external_material_dot_);

    external_material_label_ = lv_label_create(external_material_card_);
    apply_text_style(external_material_label_, lv_color_hex(theme_style_.text_muted),
                     &lv_font_montserrat_12);
    lv_obj_set_width(external_material_label_,
                     kDisplayUsesCompactRoundLayout ? 124 : 165);
    lv_label_set_long_mode(external_material_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(external_material_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(external_material_label_, LV_ALIGN_LEFT_MID, 32, 0);
    square_route_screen_gestures(external_material_label_);

    create_depth_dots(4);
    view_ = 21;
    visible_profile_ = profile.id;
  }
  for (std::size_t index = 0; index < material_cards_.size(); ++index) {
    const core::MaterialSlot* slot = index < snapshot.job.materials.slots.size()
        ? &snapshot.job.materials.slots[index] : nullptr;
    const bool installed = slot != nullptr && slot->installed;
    const std::uint32_t color = installed && slot->rgba != 0
                                    ? (slot->rgba >> 8U) & 0xFFFFFFU
                                    : theme_style_.surface_soft;
    const std::uint32_t red = (color >> 16U) & 0xFFU;
    const std::uint32_t green = (color >> 8U) & 0xFFU;
    const std::uint32_t blue = color & 0xFFU;
    const bool light = red * 299U + green * 587U + blue * 114U > 150000U;
    const std::uint32_t text_color = installed ? (light ? theme_style_.surface : 0xFFFFFF) : theme_style_.text_muted;
    lv_obj_set_style_bg_color(material_cards_[index], lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(material_cards_[index], installed ? LV_OPA_COVER : LV_OPA_60,
                            LV_PART_MAIN);
    lv_obj_set_style_border_color(material_cards_[index],
        lv_color_hex(slot != nullptr && slot->feeding
                         ? accent_color_ : installed ? theme_style_.text_secondary : theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_border_width(material_cards_[index],
                                  slot != nullptr && slot->feeding ? 3 : 2, LV_PART_MAIN);
    for (auto* label : {material_slot_labels_[index], material_name_labels_[index],
                        material_percent_labels_[index]}) {
      lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    }
    lv_label_set_text(material_name_labels_[index], installed
        ? (slot->material.empty() ? "FIL" : slot->material.c_str()) : tr("EMPTY"));
    if (installed && slot->remaining_percent >= 0) {
      lv_label_set_text_fmt(material_percent_labels_[index], "%d%%", slot->remaining_percent);
    } else lv_label_set_text(material_percent_labels_[index], "--");
    lv_label_set_text(material_feed_labels_[index], slot != nullptr && slot->feeding
                                                    ? LV_SYMBOL_UP : "");
  }
  const auto& external = snapshot.job.materials.external_spool;
  const std::uint32_t external_color = external.installed && external.rgba != 0
                                           ? (external.rgba >> 8U) & 0xFFFFFFU
                                           : theme_style_.track;
  lv_obj_set_style_bg_color(external_material_dot_, lv_color_hex(external_color), LV_PART_MAIN);
  lv_obj_set_style_border_color(external_material_card_,
                                lv_color_hex(external.feeding ? accent_color_ : theme_style_.track),
                                LV_PART_MAIN);
  lv_obj_set_style_border_width(external_material_card_, external.feeding ? 3 : 1,
                                LV_PART_MAIN);
  lv_obj_set_style_text_color(external_material_label_,
                              lv_color_hex(external.installed ? theme_style_.text_secondary : theme_style_.text_muted),
                              LV_PART_MAIN);
  if (external.installed) {
    const char* material = external.material.empty() ? tr("Material") : external.material.c_str();
    if (external.remaining_percent >= 0) {
      lv_label_set_text_fmt(external_material_label_, "%s  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET "  %d%%",
                            tr("External"), material, external.remaining_percent);
    } else {
      lv_label_set_text_fmt(external_material_label_, "%s  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET "  --",
                            tr("External"), material);
    }
  } else {
    lv_label_set_text_fmt(external_material_label_, "%s  " LV_SYMBOL_BULLET "  %s",
                          tr("External spool"), tr("Not detected"));
  }
  board_display_unlock();
}

void DisplayShell::square_show_printer_camera(const core::PrinterProfile& profile,
                                               const core::PrinterSnapshot& snapshot,
                                               const PowerSnapshot& power) {
  if (board_display_lock(1000) != ESP_OK) return;
  const bool frame_changed = snapshot.job.camera_frame &&
      camera_pixels_.get() != snapshot.job.camera_frame.get();
  const bool refresh_completed = camera_was_refreshing_ &&
      !snapshot.job.camera_refreshing && snapshot.job.camera_frame &&
      !snapshot.job.camera_frame->empty();
  camera_was_refreshing_ = snapshot.job.camera_refreshing;
  if (frame_changed || refresh_completed) {
    camera_activity_updated_until_us_ = esp_timer_get_time() + 800000;
    view_ = -1;
  }
  if (view_ != 22 || visible_profile_ != profile.id) {
    prepare_active_screen("local-camera");
    square_create_printer_chrome(profile, snapshot, &power);
    lv_label_set_text(title_label_, tr("CAMERA"));

    // Keep the camera description above the image.  The two equal gaps around
    // it visually separate both the progress bar and the camera frame.
    detail_label_ = lv_label_create(lv_screen_active());
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
    lv_obj_set_size(detail_label_, kDisplayUsesCompactRoundLayout ? 168 : 210, 16);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 44);
    square_route_screen_gestures(detail_label_);

    media_image_ = lv_image_create(lv_screen_active());
    lv_obj_set_size(media_image_, 220, 124);
    lv_image_set_inner_align(media_image_, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(media_image_, LV_ALIGN_TOP_MID, 0, 64);
    square_route_screen_gestures(media_image_);
    camera_spinner_ = lv_spinner_create(lv_screen_active());
    lv_obj_set_size(camera_spinner_, 44, 44);
    lv_obj_align(camera_spinner_, LV_ALIGN_TOP_MID, 0, 104);
    square_route_screen_gestures(camera_spinner_);

    camera_empty_label_ = lv_label_create(lv_screen_active());
    apply_text_style(camera_empty_label_, lv_color_hex(theme_style_.text_secondary),
                     &lv_font_montserrat_12);
    lv_obj_set_size(camera_empty_label_, 198, 78);
    lv_label_set_long_mode(camera_empty_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(camera_empty_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(camera_empty_label_, 5, LV_PART_MAIN);
    lv_obj_align(camera_empty_label_, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_add_flag(camera_empty_label_, LV_OBJ_FLAG_HIDDEN);
    square_route_screen_gestures(camera_empty_label_);

    // Snapshot refresh state mirrors the round camera view: a small activity
    // marker and its identifier sit below the image, aligned to its left edge.
    camera_activity_dot_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(camera_activity_dot_, 8, 8);
    lv_obj_align(camera_activity_dot_, LV_ALIGN_TOP_LEFT,
                 kDisplayUsesCompactRoundLayout ? 31 : 13, 195);
    lv_obj_set_style_radius(camera_activity_dot_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(camera_activity_dot_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(camera_activity_dot_, 0, LV_PART_MAIN);
    square_route_screen_gestures(camera_activity_dot_);

    camera_activity_label_ = lv_label_create(lv_screen_active());
    apply_text_style(camera_activity_label_, lv_color_hex(theme_style_.text_muted),
                     &lv_font_montserrat_12);
    lv_obj_set_size(camera_activity_label_,
                    kDisplayUsesCompactRoundLayout ? 96 : 112, 16);
    lv_label_set_long_mode(camera_activity_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(camera_activity_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(camera_activity_label_, LV_ALIGN_TOP_LEFT,
                 kDisplayUsesCompactRoundLayout ? 45 : 27, 191);
    if constexpr (kDisplayUsesCompactRoundLayout) {
      // The font's visible glyphs sit slightly above the centre of its LVGL
      // box. Align the marker to that optical centre, not only the box centre.
      lv_obj_align_to(camera_activity_dot_, camera_activity_label_,
                      LV_ALIGN_OUT_LEFT_MID, -6, -2);
    }
    square_route_screen_gestures(camera_activity_label_);

    camera_mode_row_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(camera_mode_row_,
                    kDisplayUsesCompactRoundLayout ? 148 : 180, 32);
    lv_obj_align(camera_mode_row_, LV_ALIGN_BOTTOM_MID, 0,
                 kDisplayUsesCompactRoundLayout ? -18 : -16);
    lv_obj_set_style_radius(camera_mode_row_, themed_radius(16), LV_PART_MAIN);
    lv_obj_set_style_bg_color(camera_mode_row_, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
    lv_obj_set_style_border_width(camera_mode_row_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(camera_mode_row_, 2, LV_PART_MAIN);
    apply_surface_effect(camera_mode_row_);
    lv_obj_set_flex_flow(camera_mode_row_, LV_FLEX_FLOW_ROW);
    // Capability detection is asynchronous, so never paint the selector until
    // a snapshot explicitly confirms that live mode is available.
    lv_obj_add_flag(camera_mode_row_, LV_OBJ_FLAG_HIDDEN);
    camera_snapshot_button_ = lv_button_create(camera_mode_row_);
    camera_live_button_ = lv_button_create(camera_mode_row_);
    for (auto* button : {camera_snapshot_button_, camera_live_button_}) {
      lv_obj_set_height(button, LV_PCT(100));
      lv_obj_set_flex_grow(button, 1);
      lv_obj_set_style_radius(button, themed_radius(14), LV_PART_MAIN);
      lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
      lv_obj_add_event_cb(button, camera_mode_event, LV_EVENT_CLICKED, this);
    }
    lv_obj_t* snapshot_label = lv_label_create(camera_snapshot_button_);
    lv_label_set_text(snapshot_label, tr("Snapshots"));
    apply_text_style(snapshot_label, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_12);
    lv_obj_center(snapshot_label);
    lv_obj_t* live_label = lv_label_create(camera_live_button_);
    lv_label_set_text(live_label, tr("Live"));
    apply_text_style(live_label, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_12);
    lv_obj_center(live_label);
    view_ = 22;
    visible_profile_ = profile.id;
  }
  const bool live = snapshot.job.camera_live_supported && camera_live_mode_.load();
  const bool recently_updated = esp_timer_get_time() < camera_activity_updated_until_us_;
  const bool refreshing = snapshot.job.camera_refreshing;
  if (live || !snapshot.job.camera_frame || snapshot.job.camera_frame->empty()) {
    lv_obj_add_flag(camera_activity_dot_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(camera_activity_label_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(camera_activity_dot_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(camera_activity_label_, LV_OBJ_FLAG_HIDDEN);
    const std::uint32_t activity_color =
        (refreshing || recently_updated) ? accent_color_ : theme_style_.track;
    lv_obj_set_style_bg_color(camera_activity_dot_, lv_color_hex(activity_color), LV_PART_MAIN);
    lv_obj_set_style_text_color(camera_activity_label_, lv_color_hex(
        (refreshing || recently_updated) ? accent_color_ : theme_style_.text_muted), LV_PART_MAIN);
    lv_label_set_text(camera_activity_label_, refreshing
        ? tr("Refreshing…") : recently_updated ? tr("Updated") : tr("Waiting…"));
  }
  if (snapshot.job.camera_live_supported) {
    lv_obj_remove_flag(camera_mode_row_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(camera_snapshot_button_,
                              lv_color_hex(live ? theme_style_.surface_soft : accent_color_), LV_PART_MAIN);
    lv_obj_set_style_bg_color(camera_live_button_,
                              lv_color_hex(live ? accent_color_ : theme_style_.surface_soft), LV_PART_MAIN);
  } else lv_obj_add_flag(camera_mode_row_, LV_OBJ_FLAG_HIDDEN);
  if (snapshot.job.camera_frame && !snapshot.job.camera_frame->empty() &&
      snapshot.job.camera_width > 0 && snapshot.job.camera_height > 0) {
    camera_pixels_ = snapshot.job.camera_frame;
    camera_image_dsc_ = {};
    camera_image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    camera_image_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    camera_image_dsc_.header.w = snapshot.job.camera_width;
    camera_image_dsc_.header.h = snapshot.job.camera_height;
    camera_image_dsc_.header.stride = snapshot.job.camera_width * sizeof(std::uint16_t);
    camera_image_dsc_.data_size = static_cast<std::uint32_t>(camera_pixels_->size());
    camera_image_dsc_.data = camera_pixels_->data();
    lv_image_set_src(media_image_, &camera_image_dsc_);
    lv_obj_add_flag(camera_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(camera_empty_label_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail_label_, live ? tr("Live local stream")
                                          : tr("Live local snapshot"));
  } else {
    lv_image_set_src(media_image_, nullptr);
    const bool rtsps_unsupported =
        snapshot.job.camera_detail == "This display does not support RTSPS cameras";
    const bool detection_failed = snapshot.job.camera_detail == "No camera detected";
    if (rtsps_unsupported) {
      lv_obj_add_flag(camera_spinner_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(camera_empty_label_, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(camera_empty_label_,
                        tr("This display does not support RTSPS cameras"));
      lv_label_set_text(detail_label_, tr("Camera unavailable"));
    } else if (detection_failed) {
      lv_obj_add_flag(camera_spinner_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(camera_empty_label_, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(camera_empty_label_,
                        tr("No camera detected\nHave a camera? Contact support."));
      lv_label_set_text(detail_label_, tr("Camera unavailable"));
    } else {
      lv_obj_remove_flag(camera_spinner_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(camera_empty_label_, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(detail_label_, tr("Detecting camera…"));
    }
  }
  square_update_power_header(power);
  board_display_unlock();
}

void DisplayShell::square_show_printer_light(const core::PrinterProfile& profile,
                                              const core::PrinterSnapshot& snapshot) {
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 23 || visible_profile_ != profile.id) {
    prepare_active_screen("printer-light");
    square_create_header("PRINTER LIGHT");
    lv_obj_t* screen = lv_screen_active();

    chamber_light_bulb_ = lv_obj_create(screen);
    lv_obj_set_size(chamber_light_bulb_, 88, 88);
    lv_obj_align(chamber_light_bulb_, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_radius(chamber_light_bulb_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(chamber_light_bulb_, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chamber_light_bulb_, 0, LV_PART_MAIN);
    square_route_screen_gestures(chamber_light_bulb_);

    lv_obj_t* bulb_base = lv_obj_create(screen);
    lv_obj_set_size(bulb_base, 40, 22);
    lv_obj_align_to(bulb_base, chamber_light_bulb_, LV_ALIGN_OUT_BOTTOM_MID, 0, -7);
    lv_obj_set_style_radius(bulb_base, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bulb_base, lv_color_hex(theme_style_.text_muted), LV_PART_MAIN);
    lv_obj_set_style_border_width(bulb_base, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bulb_base, 0, LV_PART_MAIN);
    square_route_screen_gestures(bulb_base);

    detail_label_ = lv_label_create(screen);
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_14);
    lv_obj_set_size(detail_label_, 200, 18);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 153);
    square_route_screen_gestures(detail_label_);

    chamber_light_button_ = lv_button_create(screen);
    lv_obj_set_size(chamber_light_button_, 150, 42);
    lv_obj_align(chamber_light_button_, LV_ALIGN_BOTTOM_MID, 0,
                 kDisplayUsesCompactRoundLayout ? -28 : -20);
    // Keep the visual size unchanged, but accept taps that land near the
    // rounded edge. This matters on the 240 px touch panel where a finger can
    // cover the button while the reported point is just outside its bounds.
    lv_obj_set_ext_click_area(chamber_light_button_, 10);
    lv_obj_set_style_radius(chamber_light_button_, themed_radius(18), LV_PART_MAIN);
    lv_obj_set_style_border_width(chamber_light_button_, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(chamber_light_button_, 0, LV_PART_MAIN);
    lv_obj_add_flag(chamber_light_button_, LV_OBJ_FLAG_CHECKABLE);
    square_route_screen_gestures(chamber_light_button_, true);

    chamber_light_button_label_ = lv_label_create(chamber_light_button_);
    apply_text_style(chamber_light_button_label_, lv_color_hex(theme_style_.surface),
                     &lv_font_montserrat_14);
    lv_obj_set_width(chamber_light_button_label_, 112);
    lv_label_set_long_mode(chamber_light_button_label_, LV_LABEL_LONG_DOT);
    lv_obj_center(chamber_light_button_label_);
    square_route_screen_gestures(chamber_light_button_label_);

    chamber_light_spinner_ = lv_spinner_create(chamber_light_button_);
    lv_obj_set_size(chamber_light_spinner_, 22, 22);
    lv_obj_set_style_arc_width(chamber_light_spinner_, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(chamber_light_spinner_, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(chamber_light_spinner_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_arc_color(chamber_light_spinner_, lv_color_hex(theme_style_.surface),
                               LV_PART_INDICATOR);
    lv_obj_align(chamber_light_spinner_, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_add_flag(chamber_light_spinner_, LV_OBJ_FLAG_HIDDEN);
    square_route_screen_gestures(chamber_light_spinner_);

    lv_obj_add_event_cb(chamber_light_button_, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      const bool enabled = lv_obj_has_state(lv_event_get_current_target_obj(event), LV_STATE_CHECKED);
      if (shell->chamber_light_changed_ != nullptr) {
        shell->chamber_light_changed_(shell->chamber_light_changed_context_, enabled);
      }
    }, LV_EVENT_CLICKED, this);
    create_depth_dots(4);
    view_ = 23;
    visible_profile_ = profile.id;
  }
  const bool supported = snapshot.job.chamber_light_supported;
  const bool enabled = supported && snapshot.job.chamber_light_on;
  const bool pending = supported && snapshot.job.chamber_light_pending;
  const std::uint32_t light_color = enabled ? theme_style_.accent_secondary : theme_style_.track;
  lv_obj_set_style_bg_color(chamber_light_bulb_,
                            lv_color_hex(enabled ? 0xFDE68A : theme_style_.surface_soft), LV_PART_MAIN);
  lv_obj_set_style_border_color(chamber_light_bulb_, lv_color_hex(light_color), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(chamber_light_bulb_, enabled ? 20 : 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(chamber_light_bulb_, lv_color_hex(theme_style_.accent_secondary), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(chamber_light_bulb_, enabled ? LV_OPA_50 : LV_OPA_TRANSP,
                              LV_PART_MAIN);
  lv_obj_set_style_bg_color(chamber_light_button_,
                            lv_color_hex(supported ? accent_color_ : theme_style_.border), LV_PART_MAIN);
  lv_obj_set_style_border_color(chamber_light_button_,
                                lv_color_hex(supported ? accent_color_ : theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_text_color(chamber_light_button_label_,
                              lv_color_hex(supported ? theme_style_.surface : theme_style_.text_muted), LV_PART_MAIN);
  if (enabled) lv_obj_add_state(chamber_light_button_, LV_STATE_CHECKED);
  else lv_obj_remove_state(chamber_light_button_, LV_STATE_CHECKED);
  if (supported && !pending) lv_obj_remove_state(chamber_light_button_, LV_STATE_DISABLED);
  else lv_obj_add_state(chamber_light_button_, LV_STATE_DISABLED);
  if (pending) {
    lv_obj_remove_flag(chamber_light_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail_label_, tr(snapshot.job.chamber_light_target_on
                                            ? "Turning light on…"
                                            : "Turning light off…"));
    lv_label_set_text(chamber_light_button_label_, tr(snapshot.job.chamber_light_target_on
                                                           ? "TURNING ON"
                                                           : "TURNING OFF"));
    lv_obj_align(chamber_light_button_label_, LV_ALIGN_CENTER, 13, 0);
  } else {
    lv_obj_add_flag(chamber_light_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail_label_, tr(supported
                                            ? (enabled ? "Light is on" : "Light is off")
                                            : "Light status unavailable"));
    lv_label_set_text(chamber_light_button_label_, tr(supported
                                                           ? (enabled ? "TURN OFF" : "TURN ON")
                                                           : "UNAVAILABLE"));
    lv_obj_center(chamber_light_button_label_);
  }
  board_display_unlock();
}

void DisplayShell::square_show_system_details(const NetworkStatus& network,
                                               const PowerSnapshot& power,
                                               std::size_t configured_count) {
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 4) {
    prepare_active_screen("system-details");
    square_create_header("SYSTEM", &power);
    lv_obj_t* screen = lv_screen_active();

    lv_obj_t* network_card = lv_obj_create(screen);
    lv_obj_set_size(network_card, kDisplayUsesCompactRoundLayout ? 174 : 208,
                    kDisplayUsesCompactRoundLayout ? 48 : 52);
    if constexpr (kDisplayUsesCompactRoundLayout) {
      lv_obj_align(network_card, LV_ALIGN_TOP_MID, 0, 42);
    } else {
      lv_obj_align(network_card, LV_ALIGN_TOP_LEFT, 8, 34);
    }
    lv_obj_set_style_radius(network_card, themed_radius(12), LV_PART_MAIN);
    lv_obj_set_style_bg_color(network_card, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(network_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(network_card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(network_card, lv_color_hex(theme_style_.accent_secondary), LV_PART_MAIN);
    lv_obj_set_style_border_opa(network_card, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_pad_all(network_card, 0, LV_PART_MAIN);
    apply_surface_effect(network_card);
    lv_obj_remove_flag(network_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(network_card, LV_OBJ_FLAG_CLICKABLE);

    title_label_ = lv_label_create(network_card);
    apply_text_style(title_label_, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_14);
    lv_obj_set_size(title_label_, kDisplayUsesCompactRoundLayout ? 160 : 190, 18);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(title_label_, LV_ALIGN_TOP_LEFT, 8, 6);

    detail_label_ = lv_label_create(network_card);
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
    lv_obj_set_width(detail_label_, kDisplayUsesCompactRoundLayout ? 160 : 190);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label_, LV_ALIGN_BOTTOM_LEFT, 8, -6);

    const char* captions[4] = {"CPU", "INTERNAL", "PSRAM", "SOUND"};
    const std::uint32_t colors[4] = {theme_colors_.paused, theme_style_.accent_secondary,
                                     theme_style_.accent, theme_colors_.preparing};
    lv_obj_t* values[4]{};
    for (int index = 0; index < 4; ++index) {
      const int column = index % 2;
      const int row = index / 2;
      lv_obj_t* card = lv_obj_create(screen);
      lv_obj_set_size(card, kDisplayUsesCompactRoundLayout ? 93 : 99, 44);
      lv_obj_align(card, LV_ALIGN_TOP_LEFT,
                   (kDisplayUsesCompactRoundLayout ? 11 : 8) + column * 105,
                   93 + row * 50);
      lv_obj_set_style_radius(card, themed_radius(12), LV_PART_MAIN);
      lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, lv_color_hex(colors[index]), LV_PART_MAIN);
      lv_obj_set_style_border_opa(card, LV_OPA_60, LV_PART_MAIN);
      lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
      apply_surface_effect(card);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t* caption = lv_label_create(card);
      lv_label_set_text(caption, tr(captions[index]));
      apply_text_style(caption, lv_color_hex(colors[index]), &lv_font_montserrat_12);
      lv_obj_set_width(caption, kDisplayUsesCompactRoundLayout ? 83 : 89);
      lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
      lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 3);

      values[index] = lv_label_create(card);
      apply_text_style(values[index], lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
      lv_obj_set_width(values[index], kDisplayUsesCompactRoundLayout ? 83 : 89);
      lv_obj_set_style_text_align(values[index], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_label_set_long_mode(values[index], LV_LABEL_LONG_DOT);
      lv_obj_align(values[index], LV_ALIGN_BOTTOM_MID, 0, -3);
    }
    temperature_label_ = values[0];
    metrics_label_ = values[1];
    progress_label_ = values[2];
    layer_label_ = values[3];

    lv_obj_t* update_band = lv_obj_create(screen);
    lv_obj_set_size(update_band, kDisplayUsesCompactRoundLayout ? 170 : 220, 44);
    lv_obj_align(update_band, LV_ALIGN_BOTTOM_MID, 0,
                 kDisplayUsesCompactRoundLayout ? -12 : -1);
    lv_obj_set_style_bg_opa(update_band, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(update_band, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(update_band, 0, LV_PART_MAIN);
    square_route_screen_gestures(update_band, true);
    lv_obj_add_flag(update_band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(update_band, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      if (shell->suppress_update_click_) return;
      shell->handle_update_version_click();
    }, LV_EVENT_CLICKED, this);

    version_label_ = lv_label_create(update_band);
    const std::string compact_update_text = square_compact_update_text(update_version_text_);
    lv_label_set_text(version_label_, compact_update_text.c_str());
    apply_text_style(version_label_, lv_color_hex(update_version_color_), &lv_font_montserrat_12);
    lv_obj_set_width(version_label_, kDisplayUsesCompactRoundLayout ? 160 : 210);
    lv_label_set_long_mode(version_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(version_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(version_label_, LV_ALIGN_TOP_MID, 0, 3);
    square_route_screen_gestures(version_label_);

    clock_date_label_ = lv_label_create(update_band);
    apply_text_style(clock_date_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
    lv_obj_set_width(clock_date_label_, kDisplayUsesCompactRoundLayout ? 160 : 210);
    lv_obj_set_style_text_align(clock_date_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(clock_date_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(clock_date_label_, LV_ALIGN_BOTTOM_MID, 0, -3);
    square_route_screen_gestures(clock_date_label_);
    if constexpr (kDisplayUsesCompactRoundLayout) {
      const auto state = static_cast<FirmwareUpdateState>(update_state_);
      if (state == FirmwareUpdateState::failed ||
          state == FirmwareUpdateState::current ||
          state == FirmwareUpdateState::unavailable) {
        lv_obj_add_flag(clock_date_label_, LV_OBJ_FLAG_HIDDEN);
      }
    }
    create_page_dots(1);
    view_ = 4;
  }
  const std::size_t internal_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U;
  const float psram_mb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024.0F * 1024.0F);
  lv_label_set_text_fmt(title_label_, "Wi-Fi: %s",
                        network.station_name.empty() ? tr("OFFLINE") : network.station_name.c_str());
  lv_label_set_text_fmt(detail_label_, "IP: %s", network.ipv4.empty() ? "--" : network.ipv4.c_str());
  lv_label_set_text_fmt(temperature_label_, "%d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
  lv_label_set_text_fmt(metrics_label_, "%u KB", static_cast<unsigned>(internal_kb));
  lv_label_set_text_fmt(progress_label_, "%.1f MB", static_cast<double>(psram_mb));
  lv_label_set_text_fmt(layer_label_, "%d%%", audio_enabled_ ? audio_volume_ : 0);
  const std::uint64_t uptime = static_cast<std::uint64_t>(esp_timer_get_time()) / 1000000ULL;
  lv_label_set_text_fmt(clock_date_label_, "%s %lluh %02llum", tr("UPTIME"),
                        static_cast<unsigned long long>(uptime / 3600ULL),
                        static_cast<unsigned long long>((uptime % 3600ULL) / 60ULL));
  (void)configured_count;
  square_update_power_header(power);
  board_display_unlock();
}

void DisplayShell::square_show_clock(bool analog, const PowerSnapshot& power) {
  if (board_display_lock(1000) != ESP_OK) return;
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  const bool time_known = now > 1'700'000'000;
  const int wanted_view = analog ? 6 : 5;
  if (view_ != wanted_view) {
    prepare_active_screen(analog ? "analog-clock" : "digital-clock");
    square_create_header(analog ? "ANALOG CLOCK" : "DIGITAL CLOCK", &power);
    if (analog) {
      constexpr int kAnalogFaceSize =
          kDisplayUsesCompactRoundLayout ? 152 : 172;
      constexpr int kAnalogCenter =
          kDisplayUsesCompactRoundLayout ? 74 : 84;
      constexpr float kAnalogTickRadius =
          kDisplayUsesCompactRoundLayout ? 61.0F : 70.0F;
      lv_obj_t* face = lv_obj_create(lv_screen_active());
      lv_obj_set_size(face, kAnalogFaceSize, kAnalogFaceSize);
      lv_obj_align(face, LV_ALIGN_CENTER, 0,
                   kDisplayUsesCompactRoundLayout ? -4 : 4);
      lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(face, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
      lv_obj_set_style_border_width(face, 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(face, lv_color_hex(accent_color_), LV_PART_MAIN);
      lv_obj_set_style_pad_all(face, 0, LV_PART_MAIN);
      apply_surface_effect(face);
      square_route_screen_gestures(face);
      constexpr float pi = 3.14159265358979323846F;
      for (int index = 0; index < 12; ++index) {
        const float angle = (index * 30.0F - 90.0F) * pi / 180.0F;
        lv_obj_t* tick = lv_obj_create(face);
        const int size = index % 3 == 0 ? 7 : 4;
        lv_obj_set_size(tick, size, size);
        lv_obj_set_pos(
            tick,
            kAnalogCenter +
                static_cast<int>(std::cos(angle) * kAnalogTickRadius) - size / 2,
            kAnalogCenter +
                static_cast<int>(std::sin(angle) * kAnalogTickRadius) - size / 2);
        lv_obj_set_style_radius(tick, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(tick, lv_color_hex(index % 3 == 0 ? accent_color_ : theme_style_.text_muted),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_width(tick, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tick, 0, LV_PART_MAIN);
        square_route_screen_gestures(tick);
      }
      clock_hour_hand_ = lv_line_create(face);
      clock_minute_hand_ = lv_line_create(face);
      clock_second_hand_ = lv_line_create(face);
      lv_obj_set_style_line_width(clock_hour_hand_, 6, LV_PART_MAIN);
      lv_obj_set_style_line_width(clock_minute_hand_, 4, LV_PART_MAIN);
      lv_obj_set_style_line_width(clock_second_hand_, 2, LV_PART_MAIN);
      lv_obj_set_style_line_color(clock_hour_hand_, lv_color_hex(theme_style_.text_primary), LV_PART_MAIN);
      lv_obj_set_style_line_color(clock_minute_hand_, lv_color_hex(accent_color_), LV_PART_MAIN);
      lv_obj_set_style_line_color(clock_second_hand_, lv_color_hex(theme_colors_.error),
                                  LV_PART_MAIN);
      square_route_screen_gestures(clock_hour_hand_);
      square_route_screen_gestures(clock_minute_hand_);
      square_route_screen_gestures(clock_second_hand_);
    } else {
      clock_status_label_ = lv_label_create(lv_screen_active());
      apply_text_style(clock_status_label_, lv_color_hex(accent_color_), &lv_font_montserrat_32);
      lv_obj_set_width(clock_status_label_, 240);
      lv_obj_set_style_text_align(clock_status_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_set_style_transform_pivot_x(clock_status_label_, 120, LV_PART_MAIN);
      lv_obj_set_style_transform_scale(
          clock_status_label_, kDisplayUsesCompactRoundLayout ? 346 : 384,
          LV_PART_MAIN);
      lv_obj_align(clock_status_label_, LV_ALIGN_CENTER, 0, -14);
      square_route_screen_gestures(clock_status_label_);
    }
    clock_date_label_ = lv_label_create(lv_screen_active());
    apply_text_style(clock_date_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_14);
    lv_obj_set_width(clock_date_label_, kDisplayUsesCompactRoundLayout ? 164 : 220);
    lv_obj_set_style_text_align(clock_date_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(clock_date_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(clock_date_label_, LV_ALIGN_BOTTOM_MID, 0,
                 kDisplayUsesCompactRoundLayout ? -22 : -7);
    square_route_screen_gestures(clock_date_label_);
    create_page_dots(1);
    view_ = wanted_view;
  }
  if (!analog) {
    if (time_known) lv_label_set_text_fmt(clock_status_label_, "%02d:%02d:%02d",
                                           local.tm_hour, local.tm_min, local.tm_sec);
    else lv_label_set_text(clock_status_label_, "--:--:--");
  } else {
    constexpr float pi = 3.14159265358979323846F;
    constexpr int kAnalogCenter =
        kDisplayUsesCompactRoundLayout ? 74 : 84;
    auto set_hand = [pi](auto& points, float degrees, float length) {
      const float radians = (degrees - 90.0F) * pi / 180.0F;
      points[0] = {kAnalogCenter, kAnalogCenter};
      points[1] = {
          static_cast<lv_value_precise_t>(kAnalogCenter +
                                          std::cos(radians) * length),
          static_cast<lv_value_precise_t>(kAnalogCenter +
                                          std::sin(radians) * length)};
    };
    set_hand(hour_points_, time_known
        ? (local.tm_hour % 12 + local.tm_min / 60.0F) * 30.0F : 0.0F,
        kDisplayUsesCompactRoundLayout ? 35.0F : 40.0F);
    set_hand(minute_points_, time_known ? (local.tm_min + local.tm_sec / 60.0F) * 6.0F : 0.0F,
             kDisplayUsesCompactRoundLayout ? 50.0F : 58.0F);
    set_hand(second_points_, time_known ? local.tm_sec * 6.0F : 0.0F,
             kDisplayUsesCompactRoundLayout ? 56.0F : 64.0F);
    lv_line_set_points(clock_hour_hand_, hour_points_.data(), hour_points_.size());
    lv_line_set_points(clock_minute_hand_, minute_points_.data(), minute_points_.size());
    lv_line_set_points(clock_second_hand_, second_points_.data(), second_points_.size());
  }
  if (time_known) {
    static constexpr const char* weekdays[]{"Sunday", "Monday", "Tuesday", "Wednesday",
                                             "Thursday", "Friday", "Saturday"};
    const int day = local.tm_mday;
    const int month = local.tm_mon + 1;
    const int year = local.tm_year + 1900;
    switch (clock_date_format_.load()) {
      case core::CalendarDateFormat::month_day_year:
        lv_label_set_text_fmt(clock_date_label_, "%s  %02d/%02d/%04d",
                              tr(weekdays[local.tm_wday]), month, day, year);
        break;
      case core::CalendarDateFormat::year_month_day:
        lv_label_set_text_fmt(clock_date_label_, "%s  %04d-%02d-%02d",
                              tr(weekdays[local.tm_wday]), year, month, day);
        break;
      case core::CalendarDateFormat::day_month_year:
      default:
        lv_label_set_text_fmt(clock_date_label_, "%s  %02d.%02d.%04d",
                              tr(weekdays[local.tm_wday]), day, month, year);
        break;
    }
  } else {
    lv_label_set_text(clock_date_label_, tr("Waiting for network time"));
  }
  square_update_power_header(power);
  board_display_unlock();
}

void DisplayShell::square_show_web_config(const char* ipv4, const char* local_hostname,
                                          const PowerSnapshot& power) {
  if (ipv4 == nullptr || board_display_lock(1000) != ESP_OK) return;
  const std::string primary_host = local_hostname != nullptr && local_hostname[0] != '\0'
      ? local_hostname : ipv4;
  if (view_ != 7 || visible_web_config_host_ != primary_host) {
    prepare_active_screen("web-config");
    square_create_header("WEB CONFIG", &power);
    detail_label_ = lv_label_create(lv_screen_active());
    apply_text_style(detail_label_, lv_color_hex(theme_style_.accent_secondary), &lv_font_montserrat_12);
    lv_obj_set_width(detail_label_, 220);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_t* qr = lv_qrcode_create(lv_screen_active());
    lv_qrcode_set_size(qr, kDisplayUsesCompactRoundLayout ? 132 : 142);
    lv_qrcode_set_dark_color(qr, lv_color_hex(theme_style_.background));
    lv_qrcode_set_light_color(qr, lv_color_hex(theme_style_.text_primary));
    lv_qrcode_set_quiet_zone(qr, true);
    const std::string address = std::string("http://") + primary_host;
    lv_qrcode_set_data(qr, address.c_str());
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_t* update_band = lv_obj_create(lv_screen_active());
    lv_obj_set_size(update_band, 232, 40);
    lv_obj_align(update_band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(update_band, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(update_band, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(update_band, 0, LV_PART_MAIN);
    square_route_screen_gestures(update_band, true);
    lv_obj_add_flag(update_band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(update_band, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      if (shell->suppress_update_click_) return;
      shell->handle_update_version_click();
    }, LV_EVENT_CLICKED, this);

    version_label_ = lv_label_create(update_band);
    const std::string compact_update_text = square_compact_update_text(update_version_text_);
    lv_label_set_text(version_label_, compact_update_text.c_str());
    apply_text_style(version_label_, lv_color_hex(update_version_color_), &lv_font_montserrat_12);
    lv_obj_set_size(version_label_, 220, 32);
    lv_label_set_long_mode(version_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(version_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(version_label_);
    square_route_screen_gestures(version_label_);
    create_page_dots(1);
    visible_web_config_host_ = primary_host;
    view_ = 7;
  }
  lv_label_set_text(detail_label_, primary_host.c_str());
  square_update_power_header(power);
  board_display_unlock();
}

void DisplayShell::square_ensure_update_overlay() {
  if (update_overlay_ != nullptr && lv_obj_is_valid(update_overlay_)) return;
  update_overlay_ = lv_obj_create(lv_layer_top());
  lv_obj_set_size(update_overlay_, LV_PCT(100), LV_PCT(100));
  lv_obj_center(update_overlay_);
  lv_obj_remove_flag(update_overlay_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(update_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(update_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(update_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(update_overlay_, lv_color_hex(theme_style_.background), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(update_overlay_, LV_GRAD_DIR_NONE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(update_overlay_, LV_OPA_COVER, LV_PART_MAIN);
  update_overlay_title_ = lv_label_create(update_overlay_);
  apply_text_style(update_overlay_title_, lv_color_hex(theme_style_.accent),
                   kDisplayUsesCompactRoundLayout ? &lv_font_montserrat_12
                                                  : &lv_font_montserrat_16);
  lv_obj_set_width(update_overlay_title_, kDisplayUsesCompactRoundLayout ? 134 : 220);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    lv_label_set_long_mode(update_overlay_title_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(update_overlay_title_, 1, LV_PART_MAIN);
  }
  lv_obj_align(update_overlay_title_, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 21 : 18);
  update_overlay_versions_ = lv_label_create(update_overlay_);
  apply_text_style(update_overlay_versions_, lv_color_hex(theme_style_.text_primary),
                   kDisplayUsesCompactRoundLayout ? &lv_font_montserrat_12
                                                  : &lv_font_montserrat_14);
  lv_obj_set_width(update_overlay_versions_, kDisplayUsesCompactRoundLayout ? 184 : 218);
  lv_obj_align(update_overlay_versions_, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 52 : 48);
  update_overlay_detail_ = lv_label_create(update_overlay_);
  apply_text_style(update_overlay_detail_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
  lv_obj_set_width(update_overlay_detail_, kDisplayUsesCompactRoundLayout ? 190 : 212);
  lv_label_set_long_mode(update_overlay_detail_, LV_LABEL_LONG_WRAP);
  lv_obj_align(update_overlay_detail_, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 88 : 91);
  update_overlay_progress_ = lv_label_create(update_overlay_);
  apply_text_style(update_overlay_progress_, lv_color_hex(theme_style_.accent_secondary), &lv_font_montserrat_12);
  lv_obj_set_width(update_overlay_progress_, kDisplayUsesCompactRoundLayout ? 188 : 210);
  lv_obj_align(update_overlay_progress_, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 148 : 145);
  update_overlay_progress_bar_ = lv_bar_create(update_overlay_);
  lv_obj_set_size(update_overlay_progress_bar_,
                  kDisplayUsesCompactRoundLayout ? 176 : 204,
                  kDisplayUsesCompactRoundLayout ? 8 : 9);
  lv_obj_align(update_overlay_progress_bar_, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesCompactRoundLayout ? 174 : 169);
  lv_bar_set_range(update_overlay_progress_bar_, 0, 100);
  lv_obj_set_style_bg_color(update_overlay_progress_bar_, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
  lv_obj_set_style_bg_color(update_overlay_progress_bar_, lv_color_hex(theme_style_.accent_secondary),
                            LV_PART_INDICATOR);
  auto make_button = [&](int x, std::uint32_t color, const char* text, lv_obj_t** label_out) {
    lv_obj_t* button = lv_button_create(update_overlay_);
    // A firmware update is a high-value action. The square layout keeps the
    // large edge-to-edge targets, while the round layout raises and narrows
    // them so their labels and active centers stay inside the circular panel.
    lv_obj_set_size(button, kDisplayUsesCompactRoundLayout ? 88 : 110,
                    kDisplayUsesCompactRoundLayout ? 46 : 58);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, x,
                 kDisplayUsesCompactRoundLayout ? -18 : -8);
    lv_obj_set_style_radius(button, themed_radius(14), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    apply_text_style(label, lv_color_hex(x < 0 ? theme_style_.on_accent : theme_style_.text_primary), &lv_font_montserrat_12);
    if constexpr (kDisplayUsesCompactRoundLayout) {
      lv_obj_set_width(label, 80);
      lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (label_out != nullptr) *label_out = label;
    return button;
  };
  update_install_button_ = make_button(kDisplayUsesCompactRoundLayout ? -45 : -56,
                                       theme_style_.accent, tr("UPDATE NOW"),
                                       &update_install_button_label_);
  update_dismiss_button_ = make_button(kDisplayUsesCompactRoundLayout ? 45 : 56,
                                       theme_style_.surface_soft, tr("NOT NOW"), nullptr);
  lv_obj_add_event_cb(update_install_button_, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell != nullptr && lv_event_get_code(event) == LV_EVENT_CLICKED) {
      lv_event_stop_bubbling(event);
      if (shell->update_install_requested_ != nullptr) {
        shell->update_install_requested_(shell->update_install_context_);
      }
    }
  }, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(update_dismiss_button_, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell != nullptr && lv_event_get_code(event) == LV_EVENT_CLICKED) {
      lv_event_stop_bubbling(event);
      shell->hide_update_overlay();
    }
  }, LV_EVENT_CLICKED, this);
}

}  // namespace printdeck::platform
