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

bool bounded_number(const cJSON* value, double minimum, double maximum) {
  return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
         value->valuedouble >= minimum && value->valuedouble <= maximum;
}

std::uint16_t first_layer_value(std::initializer_list<const cJSON*> values) {
  for (const cJSON* value : values) {
    if (bounded_number(value, 1, 65535) && std::floor(value->valuedouble) == value->valuedouble) {
      return static_cast<std::uint16_t>(value->valuedouble);
    }
  }
  return 0;
}

// Ordered by role and protocol: never substitute a hotend or chamber fan.
constexpr std::string_view kPartFanObjects[]{
    "fan", "fan_generic part_fan", "fan_generic partfan",
    "fan_generic part_cooling_fan", "fan_generic part_cooling",
    "fan_generic print_fan", "fan_generic print_cooling", "fan_generic fan0",
    "output_pin fan0", "pwm_tool fan0",
};

void apply_part_fan(const cJSON* status, core::JobState& job) {
  for (const auto name : kPartFanObjects) {
    const bool pin = name.starts_with("output_pin ") || name.starts_with("pwm_tool ");
    const cJSON* value = member(member(status, name.data()), pin ? "value" : "speed");
    if (!bounded_number(value, 0, 1)) continue;
    double speed = value->valuedouble;
    if (pin) {
      // Creality M106 adds fan0_min (0..255) before setting the normalized pin.
      // Recover the requested percentage, including quiet-mode limiting.
      const cJSON* minimum = member(member(status, "gcode_macro PRINTER_PARAM"), "fan0_min");
      if (speed > 0 && bounded_number(minimum, 0, 254)) {
        speed = std::clamp((speed * 255.0 - minimum->valuedouble) /
                               (255.0 - minimum->valuedouble), 0.0, 1.0);
      }
      const cJSON* enabled = member(member(status, "output_pin fan0_en"), "value");
      if (bounded_number(enabled, 0, 1) && enabled->valuedouble == 0) speed = 0;
    }
    job.motion.fan_percent = static_cast<float>(speed * 100.0);
    job.motion.fan_percent_known = true;
    return;  // A valid zero is authoritative; it must not fall through.
  }
}

std::string creality_material(std::string_view code) {
  // Numeric filament IDs from Creality Print profiles; firmware pads to six digits.
  if (code.empty() || code.size() > 6) return {};
  unsigned id = 0;
  for (const char c : code) {
    if (c < '0' || c > '9') return {};
    id = id * 10 + static_cast<unsigned>(c - '0');
  }
  switch (id) {
    case 1: return "PLA";
    case 2: return "PLA-Silk";
    case 3: return "PETG";
    case 4: return "ABS";
    case 5: return "TPU";
    case 6: return "PLA-CF";
    case 7: return "ASA";
    case 8: return "PA";
    case 9: return "PA-CF";
    case 10: return "BVOH";
    case 11: return "PVA";
    case 12: return "HIPS";
    case 13: return "PET-CF";
    case 14: return "PETG-CF";
    case 17: return "PPS";
    case 18: return "PPS-CF";
    case 19: return "PP";
    case 20: return "PET";
    case 21: return "PC";
    case 27: return "PETG-GF";
    case 32: return "PCTG";
    case 1001: return "Hyper PLA";
    case 2001: return "Hyper PLA-CF";
    case 3001: return "Hyper ABS";
    case 4001: return "CR-PLA";
    case 5001: return "CR-Silk";
    case 6001: return "CR-PETG";
    case 6002: return "Hyper PETG";
    case 6003: return "Hyper PETG-CF";
    case 6004: return "Hyper PETG-GF";
    case 7001: return "CR-ABS";
    case 7002: return "Hyper PC";
    case 8001: return "Ender-PLA";
    case 9001: return "EN-PLA+";
    case 9002: return "ENDER FAST PLA";
    case 10001: return "HP-TPU";
    case 11001: return "CR-Nylon";
    case 19001: return "HP-ASA";
    default: return {};
  }
}

