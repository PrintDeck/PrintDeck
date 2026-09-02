#include "printdeck/core/settings.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>
#include <unordered_set>

#include "printdeck/core/localization.hpp"
#include "printdeck/core/printer_driver.hpp"

namespace printdeck::core {
namespace {

void check_text(std::vector<ValidationIssue>& issues, const char* field,
                const std::string& value, std::size_t maximum, bool required) {
  if (required && value.empty()) {
    issues.push_back({field, "Value is required"});
  } else if (value.size() > maximum) {
    issues.push_back({field, "Value is too long"});
  }
}

bool parse_ipv4(std::string_view host, unsigned (&parts)[4]) {
  std::size_t cursor = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    const std::size_t end = host.find('.', cursor);
    const bool last = index == 3;
    if ((last && end != std::string_view::npos) || (!last && end == std::string_view::npos)) {
      return false;
    }
    const std::string_view part = host.substr(
        cursor, (last ? host.size() : end) - cursor);
    if (part.empty() || part.size() > 3) return false;
    unsigned value = 0;
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || value > 255) {
      return false;
    }
    parts[index] = value;
    cursor = end + 1;
  }
  return true;
}

bool private_ipv4(std::string_view host) {
  unsigned parts[4]{};
  if (!parse_ipv4(host, parts)) return false;
  return parts[0] == 10 || (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) ||
         (parts[0] == 192 && parts[1] == 168) ||
         (parts[0] == 169 && parts[1] == 254);
}

bool local_hostname(std::string_view host) {
  if (host.empty() || host.size() > 128 || host.front() == '-' || host.back() == '-') return false;
  const bool has_dot = host.find('.') != std::string_view::npos;
  if (has_dot && (host.size() <= 6 || host.substr(host.size() - 6) != ".local")) return false;
  std::size_t label_length = 0;
  unsigned char previous = 0;
  for (const unsigned char character : host) {
    if (character == '.') {
      if (label_length == 0 || label_length > 63 || previous == '-') return false;
      label_length = 0;
    } else if (std::isalnum(character) || character == '-') {
      if (character == '-' && label_length == 0) return false;
      ++label_length;
    } else {
      return false;
    }
    previous = character;
  }
  return label_length > 0 && label_length <= 63;
}

}  // namespace

bool is_local_printer_endpoint(std::string_view endpoint, PrinterProtocol protocol) {
  if (endpoint.empty()) return false;
  if (protocol == PrinterProtocol::moonraker) {
    if (endpoint.rfind("http://", 0) == 0) endpoint.remove_prefix(7);
    else if (endpoint.rfind("https://", 0) == 0) endpoint.remove_prefix(8);
  } else if (endpoint.find("://") != std::string_view::npos) {
    return false;
  }
  if (endpoint.empty() || endpoint.find_first_of("/@?#") != std::string_view::npos) return false;

  std::string_view host = endpoint;
  const std::size_t colon = endpoint.rfind(':');
  if (colon != std::string_view::npos) {
    if (protocol != PrinterProtocol::moonraker || endpoint.find(':') != colon) return false;
    host = endpoint.substr(0, colon);
    const std::string_view port_text = endpoint.substr(colon + 1);
    unsigned port = 0;
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (port_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != port_text.data() + port_text.size() || port == 0 || port > 65535) {
      return false;
    }
  }
  return private_ipv4(host) || local_hostname(host);
}

bool supported_audio_preset(std::string_view id) {
  return id == "modern" || id == "soft" || id == "oldschool" ||
         id == "arcade" || id == "scifi" || id == "clean";
}

