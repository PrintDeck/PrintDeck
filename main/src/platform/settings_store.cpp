#include "printdeck/platform/settings_store.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include "nvs.h"
#include "printdeck/core/printer_driver.hpp"

namespace printdeck::platform {
namespace {

constexpr char kNamespace[] = "printdeck";

std::string key_for(std::size_t index, const char* suffix) {
  std::array<char, 16> key{};
  std::snprintf(key.data(), key.size(), "p%u_%s", static_cast<unsigned>(index), suffix);
  return key.data();
}

esp_err_t read_text(nvs_handle_t handle, const char* key, std::string& destination) {
  std::size_t length = 0;
  esp_err_t result = nvs_get_str(handle, key, nullptr, &length);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    destination.clear();
    return ESP_OK;
  }
  if (result != ESP_OK) return result;
  std::string buffer(length, '\0');
  result = nvs_get_str(handle, key, buffer.data(), &length);
  if (result != ESP_OK) return result;
  if (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();
  destination = std::move(buffer);
  return ESP_OK;
}

esp_err_t write_text(nvs_handle_t handle, const char* key, const std::string& value) {
  return value.empty() ? nvs_erase_key(handle, key) : nvs_set_str(handle, key, value.c_str());
}

bool optional_erase_result(esp_err_t result) {
  return result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t read_optional_u32(nvs_handle_t handle, const char* key, std::uint32_t& value) {
  const esp_err_t result = nvs_get_u32(handle, key, &value);
  return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}

esp_err_t read_optional_u8(nvs_handle_t handle, const char* key, std::uint8_t& value) {
  const esp_err_t result = nvs_get_u8(handle, key, &value);
  return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}

}  // namespace

esp_err_t SettingsStore::load(core::DeviceSettings& destination) const {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    destination = {};
    return ESP_OK;
  }
  if (result != ESP_OK) return result;

