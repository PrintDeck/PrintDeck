#include "printdeck/platform/moonraker_status_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>

#include "cJSON.h"

namespace printdeck::platform {
namespace {

struct JsonDeleter {
  void operator()(cJSON* value) const { cJSON_Delete(value); }
};
using JsonDocument = std::unique_ptr<cJSON, JsonDeleter>;

const cJSON* member(const cJSON* object, const char* key) {
  return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

std::string string_member(const cJSON* object, const char* key) {
  const cJSON* value = member(object, key);
  return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : "";
}

double number_member(const cJSON* object, const char* key, double fallback = 0.0) {
  const cJSON* value = member(object, key);
  return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) ? value->valuedouble
                                                                   : fallback;
}

double array_number(const cJSON* array, int index, double fallback = 0.0) {
  const cJSON* value = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, index) : nullptr;
  return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) ? value->valuedouble
                                                                   : fallback;
}

bool bool_member(const cJSON* object, const char* key, bool fallback = false) {
  const cJSON* value = member(object, key);
  return cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback;
}

std::uint32_t rgba_from_array(const cJSON* array, int index) {
  const cJSON* value = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, index) : nullptr;
  if (!cJSON_IsString(value) || value->valuestring == nullptr) return 0;
  std::string color(value->valuestring);
  if (!color.empty() && color.front() == '#') color.erase(0, 1);
  if (color.empty()) return 0;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(color.c_str(), &end, 16);
  if (end == color.c_str() || *end != '\0') return 0;
  return color.size() <= 6 ? (static_cast<std::uint32_t>(parsed) << 8U) | 0xFFU
                           : static_cast<std::uint32_t>(parsed);
}

std::string display_job_name(std::string path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash != std::string::npos) path.erase(0, slash + 1);
  constexpr std::string_view extension = ".gcode";
  if (path.size() >= extension.size() &&
      path.compare(path.size() - extension.size(), extension.size(), extension) == 0) {
    path.resize(path.size() - extension.size());
  }
  return path;
}

std::string lowercase_ascii(std::string_view text) {
  std::string result(text);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return result;
}

bool contains_any(std::string_view text,
                  std::initializer_list<std::string_view> values) {
  for (const std::string_view value : values) {
    if (text.find(value) != std::string_view::npos) return true;
  }
  return false;
}

int light_name_score(std::string name) {
  name = lowercase_ascii(name);
  for (char& value : name) {
    if (value == '-' || value == ' ') value = '_';
  }
  const bool location = contains_any(name, {"chamber", "cavity", "enclosure", "case"});
  const bool light = contains_any(name, {"light", "lamp"});
  const bool led = name == "led" || name.ends_with("_led") || name.starts_with("led_") ||
                   name.find("_led_") != std::string::npos;
  if (location && (light || led)) return 120;
  if (contains_any(name, {"printer_light", "work_light", "main_light"})) return 115;
  if (light) return 100;
  if (name == "led") return 80;
  return 0;
}

bool led_is_on(const cJSON* object) {
  const cJSON* colors = member(object, "color_data");
  if (!cJSON_IsArray(colors)) return false;
  const cJSON* color = nullptr;
  cJSON_ArrayForEach(color, colors) {
    if (!cJSON_IsArray(color)) continue;
    const int channels = cJSON_GetArraySize(color);
    for (int index = 0; index < channels; ++index) {
      const cJSON* value = cJSON_GetArrayItem(color, index);
      if (cJSON_IsNumber(value) && value->valuedouble > 0.001) return true;
    }
  }
  return false;
}

}  // namespace

core::JobPhase moonraker_phase(std::string_view status) {
  if (status == "standby") return core::JobPhase::idle;
  if (status == "printing") return core::JobPhase::printing;
  if (status == "paused") return core::JobPhase::paused;
  if (status == "complete") return core::JobPhase::completed;
  if (status == "error") return core::JobPhase::failed;
  if (status == "cancelled") return core::JobPhase::cancelled;
  return core::JobPhase::unknown;
}

MoonrakerLightDescriptor discover_moonraker_light(
    const std::vector<std::string>& object_names) {
  MoonrakerLightDescriptor best;
  int best_score = 0;
  bool ambiguous = false;
  for (const std::string& object_name : object_names) {
    static constexpr std::string_view kLedPrefixes[]{
        "led ", "neopixel ", "dotstar ", "pca9533 ", "pca9632 ",
    };
    MoonrakerLightKind kind = MoonrakerLightKind::none;
    std::string config_name;
    for (const std::string_view prefix : kLedPrefixes) {
      if (object_name.rfind(prefix, 0) == 0 && object_name.size() > prefix.size()) {
        kind = MoonrakerLightKind::led;
        config_name = object_name.substr(prefix.size());
        break;
      }
    }
    if (kind == MoonrakerLightKind::none) {
      static constexpr std::string_view kPinPrefixes[]{"output_pin ", "pwm_tool "};
      for (const std::string_view prefix : kPinPrefixes) {
        if (object_name.rfind(prefix, 0) == 0 && object_name.size() > prefix.size()) {
          kind = MoonrakerLightKind::output_pin;
          config_name = object_name.substr(prefix.size());
          break;
        }
      }
    }
    if (kind == MoonrakerLightKind::none) continue;
    const int score = light_name_score(config_name);
    if (score == 0) continue;
    if (score > best_score) {
      best = {.kind = kind, .object_name = object_name, .config_name = config_name};
      best_score = score;
      ambiguous = false;
    } else if (score == best_score) {
      ambiguous = true;
    }
  }
  return ambiguous ? MoonrakerLightDescriptor{} : best;
}