bool valid_unified_api_token(std::string_view token) {
  if (token.size() != kUnifiedApiTokenLength || token.substr(0, 3) != "pd_") return false;
  return std::all_of(token.begin() + 3, token.end(), [](unsigned char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

bool migrate_settings(std::uint8_t source_schema, DeviceSettings& settings) {
  if (source_schema > kSettingsSchemaVersion) return false;
  if (settings.theme == "blue") settings.theme = "banana";
  if (source_schema < 2 || settings.language.empty()) settings.language = "en";
  if (source_schema < 3) settings.custom_theme.background = ThemeColors{}.background;
  if (source_schema < 4 || settings.audio_preset.empty()) settings.audio_preset = "oldschool";
  if (source_schema < 5) settings.audio_muted_events = 0;
  if (source_schema < 6) settings.printer_animations_enabled = false;
  if (source_schema < 7 &&
      (settings.audio_preset == "tactile" || settings.audio_preset == "impact" ||
       settings.audio_preset == "lucky")) {
    settings.audio_preset = "modern";
  }
  if (source_schema < 8) {
    settings.reaction_progress_bar_enabled = true;
    settings.reaction_progress_percent_enabled = true;
  }
  if (source_schema < 9) {
    settings.unified_api_enabled = false;
    settings.unified_api_token.clear();
  }
  return true;
}

std::vector<ValidationIssue> validate(const DeviceSettings& settings) {
  std::vector<ValidationIssue> issues;
  check_text(issues, "wifi_name", settings.wifi_name, 32, false);
  check_text(issues, "wifi_password", settings.wifi_password, 64, false);
  check_text(issues, "theme", settings.theme, 20, true);
  if (!supported_theme(settings.theme)) {
    issues.push_back({"theme", "Unsupported display theme"});
  }
  check_text(issues, "timezone", settings.timezone, 64, true);
  check_text(issues, "language", settings.language, 8, true);
  if (!supported_language(settings.language)) {
    issues.push_back({"language", "Unsupported interface language"});
  }
  if (settings.rotation != "auto" && settings.rotation != "0" && settings.rotation != "90" &&
      settings.rotation != "180" && settings.rotation != "270") {
    issues.push_back({"rotation", "Unsupported display rotation"});
  }
  if (settings.last_auto_rotation != 0 && settings.last_auto_rotation != 90 &&
      settings.last_auto_rotation != 180 && settings.last_auto_rotation != 270) {
    issues.push_back({"last_auto_rotation", "Unsupported remembered automatic rotation"});
  }

  if (settings.profiles.size() > kMaximumProfiles) {
    issues.push_back({"profiles", "Too many printer profiles"});
  }
  if (settings.brightness_percent < 5 || settings.brightness_percent > 100) {
    issues.push_back({"brightness_percent", "Brightness must be between 5 and 100"});
  }
  if (settings.audio_volume_percent > 100) {
    issues.push_back({"audio_volume_percent", "Audio volume must be between 0 and 100"});
  }
  if (!supported_audio_preset(settings.audio_preset)) {
    issues.push_back({"audio_preset", "Unsupported sound preset"});
  }
  if ((settings.audio_muted_events & ~kAudioEventMuteMask) != 0) {
    issues.push_back({"audio_muted_events", "Unsupported muted sound event"});
  }
  if (settings.camera_mode != "snapshots" && settings.camera_mode != "live") {
    issues.push_back({"camera_mode", "Camera mode must be snapshots or live"});
  }
  if (settings.camera_snapshot_fps != 1 && settings.camera_snapshot_fps != 2 &&
      settings.camera_snapshot_fps != 5) {
    issues.push_back({"camera_snapshot_fps", "Camera snapshot rate must be 1, 2 or 5 FPS"});
  }
  if ((!settings.unified_api_token.empty() &&
       !valid_unified_api_token(settings.unified_api_token)) ||
      (settings.unified_api_enabled && settings.unified_api_token.empty())) {
    issues.push_back({"unified_api_token", "Unified Printer API token is invalid"});
  }
  const std::uint32_t poll_interval = settings.inactive_printer_poll_interval_s;
  if (poll_interval != 0 && poll_interval != 30 && poll_interval != 60 &&
      poll_interval != 180 && poll_interval != 300) {
    issues.push_back({"inactive_printer_poll_interval_s",
                      "Inactive printer refresh must be off or 30, 60, 180 or 300 seconds"});
  }
  const DisplayPowerPolicy& power = settings.display_power;
  if (power.dim_brightness_percent > 100) {
    issues.push_back({"display_power.dim_brightness_percent",
                      "Dim brightness must be automatic or between 1 and 100"});
  }
  const auto valid_timeout = [](std::uint32_t seconds) {
    return seconds > 0 && seconds <= 3600;
  };
  if (!valid_timeout(power.dim_timeout_idle_s) ||
      !valid_timeout(power.dim_timeout_active_s) ||
      !valid_timeout(power.off_timeout_idle_s) ||
      !valid_timeout(power.off_timeout_active_s)) {
    issues.push_back({"display_power", "Display power timeouts must be between 1 and 3600 seconds"});
  }

  std::unordered_set<std::uint32_t> ids;
  bool selected_exists = settings.selected_profile == 0;
  for (std::size_t index = 0; index < settings.profiles.size(); ++index) {
    const PrinterProfile& profile = settings.profiles[index];
    const std::string prefix = "profiles[" + std::to_string(index) + "].";
    if (profile.id == 0 || !ids.insert(profile.id).second) {
      issues.push_back({prefix + "id", "Profile ID must be non-zero and unique"});
    }
    check_text(issues, (prefix + "display_name").c_str(), profile.display_name, 48, true);
    check_text(issues, (prefix + "endpoint").c_str(), profile.endpoint, 128, true);
    if (!profile.endpoint.empty() && !is_local_printer_endpoint(profile.endpoint, profile.protocol)) {
      issues.push_back({prefix + "endpoint", "Printer address must be on the local network"});
    }
    check_text(issues, (prefix + "api_key").c_str(), profile.api_key, 128, false);
    check_text(issues, (prefix + "serial").c_str(), profile.serial, 32,
               printer_supports(profile.protocol, PrinterCapability::serial_number));
    check_text(issues, (prefix + "access_code").c_str(), profile.access_code, 32,
               printer_supports(profile.protocol, PrinterCapability::access_code));
    check_text(issues, (prefix + "manufacturer").c_str(), profile.manufacturer, 48,
               profile.protocol == PrinterProtocol::moonraker);
    check_text(issues, (prefix + "model").c_str(), profile.model, 48, false);
    check_text(issues, (prefix + "brand").c_str(), profile.brand, 24, false);
    selected_exists = selected_exists || profile.id == settings.selected_profile;
  }
  if (!selected_exists) {
    issues.push_back({"selected_profile", "Selected profile does not exist"});
  }
  return issues;
}

DeviceSettings redact_secrets(DeviceSettings settings) {
  settings.wifi_password.clear();
  settings.unified_api_token.clear();
  for (auto& profile : settings.profiles) {
    profile.api_key.clear();
    profile.access_code.clear();
  }
  return settings;
}

}  // namespace printdeck::core