  core::DeviceSettings loaded;
  std::uint8_t schema = 0;
  std::uint8_t profile_count = 0;
  std::uint8_t audio_enabled = 1;
  std::uint8_t audio_volume = 60;
  std::uint8_t dim_enabled = 1;
  std::uint8_t screen_off_enabled = 1;
  std::uint8_t usb_power_save_enabled = 0;
  std::uint8_t wake_on_orientation_change = 0;
  std::uint8_t printer_animations_enabled = 0;
  std::uint8_t reaction_progress_bar_enabled = 1;
  std::uint8_t reaction_progress_percent_enabled = 1;
  std::uint8_t unified_api_enabled = 0;
  result = nvs_get_u8(handle, "schema", &schema);
  if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
  if (result == ESP_OK && schema > core::kSettingsSchemaVersion) result = ESP_ERR_INVALID_VERSION;
  if (result == ESP_OK) result = read_text(handle, "wifi_name", loaded.wifi_name);
  if (result == ESP_OK) result = read_text(handle, "wifi_pass", loaded.wifi_password);
  if (result == ESP_OK) result = read_text(handle, "theme", loaded.theme);
  if (result == ESP_OK) result = read_text(handle, "timezone", loaded.timezone);
  if (result == ESP_OK && schema >= 2) result = read_text(handle, "language", loaded.language);
  if (result == ESP_OK) result = read_text(handle, "rotation", loaded.rotation);
  if (result == ESP_OK) {
    std::uint32_t last_auto_rotation = loaded.last_auto_rotation;
    result = read_optional_u32(handle, "auto_rot_last", last_auto_rotation);
    if (result == ESP_OK && last_auto_rotation != 0 && last_auto_rotation != 90 &&
        last_auto_rotation != 180 && last_auto_rotation != 270) {
      result = ESP_ERR_INVALID_ARG;
    } else {
      loaded.last_auto_rotation = static_cast<std::uint16_t>(last_auto_rotation);
    }
  }
  if (loaded.theme.empty()) loaded.theme = "green";
  if (loaded.timezone.empty()) loaded.timezone = "UTC";
  if (loaded.language.empty()) loaded.language = "en";
  if (loaded.rotation.empty()) loaded.rotation = "auto";
  if (result == ESP_OK) result = read_optional_u32(handle, "c_print", loaded.custom_theme.printing);
  if (result == ESP_OK) result = read_optional_u32(handle, "c_done", loaded.custom_theme.done);
  if (result == ESP_OK) result = read_optional_u32(handle, "c_error", loaded.custom_theme.error);
  if (result == ESP_OK) result = read_optional_u32(handle, "c_idle", loaded.custom_theme.idle);
  if (result == ESP_OK) result = read_optional_u32(handle, "c_prep", loaded.custom_theme.preparing);
  if (result == ESP_OK) result = read_optional_u32(handle, "c_pause", loaded.custom_theme.paused);
  if (result == ESP_OK) result = read_optional_u32(handle, "c_fil", loaded.custom_theme.filament);
  if (result == ESP_OK) result = read_optional_u32(handle, "c_setup", loaded.custom_theme.setup);
  if (result == ESP_OK) result = read_optional_u32(handle, "c_off", loaded.custom_theme.offline);
  if (result == ESP_OK) result = read_optional_u32(handle, "c_unknown", loaded.custom_theme.unknown);
  if (result == ESP_OK && schema >= 3) {
    result = read_optional_u32(handle, "c_bg", loaded.custom_theme.background);
  }
  if (result == ESP_OK) result = read_optional_u32(handle, "c_preview", loaded.custom_theme.preview_background);
  if (result == ESP_OK) {
    const esp_err_t selected_result = nvs_get_u32(handle, "selected", &loaded.selected_profile);
    if (selected_result != ESP_OK && selected_result != ESP_ERR_NVS_NOT_FOUND) result = selected_result;
  }
  if (result == ESP_OK) {
    const esp_err_t brightness_result = nvs_get_u8(handle, "brightness", &loaded.brightness_percent);
    if (brightness_result == ESP_ERR_NVS_NOT_FOUND) loaded.brightness_percent = 75;
    else if (brightness_result != ESP_OK) result = brightness_result;
  }
  if (result == ESP_OK) {
    const esp_err_t audio_result = nvs_get_u8(handle, "audio", &audio_enabled);
    if (audio_result != ESP_OK && audio_result != ESP_ERR_NVS_NOT_FOUND) result = audio_result;
  }
  loaded.audio_enabled = audio_enabled != 0;
  if (result == ESP_OK) {
    const esp_err_t volume_result = nvs_get_u8(handle, "audio_vol", &audio_volume);
    if (volume_result != ESP_OK && volume_result != ESP_ERR_NVS_NOT_FOUND) result = volume_result;
  }
  loaded.audio_volume_percent = audio_volume;
  if (result == ESP_OK && schema >= 6) {
    result = read_optional_u8(handle, "anim_enable", printer_animations_enabled);
  }
  loaded.printer_animations_enabled = printer_animations_enabled != 0;
  if (result == ESP_OK && schema >= 8) {
    result = read_optional_u8(handle, "react_bar", reaction_progress_bar_enabled);
  }
  if (result == ESP_OK && schema >= 8) {
    result = read_optional_u8(handle, "react_percent", reaction_progress_percent_enabled);
  }
  loaded.reaction_progress_bar_enabled = reaction_progress_bar_enabled != 0;
  loaded.reaction_progress_percent_enabled = reaction_progress_percent_enabled != 0;
  if (result == ESP_OK && schema >= 9) {
    result = read_optional_u8(handle, "api_enabled", unified_api_enabled);
  }
  if (result == ESP_OK && schema >= 9) {
    result = read_text(handle, "api_token", loaded.unified_api_token);
  }
  loaded.unified_api_enabled = unified_api_enabled != 0;
  if (result == ESP_OK && schema >= 4) {
    result = read_text(handle, "audio_set", loaded.audio_preset);
  }
  if (result == ESP_OK && schema >= 5) {
    std::uint32_t muted_events = loaded.audio_muted_events;
    result = read_optional_u32(handle, "audio_mute", muted_events);
    if (result == ESP_OK && (muted_events & ~core::kAudioEventMuteMask) != 0) {
      result = ESP_ERR_INVALID_ARG;
    } else {
      loaded.audio_muted_events = static_cast<std::uint16_t>(muted_events);
    }
  }
  if (result == ESP_OK) {
    result = read_optional_u32(handle, "list_poll_s",
                               loaded.inactive_printer_poll_interval_s);
  }
  if (result == ESP_OK) result = read_text(handle, "cam_mode", loaded.camera_mode);
  if (loaded.camera_mode.empty()) loaded.camera_mode = "snapshots";
  if (result == ESP_OK) {
    result = read_optional_u8(handle, "cam_snap_fps", loaded.camera_snapshot_fps);
  }
  if (result == ESP_OK) result = read_optional_u8(handle, "dim_enable", dim_enabled);
  if (result == ESP_OK) {
    result = read_optional_u8(handle, "dim_level", loaded.display_power.dim_brightness_percent);
  }
  if (result == ESP_OK) result = read_optional_u8(handle, "off_enable", screen_off_enabled);
  if (result == ESP_OK) {
    result = read_optional_u32(handle, "dim_idle", loaded.display_power.dim_timeout_idle_s);
  }
  if (result == ESP_OK) {
    result = read_optional_u32(handle, "dim_active", loaded.display_power.dim_timeout_active_s);
  }
  if (result == ESP_OK) {
    result = read_optional_u32(handle, "off_idle", loaded.display_power.off_timeout_idle_s);
  }
  if (result == ESP_OK) {
    result = read_optional_u32(handle, "off_active", loaded.display_power.off_timeout_active_s);
  }
  if (result == ESP_OK) result = read_optional_u8(handle, "usb_save", usb_power_save_enabled);
  if (result == ESP_OK) {
    result = read_optional_u8(handle, "orient_wake", wake_on_orientation_change);
  }
  loaded.display_power.dim_enabled = dim_enabled != 0;
  loaded.display_power.screen_off_enabled = screen_off_enabled != 0;
  loaded.display_power.usb_power_save_enabled = usb_power_save_enabled != 0;
  loaded.display_power.wake_on_orientation_change = wake_on_orientation_change != 0;
  if (result == ESP_OK) {
    const esp_err_t profiles_result = nvs_get_u8(handle, "profiles", &profile_count);
    if (profiles_result != ESP_OK && profiles_result != ESP_ERR_NVS_NOT_FOUND) result = profiles_result;
  }
  profile_count = std::min<std::uint8_t>(profile_count, core::kMaximumProfiles);

