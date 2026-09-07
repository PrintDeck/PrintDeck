#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "printdeck/core/device_state.hpp"
#include "printdeck/core/theme.hpp"

namespace printdeck::core {

constexpr std::size_t kMaximumProfiles = 10;
constexpr std::uint8_t kSettingsSchemaVersion = 13;
constexpr std::uint8_t kScreenSaverCircles = 0;
constexpr std::uint8_t kScreenSaverGoingToSleep = 1;
constexpr std::size_t kUnifiedApiTokenLength = 67;
constexpr std::size_t kMaximumDeviceNameCharacters = 16;
constexpr std::size_t kMaximumDeviceNameBytes = kMaximumDeviceNameCharacters * 4;
constexpr std::uint16_t kAudioEventMuteMask = (1U << 14U) - 1U;

struct DisplayPowerPolicy {
  bool dim_enabled = true;
  std::uint8_t dim_brightness_percent = 0;
  std::uint8_t screen_saver_animation = kScreenSaverCircles;
  bool screen_off_enabled = true;
  std::uint32_t dim_timeout_idle_s = 20;
  std::uint32_t dim_timeout_active_s = 0;
  std::uint32_t screen_saver_timeout_idle_s = 60;
  std::uint32_t screen_saver_timeout_active_s = 0;
  // A zero off timeout keeps the display on in that print state; dimming is independent.
  std::uint32_t off_timeout_idle_s = 120;
  std::uint32_t off_timeout_active_s = 0;
  bool usb_power_save_enabled = false;
  bool usb_power_save_active_enabled = false;
  bool wake_on_orientation_change = true;
  bool wake_on_touch = true;
  std::uint8_t dim_audio_percent = 80;
  std::uint8_t off_audio_percent = 20;
  std::uint32_t shutdown_timeout_s = 0;

  // Every enabled stage is timed from the same last interaction. Zero skips it.
  // 0 = normal brightness, 1 = dimmed, 2 = fully off, 3 = screen saver.
  int mode_after_inactivity(std::uint64_t idle_ms, bool print_active) const {
    const std::uint64_t dim_at = 1000ULL *
        (print_active ? dim_timeout_active_s : dim_timeout_idle_s);
    const std::uint64_t saver_at = 1000ULL *
        (print_active ? screen_saver_timeout_active_s : screen_saver_timeout_idle_s);
    const std::uint64_t off_at = 1000ULL *
        (print_active ? off_timeout_active_s : off_timeout_idle_s);
    const bool off_enabled = screen_off_enabled && off_at > 0;
    if (off_enabled && idle_ms >= off_at) return 2;
    if (saver_at > 0 && idle_ms >= saver_at) return 3;
    if (dim_enabled && dim_at > 0 && idle_ms >= dim_at) return 1;
    return 0;
  }
  bool apply_on_usb(bool print_active) const {
    return print_active ? usb_power_save_active_enabled : usb_power_save_enabled;
  }
  bool timers_allowed(bool on_battery, bool detects_power_source,
                      bool print_active) const {
    // Boards without source detection apply their timers on any supply.
    return !detects_power_source || on_battery || apply_on_usb(print_active);
  }
};

struct DeviceSettings {
  std::string device_name;
  std::string wifi_name;
  std::string wifi_password;
  std::vector<PrinterProfile> profiles;
  std::uint32_t selected_profile = 0;
  std::uint8_t brightness_percent = 75;
  bool printer_animations_enabled = false;
  bool reaction_progress_bar_enabled = true;
  bool reaction_progress_percent_enabled = true;
  std::string theme = "green";
  ThemeColors custom_theme;
  std::string timezone = "UTC";
  std::string language = "en";
  std::string rotation = "auto";
  std::uint16_t last_auto_rotation = 0;
  bool audio_enabled = true;
  std::uint8_t audio_volume_percent = 60;
  std::string audio_preset = "modern";
  std::uint16_t audio_muted_events = 0;
  std::uint32_t inactive_printer_poll_interval_s = 60;
  std::string camera_mode = "snapshots";
  std::uint8_t camera_snapshot_fps = 1;
  bool unified_api_enabled = false;
  std::string unified_api_token;
  DisplayPowerPolicy display_power;
};

struct ValidationIssue {
  std::string field;
  std::string message;
};

std::vector<ValidationIssue> validate(const DeviceSettings& settings);
DeviceSettings redact_secrets(DeviceSettings settings);
void clear_irrelevant_printer_credentials(PrinterProfile& profile);
bool same_http_auth_context(const PrinterProfile& first, const PrinterProfile& second);
bool is_local_printer_endpoint(std::string_view endpoint, PrinterProtocol protocol);
bool supported_audio_preset(std::string_view id);
bool valid_unified_api_token(std::string_view token);
bool valid_device_name(std::string_view name);
std::string device_name_slug(std::string_view name);
bool migrate_settings(std::uint8_t source_schema, DeviceSettings& settings);

}  // namespace printdeck::core
