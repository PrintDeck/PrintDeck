#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "printdeck/core/settings.hpp"

namespace printdeck::core {

constexpr std::uint8_t kConfigurationBackupFormatVersion = 1;
constexpr std::size_t kMaximumConfigurationBackupBytes = 32U * 1024U;

enum class ConfigurationBackupActivity : std::uint8_t {
  idle,
  backing_up,
  restoring,
  restarting,
};

struct ConfigurationBackupReactionEvent {
  std::string id;
  bool enabled = true;
  std::size_t custom_bytes = 0;
};

struct ConfigurationBackupReactions {
  std::string active_set;
  std::vector<ConfigurationBackupReactionEvent> events;
};

enum class ConfigurationBackupError : std::uint8_t {
  none,
  invalid_document,
  unsupported_format,
  unsupported_version,
  incompatible_hardware,
  invalid_settings,
};

struct ConfigurationBackupSummary {
  std::uint8_t format_version = 0;
  std::uint8_t settings_schema = 0;
  std::string hardware_id;
  std::size_t profile_count = 0;
};

struct ConfigurationBackupResult {
  ConfigurationBackupError error = ConfigurationBackupError::invalid_document;
  ConfigurationBackupSummary summary;
  DeviceSettings settings;
  ConfigurationBackupReactions reactions;

  explicit operator bool() const { return error == ConfigurationBackupError::none; }
};

std::string serialize_configuration_backup(const DeviceSettings& settings,
                                           std::string_view hardware_id,
                                           const ConfigurationBackupReactions& reactions);
ConfigurationBackupResult parse_configuration_backup(
    std::string_view document, std::string_view expected_hardware_id);

}  // namespace printdeck::core