  for (std::size_t index = 0; result == ESP_OK && index < profile_count; ++index) {
    core::PrinterProfile profile;
    std::uint8_t protocol = 0;
    result = nvs_get_u32(handle, key_for(index, "id").c_str(), &profile.id);
    if (result == ESP_OK) result = nvs_get_u8(handle, key_for(index, "proto").c_str(), &protocol);
    if (result == ESP_OK) result = read_text(handle, key_for(index, "name").c_str(), profile.display_name);
    if (result == ESP_OK) result = read_text(handle, key_for(index, "host").c_str(), profile.endpoint);
    if (result == ESP_OK) result = read_text(handle, key_for(index, "api").c_str(), profile.api_key);
    if (result == ESP_OK) result = read_text(handle, key_for(index, "serial").c_str(), profile.serial);
    if (result == ESP_OK) result = read_text(handle, key_for(index, "code").c_str(), profile.access_code);
    if (result == ESP_OK) result = read_text(handle, key_for(index, "maker").c_str(), profile.manufacturer);
    if (result == ESP_OK) result = read_text(handle, key_for(index, "model").c_str(), profile.model);
    if (result == ESP_OK) result = read_text(handle, key_for(index, "brand").c_str(), profile.brand);
    if (result == ESP_OK && !core::printer_protocol_from_storage_id(protocol, profile.protocol)) {
      result = ESP_ERR_INVALID_VERSION;
    }
    if (profile.protocol == core::PrinterProtocol::bambu_lan) {
      const core::PrinterDriverDescriptor& driver = core::printer_driver(profile.protocol);
      if (profile.manufacturer.empty()) profile.manufacturer = driver.default_manufacturer;
      if (profile.brand.empty()) profile.brand = driver.default_brand;
    } else if (profile.manufacturer.empty()) {
      // Profiles saved before identity detection existed remain usable and can
      // be refined from Web Config without losing their credentials.
      profile.manufacturer = "Klipper";
      profile.brand = "klipper";
    }
    if (result == ESP_OK) loaded.profiles.push_back(std::move(profile));
  }
  nvs_close(handle);
  if (result != ESP_OK) return result;
  if (!core::migrate_settings(schema, loaded)) return ESP_ERR_INVALID_VERSION;
  if (!core::validate(loaded).empty()) return ESP_ERR_INVALID_ARG;
  destination = std::move(loaded);
  return ESP_OK;
}

esp_err_t SettingsStore::save(const core::DeviceSettings& settings) const {
  if (!core::validate(settings).empty()) return ESP_ERR_INVALID_ARG;
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) return result;

