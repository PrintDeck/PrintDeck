#include "printdeck/core/configuration_backup.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include "cJSON.h"
#include "printdeck/core/printer_driver.hpp"

namespace printdeck::core {
namespace {

constexpr std::string_view kDocumentFormat = "printdeck-configuration";
constexpr std::size_t kMaximumReactionBackupEvents = 64;
constexpr std::size_t kMaximumReactionBackupBytes = 5U * 1024U * 1024U;

struct JsonDeleter {
  void operator()(cJSON* value) const { cJSON_Delete(value); }
};

using JsonDocument = std::unique_ptr<cJSON, JsonDeleter>;

const cJSON* item(const cJSON* object, const char* key) {
  return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

bool add_string(cJSON* object, const char* key, std::string_view value) {
  return cJSON_AddStringToObject(object, key, std::string(value).c_str()) != nullptr;
}

bool add_number(cJSON* object, const char* key, std::uint32_t value) {
  return cJSON_AddNumberToObject(object, key, static_cast<double>(value)) != nullptr;
}

bool add_bool(cJSON* object, const char* key, bool value) {
  return cJSON_AddBoolToObject(object, key, value) != nullptr;
}

bool read_string(const cJSON* object, const char* key, std::string& value,
                 std::size_t maximum, bool required) {
  const cJSON* candidate = item(object, key);
  if (!candidate) return !required;
  if (!cJSON_IsString(candidate) || !candidate->valuestring) return false;
  const std::string_view text(candidate->valuestring);
  if (text.size() > maximum) return false;
  value.assign(text);
  return true;
}

bool read_unsigned(const cJSON* object, const char* key, std::uint32_t& value,
                   std::uint32_t maximum, bool required) {
  const cJSON* candidate = item(object, key);
  if (!candidate) return !required;
  if (!cJSON_IsNumber(candidate) || !std::isfinite(candidate->valuedouble) ||
      candidate->valuedouble < 0.0 ||
      candidate->valuedouble > static_cast<double>(maximum) ||
      std::floor(candidate->valuedouble) != candidate->valuedouble) {
    return false;
  }
  value = static_cast<std::uint32_t>(candidate->valuedouble);
  return true;
}

template <typename T>
bool read_unsigned(const cJSON* object, const char* key, T& value,
                   std::uint32_t maximum, bool required) {
  std::uint32_t parsed = value;
  if (!read_unsigned(object, key, parsed, maximum, required)) return false;
  value = static_cast<T>(parsed);
  return true;
}

bool read_bool(const cJSON* object, const char* key, bool& value, bool required) {
  const cJSON* candidate = item(object, key);
  if (!candidate) return !required;
  if (!cJSON_IsBool(candidate)) return false;
  value = cJSON_IsTrue(candidate);
  return true;
}

bool add_theme(cJSON* settings_object, const ThemeColors& colors) {
  cJSON* theme = cJSON_AddObjectToObject(settings_object, "custom_theme");
  return theme && add_number(theme, "printing", colors.printing) &&
         add_number(theme, "done", colors.done) &&
         add_number(theme, "error", colors.error) &&
         add_number(theme, "idle", colors.idle) &&
         add_number(theme, "preparing", colors.preparing) &&
         add_number(theme, "paused", colors.paused) &&
         add_number(theme, "filament", colors.filament) &&
         add_number(theme, "setup", colors.setup) &&
         add_number(theme, "offline", colors.offline) &&
         add_number(theme, "unknown", colors.unknown) &&
         add_number(theme, "background", colors.background) &&
         add_number(theme, "preview_background", colors.preview_background);
}

bool read_theme(const cJSON* settings_object, ThemeColors& colors, bool required) {
  const cJSON* theme = item(settings_object, "custom_theme");
  if (!theme) return !required;
  if (!cJSON_IsObject(theme)) return false;
  constexpr std::uint32_t kMaximumColor = 0xFFFFFFU;
  return read_unsigned(theme, "printing", colors.printing, kMaximumColor, required) &&
         read_unsigned(theme, "done", colors.done, kMaximumColor, required) &&
         read_unsigned(theme, "error", colors.error, kMaximumColor, required) &&
         read_unsigned(theme, "idle", colors.idle, kMaximumColor, required) &&
         read_unsigned(theme, "preparing", colors.preparing, kMaximumColor, required) &&
         read_unsigned(theme, "paused", colors.paused, kMaximumColor, required) &&
         read_unsigned(theme, "filament", colors.filament, kMaximumColor, required) &&
         read_unsigned(theme, "setup", colors.setup, kMaximumColor, required) &&
         read_unsigned(theme, "offline", colors.offline, kMaximumColor, required) &&
         read_unsigned(theme, "unknown", colors.unknown, kMaximumColor, required) &&
         read_unsigned(theme, "background", colors.background, kMaximumColor, required) &&
         read_unsigned(theme, "preview_background", colors.preview_background,
                       kMaximumColor, required);
}

bool add_display_power(cJSON* settings_object, const DisplayPowerPolicy& power) {
  cJSON* object = cJSON_AddObjectToObject(settings_object, "display_power");
  return object && add_bool(object, "dim_enabled", power.dim_enabled) &&
         add_number(object, "dim_brightness_percent", power.dim_brightness_percent) &&
         add_bool(object, "screen_off_enabled", power.screen_off_enabled) &&
         add_number(object, "dim_timeout_idle_s", power.dim_timeout_idle_s) &&
         add_number(object, "dim_timeout_active_s", power.dim_timeout_active_s) &&
         add_number(object, "off_timeout_idle_s", power.off_timeout_idle_s) &&
         add_number(object, "off_timeout_active_s", power.off_timeout_active_s) &&
         add_bool(object, "usb_power_save_enabled", power.usb_power_save_enabled) &&
         add_bool(object, "wake_on_orientation_change", power.wake_on_orientation_change);
}

bool read_display_power(const cJSON* settings_object, DisplayPowerPolicy& power,
                        bool required) {
  const cJSON* object = item(settings_object, "display_power");
  if (!object) return !required;
  if (!cJSON_IsObject(object)) return false;
  return read_bool(object, "dim_enabled", power.dim_enabled, required) &&
         read_unsigned(object, "dim_brightness_percent", power.dim_brightness_percent,
                       100, required) &&
         read_bool(object, "screen_off_enabled", power.screen_off_enabled, required) &&
         read_unsigned(object, "dim_timeout_idle_s", power.dim_timeout_idle_s, 3600,
                       required) &&
         read_unsigned(object, "dim_timeout_active_s", power.dim_timeout_active_s, 3600,
                       required) &&
         read_unsigned(object, "off_timeout_idle_s", power.off_timeout_idle_s, 3600,
                       required) &&
         read_unsigned(object, "off_timeout_active_s", power.off_timeout_active_s, 3600,
                       required) &&
         read_bool(object, "usb_power_save_enabled", power.usb_power_save_enabled,
                   required) &&
         read_bool(object, "wake_on_orientation_change", power.wake_on_orientation_change,
                   required);
}

bool add_profiles(cJSON* settings_object, const std::vector<PrinterProfile>& profiles) {
  cJSON* array = cJSON_AddArrayToObject(settings_object, "profiles");
  if (!array) return false;
  for (const PrinterProfile& profile : profiles) {
    cJSON* object = cJSON_CreateObject();
    if (!object || !cJSON_AddItemToArray(array, object)) {
      cJSON_Delete(object);
      return false;
    }
    if (!add_number(object, "id", profile.id) ||
        !add_string(object, "protocol", printer_driver(profile.protocol).id) ||
        !add_string(object, "display_name", profile.display_name) ||
        !add_string(object, "endpoint", profile.endpoint) ||
        !add_string(object, "api_key", profile.api_key) ||
        !add_string(object, "serial", profile.serial) ||
        !add_string(object, "access_code", profile.access_code) ||
        !add_string(object, "manufacturer", profile.manufacturer) ||
        !add_string(object, "model", profile.model) ||
        !add_string(object, "brand", profile.brand)) {
      return false;
    }
  }
  return true;
}

bool read_profiles(const cJSON* settings_object, std::vector<PrinterProfile>& profiles) {
  const cJSON* array = item(settings_object, "profiles");
  if (!cJSON_IsArray(array)) return false;
  const int count = cJSON_GetArraySize(array);
  if (count < 0 || static_cast<std::size_t>(count) > kMaximumProfiles) return false;
  profiles.clear();
  profiles.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const cJSON* object = cJSON_GetArrayItem(array, index);
    if (!cJSON_IsObject(object)) return false;
    PrinterProfile profile;
    std::string protocol;
    if (!read_unsigned(object, "id", profile.id,
                       std::numeric_limits<std::uint32_t>::max(), true) ||
        !read_string(object, "protocol", protocol, 24, true) ||
        !printer_protocol_from_id(protocol, profile.protocol) ||
        !read_string(object, "display_name", profile.display_name, 48, true) ||
        !read_string(object, "endpoint", profile.endpoint, 128, true) ||
        !read_string(object, "api_key", profile.api_key, 128, true) ||
        !read_string(object, "serial", profile.serial, 32, true) ||
        !read_string(object, "access_code", profile.access_code, 32, true) ||
        !read_string(object, "manufacturer", profile.manufacturer, 48, true) ||
        !read_string(object, "model", profile.model, 48, true) ||
        !read_string(object, "brand", profile.brand, 24, true)) {
      return false;
    }
    profiles.push_back(std::move(profile));
  }
  return true;
}

bool add_reactions(cJSON* root, const ConfigurationBackupReactions& reactions) {
  if (reactions.active_set.empty() || reactions.active_set.size() > 48 ||
      reactions.events.empty() ||
      reactions.events.size() > kMaximumReactionBackupEvents) {
    return false;
  }
  cJSON* object = cJSON_AddObjectToObject(root, "reactions");
  cJSON* events = object ? cJSON_AddArrayToObject(object, "events") : nullptr;
  if (!object || !events || !add_string(object, "active_set", reactions.active_set)) {
    return false;
  }
  std::vector<std::string_view> seen;
  seen.reserve(reactions.events.size());
  for (const ConfigurationBackupReactionEvent& event : reactions.events) {
    if (event.id.empty() || event.id.size() > 48 ||
        event.custom_bytes > kMaximumReactionBackupBytes ||
        std::find(seen.begin(), seen.end(), event.id) != seen.end()) {
      return false;
    }
    seen.push_back(event.id);
    cJSON* entry = cJSON_CreateObject();
    if (!entry || !cJSON_AddItemToArray(events, entry)) {
      cJSON_Delete(entry);
      return false;
    }
    if (!add_string(entry, "id", event.id) ||
        !add_bool(entry, "enabled", event.enabled) ||
        !add_number(entry, "custom_bytes",
                    static_cast<std::uint32_t>(event.custom_bytes))) {
      return false;
    }
  }
  return true;
}

bool read_reactions(const cJSON* root, ConfigurationBackupReactions& reactions) {
  const cJSON* object = item(root, "reactions");
  const cJSON* events = item(object, "events");
  if (!cJSON_IsObject(object) || !cJSON_IsArray(events) ||
      !read_string(object, "active_set", reactions.active_set, 48, true) ||
      reactions.active_set.empty()) {
    return false;
  }
  const int count = cJSON_GetArraySize(events);
  if (count <= 0 || static_cast<std::size_t>(count) > kMaximumReactionBackupEvents) {
    return false;
  }
  reactions.events.clear();
  reactions.events.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const cJSON* entry = cJSON_GetArrayItem(events, index);
    ConfigurationBackupReactionEvent event;
    if (!cJSON_IsObject(entry) ||
        !read_string(entry, "id", event.id, 48, true) || event.id.empty() ||
        !read_bool(entry, "enabled", event.enabled, true) ||
        !read_unsigned(entry, "custom_bytes", event.custom_bytes,
                       kMaximumReactionBackupBytes, true) ||
        std::any_of(reactions.events.begin(), reactions.events.end(),
                    [&](const ConfigurationBackupReactionEvent& existing) {
                      return existing.id == event.id;
                    })) {
      return false;
    }
    reactions.events.push_back(std::move(event));
  }
  return true;
}

bool add_settings(cJSON* root, const DeviceSettings& settings) {
  cJSON* object = cJSON_AddObjectToObject(root, "settings");
  return object && add_string(object, "wifi_name", settings.wifi_name) &&
         add_string(object, "wifi_password", settings.wifi_password) &&
         add_profiles(object, settings.profiles) &&
         add_number(object, "selected_profile", settings.selected_profile) &&
         add_number(object, "brightness_percent", settings.brightness_percent) &&
         add_bool(object, "printer_animations_enabled", settings.printer_animations_enabled) &&
         add_bool(object, "reaction_progress_bar_enabled",
                  settings.reaction_progress_bar_enabled) &&
         add_bool(object, "reaction_progress_percent_enabled",
                  settings.reaction_progress_percent_enabled) &&
         add_string(object, "theme", settings.theme) && add_theme(object, settings.custom_theme) &&
         add_string(object, "timezone", settings.timezone) &&
         add_string(object, "language", settings.language) &&
         add_string(object, "rotation", settings.rotation) &&
         add_number(object, "last_auto_rotation", settings.last_auto_rotation) &&
         add_bool(object, "audio_enabled", settings.audio_enabled) &&
         add_number(object, "audio_volume_percent", settings.audio_volume_percent) &&
         add_string(object, "audio_preset", settings.audio_preset) &&
         add_number(object, "audio_muted_events", settings.audio_muted_events) &&
         add_number(object, "inactive_printer_poll_interval_s",
                    settings.inactive_printer_poll_interval_s) &&
         add_string(object, "camera_mode", settings.camera_mode) &&
         add_number(object, "camera_snapshot_fps", settings.camera_snapshot_fps) &&
         add_bool(object, "unified_api_enabled", settings.unified_api_enabled) &&
         add_string(object, "unified_api_token", settings.unified_api_token) &&
         add_display_power(object, settings.display_power);
}

bool read_settings(const cJSON* root, std::uint8_t source_schema, DeviceSettings& settings) {
  const cJSON* object = item(root, "settings");
  if (!cJSON_IsObject(object)) return false;
  const bool required = source_schema >= kSettingsSchemaVersion;
  return read_string(object, "wifi_name", settings.wifi_name, 32, required) &&
         read_string(object, "wifi_password", settings.wifi_password, 64, required) &&
         read_profiles(object, settings.profiles) &&
         read_unsigned(object, "selected_profile", settings.selected_profile,
                       std::numeric_limits<std::uint32_t>::max(), required) &&
         read_unsigned(object, "brightness_percent", settings.brightness_percent, 100,
                       required) &&
         read_bool(object, "printer_animations_enabled",
                   settings.printer_animations_enabled, required) &&
         read_bool(object, "reaction_progress_bar_enabled",
                   settings.reaction_progress_bar_enabled, required) &&
         read_bool(object, "reaction_progress_percent_enabled",
                   settings.reaction_progress_percent_enabled, required) &&
         read_string(object, "theme", settings.theme, 20, required) &&
         read_theme(object, settings.custom_theme, required) &&
         read_string(object, "timezone", settings.timezone, 64, required) &&
         read_string(object, "language", settings.language, 8, required) &&
         read_string(object, "rotation", settings.rotation, 4, required) &&
         read_unsigned(object, "last_auto_rotation", settings.last_auto_rotation, 270,
                       required) &&
         read_bool(object, "audio_enabled", settings.audio_enabled, required) &&
         read_unsigned(object, "audio_volume_percent", settings.audio_volume_percent, 100,
                       required) &&
         read_string(object, "audio_preset", settings.audio_preset, 20, required) &&
         read_unsigned(object, "audio_muted_events", settings.audio_muted_events,
                       kAudioEventMuteMask, required) &&
         read_unsigned(object, "inactive_printer_poll_interval_s",
                       settings.inactive_printer_poll_interval_s, 300, required) &&
         read_string(object, "camera_mode", settings.camera_mode, 16, required) &&
         read_unsigned(object, "camera_snapshot_fps", settings.camera_snapshot_fps, 5,
                       required) &&
         read_bool(object, "unified_api_enabled", settings.unified_api_enabled,
                   source_schema >= 9) &&
         read_string(object, "unified_api_token", settings.unified_api_token,
                     kUnifiedApiTokenLength, source_schema >= 9) &&
         read_display_power(object, settings.display_power, required);
}

}  // namespace

