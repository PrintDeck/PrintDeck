#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "printdeck/core/device_state.hpp"
#include "printdeck/core/theme.hpp"

namespace printdeck::core {

constexpr std::size_t kMaximumProfiles = 10;
constexpr std::uint8_t kSettingsSchemaVersion = 10;
constexpr std::size_t kUnifiedApiTokenLength = 67;
constexpr std::size_t kMaximumDeviceNameCharacters = 16;
constexpr std::size_t kMaximumDeviceNameBytes = kMaximumDeviceNameCharacters * 4;
constexpr std::uint16_t kAudioEventMuteMask = (1U << 14U) - 1U;

struct DisplayPowerPolicy {
  bool dim_enabled = true;
  std::uint8_t dim_brightness_percent = 0;
  bool screen_off_enabled = true;
  std::uint32_t dim_timeout_idle_s = 20;
  std::uint32_t dim_timeout_active_s = 30;
  std::uint32_t off_timeout_idle_s = 60;
  std::uint32_t off_timeout_active_s = 120;
  bool usb_power_save_enabled = false;
  bool wake_on_orientation_change = true;
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
bool is_local_printer_endpoint(std::string_view endpoint, PrinterProtocol protocol);
bool supported_audio_preset(std::string_view id);
bool valid_unified_api_token(std::string_view token);
bool valid_device_name(std::string_view name);
std::string device_name_slug(std::string_view name);
bool migrate_settings(std::uint8_t source_schema, DeviceSettings& settings);

}  // namespace printdeck::core