  auto write = [&result](esp_err_t operation) {
    if (result == ESP_OK && !optional_erase_result(operation)) result = operation;
  };
  write(nvs_set_u8(handle, "schema", core::kSettingsSchemaVersion));
  write(write_text(handle, "wifi_name", settings.wifi_name));
  write(write_text(handle, "wifi_pass", settings.wifi_password));
  write(write_text(handle, "theme", settings.theme));
  write(write_text(handle, "timezone", settings.timezone));
  write(write_text(handle, "language", settings.language));
  write(write_text(handle, "rotation", settings.rotation));
  write(nvs_set_u32(handle, "auto_rot_last", settings.last_auto_rotation));
  write(nvs_set_u32(handle, "c_print", settings.custom_theme.printing));
  write(nvs_set_u32(handle, "c_done", settings.custom_theme.done));
  write(nvs_set_u32(handle, "c_error", settings.custom_theme.error));
  write(nvs_set_u32(handle, "c_idle", settings.custom_theme.idle));
  write(nvs_set_u32(handle, "c_prep", settings.custom_theme.preparing));
  write(nvs_set_u32(handle, "c_pause", settings.custom_theme.paused));
  write(nvs_set_u32(handle, "c_fil", settings.custom_theme.filament));
  write(nvs_set_u32(handle, "c_setup", settings.custom_theme.setup));
  write(nvs_set_u32(handle, "c_off", settings.custom_theme.offline));
  write(nvs_set_u32(handle, "c_unknown", settings.custom_theme.unknown));
  write(nvs_set_u32(handle, "c_bg", settings.custom_theme.background));
  write(nvs_set_u32(handle, "c_preview", settings.custom_theme.preview_background));
  write(nvs_set_u32(handle, "selected", settings.selected_profile));
  write(nvs_set_u8(handle, "brightness", settings.brightness_percent));
  write(nvs_set_u8(handle, "anim_enable", settings.printer_animations_enabled ? 1 : 0));
  write(nvs_set_u8(handle, "react_bar", settings.reaction_progress_bar_enabled ? 1 : 0));
  write(nvs_set_u8(handle, "react_percent",
                   settings.reaction_progress_percent_enabled ? 1 : 0));
  write(nvs_set_u8(handle, "api_enabled", settings.unified_api_enabled ? 1 : 0));
  write(write_text(handle, "api_token", settings.unified_api_token));
  write(nvs_erase_key(handle, "protected"));
  write(nvs_set_u8(handle, "audio", settings.audio_enabled ? 1 : 0));
  write(nvs_set_u8(handle, "audio_vol", settings.audio_volume_percent));
  write(write_text(handle, "audio_set", settings.audio_preset));
  write(nvs_set_u32(handle, "audio_mute", settings.audio_muted_events));
  write(nvs_set_u32(handle, "list_poll_s",
                    settings.inactive_printer_poll_interval_s));
  write(write_text(handle, "cam_mode", settings.camera_mode));
  write(nvs_set_u8(handle, "cam_snap_fps", settings.camera_snapshot_fps));
  write(nvs_set_u8(handle, "dim_enable", settings.display_power.dim_enabled ? 1 : 0));
  write(nvs_set_u8(handle, "dim_level", settings.display_power.dim_brightness_percent));
  write(nvs_set_u8(handle, "off_enable", settings.display_power.screen_off_enabled ? 1 : 0));
  write(nvs_set_u32(handle, "dim_idle", settings.display_power.dim_timeout_idle_s));
  write(nvs_set_u32(handle, "dim_active", settings.display_power.dim_timeout_active_s));
  write(nvs_set_u32(handle, "off_idle", settings.display_power.off_timeout_idle_s));
  write(nvs_set_u32(handle, "off_active", settings.display_power.off_timeout_active_s));
  write(nvs_set_u8(handle, "usb_save", settings.display_power.usb_power_save_enabled ? 1 : 0));
  write(nvs_set_u8(handle, "orient_wake",
                   settings.display_power.wake_on_orientation_change ? 1 : 0));
  write(nvs_set_u8(handle, "profiles", static_cast<std::uint8_t>(settings.profiles.size())));

  for (std::size_t index = 0; index < settings.profiles.size(); ++index) {
    const core::PrinterProfile& profile = settings.profiles[index];
    write(nvs_set_u32(handle, key_for(index, "id").c_str(), profile.id));
    write(nvs_set_u8(handle, key_for(index, "proto").c_str(),
                     core::printer_driver(profile.protocol).storage_id));
    write(write_text(handle, key_for(index, "name").c_str(), profile.display_name));
    write(write_text(handle, key_for(index, "host").c_str(), profile.endpoint));
    write(write_text(handle, key_for(index, "api").c_str(), profile.api_key));
    write(write_text(handle, key_for(index, "serial").c_str(), profile.serial));
    write(write_text(handle, key_for(index, "code").c_str(), profile.access_code));
    write(write_text(handle, key_for(index, "maker").c_str(), profile.manufacturer));
    write(write_text(handle, key_for(index, "model").c_str(), profile.model));
    write(write_text(handle, key_for(index, "brand").c_str(), profile.brand));
  }
  for (std::size_t index = settings.profiles.size(); index < core::kMaximumProfiles; ++index) {
    for (const char* suffix : {"id", "proto", "name", "host", "api", "serial", "code",
                               "maker", "model", "brand"}) {
      write(nvs_erase_key(handle, key_for(index, suffix).c_str()));
    }
  }
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result;
}

esp_err_t SettingsStore::save_selected_profile(std::uint32_t profile_id) const {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) return result;
  result = nvs_set_u32(handle, "selected", profile_id);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result;
}

}  // namespace printdeck::platform