std::string serialize_configuration_backup(const DeviceSettings& settings,
                                           std::string_view hardware_id,
                                           const ConfigurationBackupReactions& reactions) {
  if (hardware_id.empty() || hardware_id.size() > 32 || !validate(settings).empty()) return {};
  JsonDocument root(cJSON_CreateObject());
  if (!root || !add_string(root.get(), "format", kDocumentFormat) ||
      !add_number(root.get(), "format_version", kConfigurationBackupFormatVersion) ||
      !add_number(root.get(), "settings_schema", kSettingsSchemaVersion) ||
      !add_string(root.get(), "hardware_id", hardware_id) ||
      !add_settings(root.get(), settings) || !add_reactions(root.get(), reactions)) {
    return {};
  }
  char* printed = cJSON_PrintUnformatted(root.get());
  if (!printed) return {};
  std::string document(printed);
  cJSON_free(printed);
  if (document.size() > kMaximumConfigurationBackupBytes) return {};
  return document;
}

ConfigurationBackupResult parse_configuration_backup(
    std::string_view document, std::string_view expected_hardware_id) {
  ConfigurationBackupResult result;
  if (document.empty() || document.size() > kMaximumConfigurationBackupBytes ||
      expected_hardware_id.empty() || expected_hardware_id.size() > 32) {
    return result;
  }

  const char* parse_end = nullptr;
  JsonDocument root(cJSON_ParseWithLengthOpts(document.data(), document.size(), &parse_end, 0));
  if (!root || !cJSON_IsObject(root.get()) || !parse_end) return result;
  while (parse_end < document.data() + document.size() &&
         (*parse_end == ' ' || *parse_end == '\t' || *parse_end == '\r' ||
          *parse_end == '\n')) {
    ++parse_end;
  }
  if (parse_end != document.data() + document.size()) return result;

  std::string format;
  if (!read_string(root.get(), "format", format, 32, true)) return result;
  if (format != kDocumentFormat) {
    result.error = ConfigurationBackupError::unsupported_format;
    return result;
  }
  if (!read_unsigned(root.get(), "format_version", result.summary.format_version,
                     std::numeric_limits<std::uint8_t>::max(), true)) {
    return result;
  }
  if (result.summary.format_version != kConfigurationBackupFormatVersion) {
    result.error = ConfigurationBackupError::unsupported_version;
    return result;
  }
  if (!read_unsigned(root.get(), "settings_schema", result.summary.settings_schema,
                     std::numeric_limits<std::uint8_t>::max(), true) ||
      !read_string(root.get(), "hardware_id", result.summary.hardware_id, 32, true)) {
    return result;
  }
  if (result.summary.settings_schema == 0 ||
      result.summary.settings_schema > kSettingsSchemaVersion) {
    result.error = ConfigurationBackupError::unsupported_version;
    return result;
  }
  if (result.summary.hardware_id != expected_hardware_id) {
    result.error = ConfigurationBackupError::incompatible_hardware;
    return result;
  }
  if (!read_settings(root.get(), result.summary.settings_schema, result.settings) ||
      !read_reactions(root.get(), result.reactions)) {
    return result;
  }
  if (!migrate_settings(result.summary.settings_schema, result.settings) ||
      !validate(result.settings).empty()) {
    result.error = ConfigurationBackupError::invalid_settings;
    return result;
  }
  result.summary.profile_count = result.settings.profiles.size();
  result.error = ConfigurationBackupError::none;
  return result;
}

}  // namespace printdeck::core