MoonrakerStatusParseResult parse_moonraker_status(
    const char* payload, std::size_t length, std::uint32_t profile_id,
    std::uint64_t updated_at_ms, const MoonrakerStatusParseContext& context) {
  MoonrakerStatusParseResult result;
  if (payload == nullptr || length == 0) return result;
  JsonDocument document(cJSON_ParseWithLength(payload, length));
  const cJSON* status = member(member(document.get(), "result"), "status");
  if (!document || !cJSON_IsObject(status)) return result;
  result.parsed = true;
  const cJSON* webhooks = member(status, "webhooks");
  if (string_member(webhooks, "state") != "ready") return result;
  result.ready = true;

  core::PrinterSnapshot& next = result.snapshot;
  next.profile_id = profile_id;
  next.link = core::LinkState::online;
  next.link_detail = "Connected";
  next.updated_at_ms = updated_at_ms;
  next.job.reachable = true;

  const cJSON* stats = member(status, "print_stats");
  const cJSON* virtual_sd = member(status, "virtual_sdcard");
  const cJSON* display = member(status, "display_status");
  next.job.phase = moonraker_phase(string_member(stats, "state"));
  next.job.gcode_file = string_member(stats, "filename");
  next.job.name = display_job_name(next.job.gcode_file);
  next.job.preview = context.preview;
  next.job.detail = string_member(stats, "message");
  const double progress = std::clamp(
      number_member(display, "progress", number_member(virtual_sd, "progress")), 0.0, 1.0);
  const double elapsed = std::max(0.0, number_member(stats, "print_duration"));
  next.job.completion = static_cast<float>(progress * 100.0);
  next.job.elapsed_seconds = static_cast<std::uint32_t>(elapsed);
  if (progress > 0.001 && progress < 1.0 && elapsed > 0.0) {
    next.job.remaining_seconds = static_cast<std::uint32_t>(elapsed / progress - elapsed);
  } else if (context.estimated_seconds > elapsed) {
    next.job.remaining_seconds = context.estimated_seconds - static_cast<std::uint32_t>(elapsed);
  }
  const cJSON* layer_info = member(stats, "info");
  next.job.current_layer = static_cast<std::uint16_t>(
      std::clamp(number_member(layer_info, "current_layer"), 0.0, 65535.0));
  next.job.total_layers = static_cast<std::uint16_t>(
      std::clamp(number_member(layer_info, "total_layer"), 0.0, 65535.0));
  if (next.job.total_layers == 0) next.job.total_layers = context.total_layers;

  const cJSON* toolhead = member(status, "toolhead");
  const std::string active_extruder = string_member(toolhead, "extruder");
  next.job.toolhead_count = static_cast<std::uint8_t>(
      std::min(context.tool_objects.size(), core::kMaximumToolheads));
  const cJSON* task_config = member(status, "print_task_config");
  const cJSON* filament_types = member(task_config, "filament_type");
  const cJSON* filament_colors = member(task_config, "filament_color_rgba");
  const cJSON* filament_exists = member(task_config, "filament_exist");
  for (std::size_t index = 0; index < next.job.toolhead_count; ++index) {
    const std::string& object_name = context.tool_objects[index];
    const cJSON* tool = member(status, object_name.c_str());
    core::ToolheadState& info = next.job.toolheads[index];
    info.present = cJSON_IsObject(tool);
    info.active = object_name == active_extruder || bool_member(tool, "active_pin");
    info.temperature_known = cJSON_IsNumber(member(tool, "temperature"));
    info.temperature_c = static_cast<float>(number_member(tool, "temperature"));
    info.target_c = static_cast<float>(number_member(tool, "target"));
    info.heater_power_known = cJSON_IsNumber(member(tool, "power"));
    info.heater_power = static_cast<float>(number_member(tool, "power"));
    info.nozzle_diameter_mm = static_cast<float>(number_member(tool, "nozzle_diameter"));
    info.state = string_member(tool, "state");
    const cJSON* material = cJSON_IsArray(filament_types)
                                ? cJSON_GetArrayItem(filament_types, static_cast<int>(index))
                                : nullptr;
    if (cJSON_IsString(material) && material->valuestring != nullptr) {
      info.material = material->valuestring;
      if (info.material == "NONE") info.material.clear();
    }
    const cJSON* exists = cJSON_IsArray(filament_exists)
                              ? cJSON_GetArrayItem(filament_exists, static_cast<int>(index))
                              : nullptr;
    if (cJSON_IsBool(exists)) {
      info.filament_state_known = true;
      info.filament_detected = cJSON_IsTrue(exists);
    } else if (!info.material.empty()) {
      info.filament_state_known = true;
      info.filament_detected = true;
    }
    info.material_rgba = info.filament_detected
                             ? rgba_from_array(filament_colors, static_cast<int>(index))
                             : 0;
    if (info.active) next.job.active_toolhead = static_cast<int>(index);
  }
  if (next.job.active_toolhead < 0 && next.job.toolhead_count > 0) {
    next.job.active_toolhead = 0;
    next.job.toolheads[0].active = true;
  }

  const cJSON* extruder = next.job.active_toolhead >= 0
                              ? member(status, context.tool_objects[next.job.active_toolhead].c_str())
                              : member(status, "extruder");
  const cJSON* bed = member(status, "heater_bed");
  next.job.temperatures.nozzle_c = static_cast<float>(number_member(extruder, "temperature"));
  next.job.temperatures.nozzle_target_c = static_cast<float>(number_member(extruder, "target"));
  next.job.temperatures.bed_c = static_cast<float>(number_member(bed, "temperature"));
  next.job.temperatures.bed_target_c = static_cast<float>(number_member(bed, "target"));
  next.job.bed_heater_power_known = cJSON_IsNumber(member(bed, "power"));
  next.job.bed_heater_power = static_cast<float>(number_member(bed, "power"));
  const cJSON* chamber = context.chamber_sensor_object.empty()
                             ? nullptr : member(status, context.chamber_sensor_object.c_str());
  if (cJSON_IsNumber(member(chamber, "temperature"))) {
    next.job.temperatures.chamber_c = static_cast<float>(number_member(chamber, "temperature"));
    next.job.temperatures.chamber_known = true;
  }

  const cJSON* chamber_light = context.chamber_light.object_name.empty()
                                   ? nullptr
                                   : member(status, context.chamber_light.object_name.c_str());
  if (cJSON_IsObject(chamber_light)) {
    if (context.chamber_light.kind == MoonrakerLightKind::output_pin &&
        cJSON_IsNumber(member(chamber_light, "value"))) {
      next.job.chamber_light_supported = true;
      next.job.chamber_light_on = number_member(chamber_light, "value") > 0.001;
    } else if (context.chamber_light.kind == MoonrakerLightKind::led &&
               cJSON_IsArray(member(chamber_light, "color_data"))) {
      next.job.chamber_light_supported = true;
      next.job.chamber_light_on = led_is_on(chamber_light);
    }
  }
  if (next.job.chamber_light_supported && context.chamber_light_pending &&
      next.job.chamber_light_on != context.chamber_light_target_on) {
    next.job.chamber_light_pending = true;
    next.job.chamber_light_target_on = context.chamber_light_target_on;
  }

  const cJSON* position = member(toolhead, "position");
  next.job.motion.x_mm = static_cast<float>(array_number(position, 0));
  next.job.motion.y_mm = static_cast<float>(array_number(position, 1));
  next.job.motion.z_mm = static_cast<float>(array_number(position, 2));
  next.job.motion.position_known = cJSON_IsArray(position) && cJSON_GetArraySize(position) >= 3;
  next.job.motion.homed_axes = string_member(toolhead, "homed_axes");
  const cJSON* movement = member(status, "gcode_move");
  const cJSON* motion_report = member(status, "motion_report");
  const cJSON* live_velocity = member(motion_report, "live_velocity");
  next.job.motion.velocity_mm_s = static_cast<float>(number_member(motion_report, "live_velocity"));
  next.job.motion.velocity_known = cJSON_IsNumber(live_velocity);
  next.job.motion.speed_multiplier = static_cast<float>(number_member(movement, "speed_factor") * 100.0);
  next.job.motion.speed_multiplier_known = cJSON_IsNumber(member(movement, "speed_factor"));
  next.job.motion.extrusion_multiplier = static_cast<float>(number_member(movement, "extrude_factor") * 100.0);
  next.job.motion.extrusion_multiplier_known = cJSON_IsNumber(member(movement, "extrude_factor"));
  next.job.motion.fan_percent = static_cast<float>(number_member(member(status, "fan"), "speed") * 100.0);
  next.job.motion.fan_percent_known = cJSON_IsNumber(member(member(status, "fan"), "speed"));
  return result;
}

}  // namespace printdeck::platform
