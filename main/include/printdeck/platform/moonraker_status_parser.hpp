#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {

enum class MoonrakerLightKind : std::uint8_t {
  none,
  led,
  output_pin,
};

struct MoonrakerLightDescriptor {
  MoonrakerLightKind kind = MoonrakerLightKind::none;
  std::string object_name;
  std::string config_name;
};

struct MoonrakerStatusParseContext {
  std::vector<std::string> tool_objects;
  std::string chamber_sensor_object;
  MoonrakerLightDescriptor chamber_light;
  bool chamber_light_pending = false;
  bool chamber_light_target_on = false;
  std::shared_ptr<std::vector<std::uint8_t>> preview;
  std::uint32_t estimated_seconds = 0;
  std::uint16_t total_layers = 0;
};

struct MoonrakerStatusParseResult {
  bool parsed = false;
  bool ready = false;
  core::PrinterSnapshot snapshot;
};

core::JobPhase moonraker_phase(std::string_view status);
MoonrakerLightDescriptor discover_moonraker_light(
    const std::vector<std::string>& object_names);
MoonrakerStatusParseResult parse_moonraker_status(
    const char* payload, std::size_t length, std::uint32_t profile_id,
    std::uint64_t updated_at_ms, const MoonrakerStatusParseContext& context);

}  // namespace printdeck::platform
