#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "printdeck/core/device_state.hpp"
#include "printdeck/core/theme.hpp"

namespace printdeck::core {

constexpr std::size_t kMaximumProfiles = 10;
constexpr std::uint8_t kSettingsSchemaVersion = 15;
constexpr std::uint32_t kDisplayDurationUntilWake = 86401;
constexpr std::uint8_t kScreenSaverCircles = 0;
constexpr std::uint8_t kScreenSaverGoingToSleep = 1;
constexpr std::size_t kUnifiedApiTokenLength = 67;
constexpr std::size_t kMaximumDeviceNameCharacters = 16;
constexpr std::size_t kMaximumDeviceNameBytes = kMaximumDeviceNameCharacters * 4;
constexpr std::uint16_t kAudioEventMuteMask = (1U << 14U) - 1U;

struct DisplayPowerPolicy {
  // Schema 14: delay before the sequence, then durations. Zero skips a stage.
  std::uint32_t start_timeout_idle_s = 20;
  std::uint32_t start_timeout_active_s = 0;
  std::uint32_t dim_duration_idle_s = 60;
  std::uint32_t dim_duration_active_s = 60;
  std::uint32_t saver_duration_idle_s = 120;
  std::uint32_t saver_duration_active_s = 120;
  // Legacy absolute thresholds retained only to read and migrate older settings.
  bool dim_enabled = true;
  std::uint8_t dim_brightness_percent = 0;
  std::uint8_t screen_saver_animation = kScreenSaverGoingToSleep;
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

  // 0 = normal brightness, 1 = dimmed, 2 = fully off, 3 = screen saver.
  int mode_after_inactivity(std::uint64_t idle_ms, bool print_active) const {
    const auto start = print_active ? start_timeout_active_s : start_timeout_idle_s;
    if (start == 0 || idle_ms < 1000ULL * start) return 0;
    auto elapsed = idle_ms - 1000ULL * start;
    const auto dim = print_active ? dim_duration_active_s : dim_duration_idle_s;
    const auto saver = print_active ? saver_duration_active_s : saver_duration_idle_s;
    if (dim == kDisplayDurationUntilWake || elapsed < 1000ULL * dim) return 1;
    elapsed -= 1000ULL * dim;
    if (saver == kDisplayDurationUntilWake || elapsed < 1000ULL * saver) return 3;
    return 2;
  }
  bool apply_on_usb(bool /*print_active*/) const { return usb_power_save_enabled; }
  bool shutdown_after_display_off(std::uint64_t off_elapsed_ms) const {
    return shutdown_timeout_s > 0 && off_elapsed_ms >= 1000ULL * shutdown_timeout_s;
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
  bool voice_enabled = false;
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