std::uint32_t creality_color(std::string color) {
  if (!color.empty() && color.front() == '#') color.erase(0, 1);
  if (color.size() == 7 && color.front() == '0') color.erase(0, 1);
  if (color.size() != 6) return 0;
  std::uint32_t rgb = 0;
  for (const unsigned char c : color) {
    if (!std::isxdigit(c)) return 0;
    rgb = (rgb << 4U) | (c <= '9' ? c - '0' : std::tolower(c) - 'a' + 10);
  }
  return (rgb << 8U) | 0xFFU;  // Black is present (opaque), not an unknown color.
}

void apply_external_filament(const cJSON* status, const cJSON* job_metadata,
                             core::JobState& job) {
  const cJSON* rack = member(status, "filament_rack");
  const cJSON* box_enabled = member(member(status, "box"), "enable");
  const bool external = cJSON_IsFalse(box_enabled) ||
                        (bounded_number(box_enabled, 0, 0));
  if (!cJSON_IsObject(rack) || !external || job.toolhead_count != 1 ||
      !job.toolheads[0].present) return;
  auto& tool = job.toolheads[0];
  // Remaining-material fields describe the previous load, not the current spool.
  tool.material = creality_material(string_member(rack, "material_type"));
  if (tool.material.empty()) {
    const std::string type = string_member(job_metadata, "filament_type");
    // A job's single material is a fallback only; never assign a multi-material list.
    if (!type.empty() && type.size() <= 24 &&
        type.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-+ ") ==
            std::string::npos) tool.material = type;
  }
  tool.filament_state_known = false;
  tool.filament_detected = false;
  const cJSON* detected = member(member(status, "filament_switch_sensor filament_sensor"),
                                 "filament_detected");
  if (cJSON_IsBool(detected)) {
    tool.filament_state_known = true;
    tool.filament_detected = cJSON_IsTrue(detected);
  }
  tool.material_rgba = tool.filament_state_known && !tool.filament_detected
                           ? 0 : creality_color(string_member(rack, "color_value"));
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

void apply_snapmaker_activity(const cJSON* status, core::JobState& job,
                              const std::vector<std::string>& tool_objects) {
  using Activity = core::PrinterActivity;
  using Detail = core::JobState::ActivityDetail;
  if (job.phase != core::JobPhase::printing &&
      job.phase != core::JobPhase::preparing && job.phase != core::JobPhase::idle) return;
  const cJSON* manager = member(status, "machine_state_manager");
  const cJSON* action = member(manager, "action_code");
  if (!cJSON_IsNumber(action) || !std::isfinite(action->valuedouble) ||
      action->valuedouble < 0 || action->valuedouble > 65535 ||
      action->valuedouble != action->valueint) return;
  if (job.phase == core::JobPhase::idle && number_member(manager, "main_state", -1) == 0) return;
  job.activity_status_available = true;
  switch (action->valueint) {
    case 2:
    case 136:
      job.activity = Activity::preparing;
      job.activity_detail = Detail::bed_detection;
      break;
    case 132:
      job.activity = Activity::filament_changing;
      job.activity_detail = Detail::tool_check;
      break;
    case 134:
      // PRINT_PREEXTRUDING follows activation; it is not the mechanical swap.
      job.activity = Activity::filament_purging;
      job.activity_toolhead = job.active_toolhead;
      break;
    case 320:
    case 321:
    case 322:
    case 323:
      job.activity = Activity::calibrating;
      job.activity_detail = Detail::flow_calibration;
      job.activity_toolhead = action->valueint - 320;
      break;
    case 257:
      job.activity = Activity::bed_heating;
      job.activity_detail = Detail::bed_preheat;
      break;
    case 258:
      job.activity = Activity::bed_leveling;
      job.activity_detail = Detail::bed_prescan;
      break;
    case 256: job.activity = Activity::bed_leveling; break;
    case 0: {
      if (job.phase != core::JobPhase::printing) break;
      const cJSON* layer = member(member(member(status, "print_stats"), "info"), "current_layer");
      // A reported layer zero identifies preparation in U1 start macros.
      // Neither elapsed time nor the global PRINTING state identifies the model.
      if (cJSON_IsNumber(layer) && layer->valuedouble == 0) {
        job.activity = Activity::preparing;
        const auto& temperatures = job.temperatures;
        if (temperatures.bed_known && temperatures.bed_target_known &&
            temperatures.bed_target_c > temperatures.bed_c + 2.0F) {
          job.activity = Activity::bed_heating;
        } else if (temperatures.nozzle_known && temperatures.nozzle_target_known &&
                   temperatures.nozzle_target_c > temperatures.nozzle_c + 2.0F) {
          job.activity = Activity::nozzle_heating;
        }
      } else if (job.current_layer > 0) {
        bool has_attachment_state = false;
        for (const auto& tool : job.toolheads) {
          has_attachment_state |= tool.present && !tool.state.empty();
        }
        const std::string logical_tool = string_member(member(status, "toolhead"), "extruder");
        const std::string attached_tool = job.active_toolhead < 0 ? "" :
            tool_objects[job.active_toolhead];
        if (has_attachment_state && (job.active_toolhead < 0 ||
            logical_tool != attached_tool ||
            job.toolheads[job.active_toolhead].state == "UNKNOWN")) {
          job.activity = Activity::filament_changing;
          job.activity_toolhead = job.active_toolhead;
        }
      }
      break;
    }
    default: break;
  }
  const cJSON* progress = member(member(status, "bed_mesh"), "progress");
  const std::string probe_state = string_member(progress, "probe_state");
  if (action->valueint == 256 && probe_state == "probing") {
    const double current = number_member(progress, "current_point", -1);
    const double total = number_member(progress, "total_points", -1);
    if (current >= 0 && current <= total && total > 0 && total <= 65535 &&
        std::floor(current) == current && std::floor(total) == total) {
      job.activity_current = static_cast<std::uint16_t>(current);
      job.activity_total = static_cast<std::uint16_t>(total);
    }
  }
  if (action->valueint == 257 && probe_state == "preheating") {
    const double remaining = number_member(progress, "preheat_remaining_time", -1);
    const double total = number_member(progress, "preheat_total_duration", -1);
    if (remaining >= 0 && remaining <= total && total <= 3600 &&
        std::floor(remaining) == remaining) {
      job.activity_remaining_seconds = static_cast<int>(remaining);
    }
  }
  if (job.phase == core::JobPhase::idle && job.activity != Activity::unknown) {
    job.condition = core::PrinterCondition::busy;
    if (job.activity == Activity::bed_leveling || job.activity == Activity::calibrating) {
      job.kind = core::JobKind::calibration;
    }
  }
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

std::string_view moonraker_telemetry_fields(std::string_view object_name) {
  for (const auto fan : kPartFanObjects) {
    if (object_name == fan) {
      return fan.starts_with("output_pin ") || fan.starts_with("pwm_tool ") ? "value" : "speed";
    }
  }
  if (object_name == "filament_rack") return "material_type,color_value";
  if (object_name == "box") return "enable";
  if (object_name == "filament_switch_sensor filament_sensor") return "filament_detected";
  if (object_name == "gcode_macro PRINTER_PARAM") return "fan0_min";
  if (object_name == "output_pin fan0_en") return "value";
  return {};
}

namespace {
const cJSON* current_job_metadata(const cJSON* status) {
  const cJSON* stats = member(status, "print_stats");
  const auto filename = string_member(stats, "filename");
  const auto phase = moonraker_phase(string_member(stats, "state"));
  const cJSON* current = member(member(status, "virtual_sdcard"), "cur_print_data");
  if (filename.empty() || string_member(current, "filename") != filename ||
      (phase != core::JobPhase::printing && phase != core::JobPhase::paused)) return nullptr;
  const cJSON* metadata = member(current, "metadata");
  return cJSON_IsObject(metadata) ? metadata : nullptr;
}

const cJSON* progress_value(const cJSON* status) {
  const cJSON* file = member(member(status, "virtual_sdcard"), "progress");
  const cJSON* display = member(member(status, "display_status"), "progress");
  // Appliances exposing a matching current-job record use virtual_sdcard for
  // their native progress. Their M73/display value may describe a different
  // estimate (observed 39% versus 89% on the physical display).
  if (current_job_metadata(status) && bounded_number(file, 0, 1)) return file;
  if (bounded_number(display, 0, 1)) return display;
  return bounded_number(file, 0, 1) ? file : nullptr;
}
}  // namespace

double moonraker_progress(const cJSON* status) {
  const cJSON* value = progress_value(status);
  return value ? value->valuedouble : 0;
}

MoonrakerJobTiming moonraker_job_timing(const cJSON* status,
                                      std::uint32_t estimated_seconds) {
  MoonrakerJobTiming result;
  const cJSON* value = progress_value(status);
  const double progress = value ? value->valuedouble : 0;
  result.completion_known = value != nullptr;
  result.completion = static_cast<float>(progress * 100.0);
  const cJSON* duration = member(member(status, "print_stats"), "print_duration");
  result.elapsed_known = bounded_number(duration, 0, 4294967295.0);
  if (!result.elapsed_known) return result;
  const double elapsed = duration->valuedouble;
  result.elapsed_seconds = static_cast<std::uint32_t>(elapsed);
  const cJSON* estimate = member(current_job_metadata(status), "estimated_time");
  if (bounded_number(estimate, 1, 4294967295.0))
    estimated_seconds = static_cast<std::uint32_t>(estimate->valuedouble);
  // A slicer estimate is not a deadline: a live job can outlast it. Once it
  // expires, project the remaining layers at the observed average layer time.
  // Fall back to the same validated progress source used by the dashboard.
  if (estimated_seconds > elapsed) {
    result.remaining_seconds = estimated_seconds - result.elapsed_seconds;
    result.remaining_known = true;
  } else if (elapsed > 0) {
    const auto phase = moonraker_phase(string_member(member(status, "print_stats"), "state"));
    const bool active = phase == core::JobPhase::printing || phase == core::JobPhase::paused;
    double fraction = progress;
    if (estimated_seconds > 0) {
      const cJSON* info = member(member(status, "print_stats"), "info");
      const cJSON* sd = member(status, "virtual_sdcard");
      const auto layer = first_layer_value({member(info, "current_layer"), member(sd, "layer")});
      const auto total = first_layer_value({member(info, "total_layer"), member(sd, "layer_count"),
                                            member(current_job_metadata(status), "layer_count")});
      if (layer > 0 && total > layer) fraction = static_cast<double>(layer) / total;
    }
    const double remaining = fraction > 0.001 && fraction < 1.0
                                 ? elapsed * (1.0 - fraction) / fraction : 0;
    if (active && std::isfinite(remaining) && remaining > 0 && remaining <= 4294967295.0) {
      result.remaining_seconds = static_cast<std::uint32_t>(std::ceil(remaining));
      result.remaining_known = true;
    }
  }
  return result;
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
  next.job.phase = moonraker_phase(string_member(stats, "state"));
  next.job.gcode_file = string_member(stats, "filename");
  const cJSON* current_print = member(virtual_sd, "cur_print_data");
  const cJSON* job_metadata = !next.job.gcode_file.empty() &&
          string_member(current_print, "filename") == next.job.gcode_file &&
          (next.job.phase == core::JobPhase::printing || next.job.phase == core::JobPhase::paused)
      ? member(current_print, "metadata") : nullptr;
  next.job.name = display_job_name(next.job.gcode_file);
  next.job.preview = context.preview;
  next.job.detail = string_member(stats, "message");
  const auto timing = moonraker_job_timing(status, context.estimated_seconds);
  next.job.completion = timing.completion;
  next.job.completion_known = timing.completion_known;
  next.job.elapsed_seconds = timing.elapsed_seconds;
  next.job.elapsed_known = timing.elapsed_known;
  next.job.remaining_seconds = timing.remaining_seconds;
  next.job.remaining_known = timing.remaining_known;
  const cJSON* layer_info = member(stats, "info");
  next.job.current_layer = first_layer_value(
      {member(layer_info, "current_layer"), member(virtual_sd, "layer")});
  next.job.total_layers = first_layer_value(
      {member(layer_info, "total_layer"), member(virtual_sd, "layer_count"),
       member(job_metadata, "layer_count")});
  if (next.job.total_layers == 0) next.job.total_layers = context.total_layers;

  const cJSON* toolhead = member(status, "toolhead");
  const std::string active_extruder = string_member(toolhead, "extruder");
  next.job.toolhead_count = static_cast<std::uint8_t>(
      std::min(context.tool_objects.size(), core::kMaximumToolheads));
  const cJSON* task_config = member(status, "print_task_config");
  const cJSON* filament_types = member(task_config, "filament_type");
  const cJSON* filament_colors = member(task_config, "filament_color_rgba");
  const cJSON* filament_exists = member(task_config, "filament_exist");
  int logical_toolhead = -1;
  int attached_toolhead = -1;
  int attachment_count = 0;
  bool attachment_known = false;
  for (std::size_t index = 0; index < next.job.toolhead_count; ++index) {
    const std::string& object_name = context.tool_objects[index];
    const cJSON* tool = member(status, object_name.c_str());
    core::ToolheadState& info = next.job.toolheads[index];
    info.present = cJSON_IsObject(tool);
    if (info.present && object_name == active_extruder) logical_toolhead = static_cast<int>(index);
    if (cJSON_IsBool(member(tool, "active_pin"))) {
      attachment_known = true;
      if (bool_member(tool, "active_pin")) {
        attached_toolhead = static_cast<int>(index);
        ++attachment_count;
      }
    }
    info.temperature_known = cJSON_IsNumber(member(tool, "temperature"));
    info.target_known = cJSON_IsNumber(member(tool, "target"));
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
  }
  // Physical attachment wins over the delayed logical selection. During an
  // exchange every tool can be parked; never invent an active first tool.
  next.job.active_toolhead = attachment_known
                                ? (attachment_count == 1 ? attached_toolhead : -1)
                                : logical_toolhead;
  if (!attachment_known && next.job.active_toolhead < 0) {
    for (std::size_t index = 0; index < next.job.toolhead_count; ++index) {
      if (next.job.toolheads[index].present) {
        next.job.active_toolhead = static_cast<int>(index);
        break;
      }
    }
  }
  for (std::size_t index = 0; index < next.job.toolhead_count; ++index) {
    next.job.toolheads[index].active = static_cast<int>(index) == next.job.active_toolhead;
  }
  apply_external_filament(status, job_metadata, next.job);

  const cJSON* extruder = next.job.active_toolhead >= 0
                              ? member(status, context.tool_objects[next.job.active_toolhead].c_str())
                              : attachment_known ? nullptr : member(status, "extruder");
  const cJSON* bed = member(status, "heater_bed");
  next.job.temperatures.nozzle_c = static_cast<float>(number_member(extruder, "temperature"));
  next.job.temperatures.nozzle_target_c = static_cast<float>(number_member(extruder, "target"));
  next.job.temperatures.bed_c = static_cast<float>(number_member(bed, "temperature"));
  next.job.temperatures.bed_target_c = static_cast<float>(number_member(bed, "target"));
  next.job.temperatures.nozzle_known = cJSON_IsNumber(member(extruder, "temperature"));
  next.job.temperatures.nozzle_target_known = cJSON_IsNumber(member(extruder, "target"));
  next.job.temperatures.bed_known = cJSON_IsNumber(member(bed, "temperature"));
  next.job.temperatures.bed_target_known = cJSON_IsNumber(member(bed, "target"));
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
  next.job.motion.x_known = cJSON_IsNumber(cJSON_GetArrayItem(position, 0));
  next.job.motion.y_known = cJSON_IsNumber(cJSON_GetArrayItem(position, 1));
  next.job.motion.z_known = cJSON_IsNumber(cJSON_GetArrayItem(position, 2));
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
  apply_part_fan(status, next.job);
  apply_snapmaker_activity(status, next.job, context.tool_objects);
  return result;
}

}  // namespace printdeck::platform
