#include "printdeck/platform/bambu_report_parser.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

bool read_number(const cJSON* object, const char* key, double& result) {
  const cJSON* value = member(object, key);
  if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble)) {
    result = value->valuedouble;
    return true;
  }
  if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
  char* end = nullptr;
  const double parsed = std::strtod(value->valuestring, &end);
  if (end == value->valuestring || end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
    return false;
  }
  result = parsed;
  return true;
}

bool read_text(const cJSON* object, const char* key, std::string& result) {
  const cJSON* value = member(object, key);
  if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
  result = value->valuestring;
  return true;
}

bool read_uint64_value(const cJSON* value, std::uint64_t& result) {
  constexpr double kMaximumExactJsonInteger = 9007199254740991.0;
  if (cJSON_IsNumber(value)) {
    const double number = value->valuedouble;
    if (!std::isfinite(number) || number < 0.0 || number > kMaximumExactJsonInteger ||
        std::floor(number) != number) {
      return false;
    }
    result = static_cast<std::uint64_t>(number);
    return true;
  }
  if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
  const char* text = value->valuestring;
  while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)) != 0) ++text;
  if (*text == '\0' || *text == '-') return false;
  const bool explicit_hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  bool hex_like = true;
  std::size_t digits = 0;
  for (const char* cursor = text + (explicit_hex ? 2 : 0); *cursor != '\0'; ++cursor) {
    if (std::isspace(static_cast<unsigned char>(*cursor)) != 0) break;
    if (std::isxdigit(static_cast<unsigned char>(*cursor)) == 0) {
      hex_like = false;
      break;
    }
    ++digits;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(
      text, &end, explicit_hex || (hex_like && digits >= 8) ? 16 : 10);
  if (end == text || errno == ERANGE) return false;
  while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)) != 0) ++end;
  if (*end != '\0' || parsed > std::numeric_limits<std::uint64_t>::max()) return false;
  result = static_cast<std::uint64_t>(parsed);
  return true;
}

bool read_uint64_field(const cJSON* object, std::initializer_list<const char*> keys,
                       std::uint64_t& result) {
  for (const char* key : keys) {
    if (read_uint64_value(member(object, key), result)) return true;
  }
  return false;
}

bool hms_code_from(const cJSON* item, std::uint64_t& result) {
  if (read_uint64_value(item, result) && result > 0xFFFFFFFFULL) return true;
  if (!cJSON_IsObject(item)) return false;
  if (read_uint64_field(item, {"ecode", "hms_code", "hmsCode", "full_code", "fullCode"},
                        result) && result > 0xFFFFFFFFULL) {
    return true;
  }
  std::uint64_t attribute = 0;
  std::uint64_t code = 0;
  if (read_uint64_field(item, {"attr", "hms_attr", "hmsAttr"}, attribute) &&
      read_uint64_field(item, {"code", "err_code", "errCode", "alarm_code", "alarmCode"},
                        code)) {
    result = ((attribute & 0xFFFFFFFFULL) << 32U) | (code & 0xFFFFFFFFULL);
    return result != 0;
  }
  return read_uint64_field(item, {"code", "err_code", "errCode", "alarm_code", "alarmCode"},
                           result) && result > 0xFFFFFFFFULL;
}

std::vector<std::uint64_t> hms_codes_from(const cJSON* hms) {
  std::vector<std::uint64_t> result;
  if (hms == nullptr) return result;
  const auto append = [&result](std::uint64_t code) {
    if (code == 0 || code == 0x050002000003000AULL) return;
    if (std::find(result.begin(), result.end(), code) == result.end()) result.push_back(code);
  };
  if (cJSON_IsArray(hms)) {
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, hms) {
      std::uint64_t code = 0;
      if (hms_code_from(item, code)) append(code);
    }
  } else {
    std::uint64_t code = 0;
    if (hms_code_from(hms, code)) {
      append(code);
    } else if (cJSON_IsObject(hms)) {
      for (const cJSON* item = hms->child; item != nullptr; item = item->next) {
        if (hms_code_from(item, code)) append(code);
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

core::JobPhase phase_for(const std::string& value) {
  if (value == "IDLE") return core::JobPhase::idle;
  if (value == "PREPARE" || value == "SLICING" || value == "INIT") {
    return core::JobPhase::preparing;
  }
  if (value == "RUNNING") return core::JobPhase::printing;
  if (value == "PAUSE" || value == "PAUSED") return core::JobPhase::paused;
  if (value == "FINISH") return core::JobPhase::completed;
  if (value == "FAILED" || value == "OFFLINE") return core::JobPhase::failed;
  return core::JobPhase::unknown;
}

core::PrinterActivity activity_for_stage(int stage) {
  switch (stage) {
    case 1: return core::PrinterActivity::bed_leveling;
    case 2: return core::PrinterActivity::bed_heating;
    case 3: return core::PrinterActivity::calibrating;
    case 4: return core::PrinterActivity::filament_changing;
    case 7: return core::PrinterActivity::nozzle_heating;
    case 8:
    case 18:
    case 19:
    case 25: return core::PrinterActivity::calibrating;
    case 9: return core::PrinterActivity::bed_leveling;
    case 10:
    case 11:
    case 12: return core::PrinterActivity::calibrating;
    case 13: return core::PrinterActivity::homing;
    case 14: return core::PrinterActivity::nozzle_cleaning;
    case 15: return core::PrinterActivity::nozzle_heating;
    case 22: return core::PrinterActivity::filament_unloading;
    case 24: return core::PrinterActivity::filament_loading;
    default: return core::PrinterActivity::unknown;
  }
}

bool filament_activity(core::PrinterActivity activity) {
  return activity == core::PrinterActivity::filament_changing ||
         activity == core::PrinterActivity::filament_unloading ||
         activity == core::PrinterActivity::filament_loading ||
         activity == core::PrinterActivity::filament_purging;
}

core::PrinterActivity activity_for_ams_status(int status) {
  const int main = (status >> 8) & 0xFF;
  const int sub = status & 0xFF;
  if (main != 1) return core::PrinterActivity::unknown;
  if (sub == 4) return core::PrinterActivity::filament_unloading;
  if (sub == 5 || sub == 6) return core::PrinterActivity::filament_loading;
  if (sub == 7) return core::PrinterActivity::filament_purging;
  return core::PrinterActivity::filament_changing;
}

std::string file_stem(std::string path) {
  const std::size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos) path.erase(0, slash + 1);
  constexpr std::string_view extension = ".gcode";
  if (path.size() >= extension.size() &&
      path.compare(path.size() - extension.size(), extension.size(), extension) == 0) {
    path.resize(path.size() - extension.size());
  }
  return path;
}

bool is_calibration_job(std::string_view name, std::string_view gcode_file) {
  constexpr std::string_view prefix = "auto_cali_for_";
  const auto matches = [prefix](std::string_view value) {
    const std::size_t slash = value.find_last_of("/\\");
    if (slash != std::string_view::npos) value.remove_prefix(slash + 1);
    return value.rfind(prefix, 0) == 0;
  };
  return matches(name) || matches(gcode_file);
}

std::uint32_t rgba(std::string text) {
  if (!text.empty() && text.front() == '#') text.erase(0, 1);
  if (text.size() != 6 && text.size() != 8) return 0;
  if (!std::all_of(text.begin(), text.end(), [](unsigned char value) {
        return std::isxdigit(value) != 0;
      })) return 0;
  const unsigned long value = std::strtoul(text.c_str(), nullptr, 16);
  return text.size() == 6 ? (static_cast<std::uint32_t>(value) << 8U) | 0xFFU
                           : static_cast<std::uint32_t>(value);
}

void merge_materials(const cJSON* print, core::MaterialSystem& materials) {
  const cJSON* ams = member(print, "ams");
  if (!cJSON_IsObject(ams)) return;
  double active_value = -1;
  read_number(ams, "tray_now", active_value);
  const int active = static_cast<int>(active_value);
  const cJSON* units = member(ams, "ams");
  if (cJSON_IsArray(units)) {
    std::vector<core::MaterialSlot> next_slots;
    const cJSON* unit = nullptr;
    int fallback_unit = 0;
    cJSON_ArrayForEach(unit, units) {
      double unit_value = fallback_unit++;
      read_number(unit, "id", unit_value);
      const int unit_id = static_cast<int>(unit_value);
      if (unit_id < 0 || unit_id > 3) continue;
      const cJSON* trays = member(unit, "tray");
      if (!cJSON_IsArray(trays)) continue;
      const cJSON* tray = nullptr;
      int fallback_tray = 0;
      cJSON_ArrayForEach(tray, trays) {
        double tray_value = fallback_tray++;
        read_number(tray, "id", tray_value);
        const int tray_id = static_cast<int>(tray_value);
        if (tray_id < 0 || tray_id > 3) continue;
        const std::size_t index = static_cast<std::size_t>(unit_id * 4 + tray_id);
        if (next_slots.size() <= index) next_slots.resize(index + 1);
        core::MaterialSlot& slot = next_slots[index];
        slot.installed = member(tray, "tray_type") != nullptr ||
                         member(tray, "tray_color") != nullptr;
        slot.feeding = slot.installed && static_cast<int>(index) == active;
        std::string text;
        if (read_text(tray, "tray_type", text)) slot.material = text;
        if (read_text(tray, "tray_color", text)) slot.rgba = rgba(text);
        double remaining = -1;
        if (read_number(tray, "remain", remaining)) {
          slot.remaining_percent = static_cast<int>(std::clamp(remaining, 0.0, 100.0));
        }
      }
    }
    materials.slots = std::move(next_slots);
  }
  const cJSON* external = member(print, "vt_tray");
  if (cJSON_IsObject(external)) {
    core::MaterialSlot slot;
    slot.installed = member(external, "tray_type") != nullptr ||
                     member(external, "tray_color") != nullptr;
    slot.feeding = slot.installed && active == 254;
    std::string text;
    if (read_text(external, "tray_type", text)) slot.material = text;
    if (read_text(external, "tray_color", text)) slot.rgba = rgba(text);
    double remaining = -1;
    if (read_number(external, "remain", remaining)) {
      slot.remaining_percent = static_cast<int>(std::clamp(remaining, 0.0, 100.0));
    }
    materials.external_spool = std::move(slot);
  } else if (cJSON_IsNull(external)) {
    materials.external_spool = {};
  }
}

}  // namespace

BambuReportParseResult parse_bambu_report(const char* payload, std::size_t length,
                                           const core::PrinterSnapshot& previous,
                                           std::uint32_t profile_id,
                                           std::uint64_t updated_at_ms) {
  BambuReportParseResult result;
  if (payload == nullptr || length == 0) return result;
  JsonDocument document(cJSON_ParseWithLength(payload, length));
  const cJSON* print = member(document.get(), "print");
  if (!document || !cJSON_IsObject(print)) return result;

  result.parsed = true;
  result.snapshot = previous;
  core::PrinterSnapshot& next = result.snapshot;
  next.profile_id = profile_id;
  next.link = core::LinkState::online;
  next.link_detail = "Connected";
  next.job.reachable = true;
  next.updated_at_ms = updated_at_ms;
  std::string text;
  if (read_text(print, "gcode_state", text)) {
    next.job.phase = phase_for(text);
    next.job.detail = core::phase_label(next.job.phase);
    if (next.job.phase == core::JobPhase::idle ||
        next.job.phase == core::JobPhase::completed ||
        next.job.phase == core::JobPhase::failed ||
        next.job.phase == core::JobPhase::cancelled) {
      next.job.activity = core::PrinterActivity::unknown;
    }
    if (next.job.phase == core::JobPhase::idle) {
      next.job.kind = core::JobKind::print;
      next.job.name.clear();
      next.job.gcode_file.clear();
      next.job.preview_hint.clear();
      next.job.preview_plate_hint.clear();
      next.job.preview.reset();
    }
  }
  if (read_text(print, "subtask_name", text)) next.job.name = text;
  if (read_text(print, "gcode_file", text)) next.job.gcode_file = text;
  if (read_text(print, "file", text)) next.job.preview_hint = text;
  if (read_text(print, "param", text)) next.job.preview_plate_hint = text;
  if (next.job.preview_plate_hint.empty()) next.job.preview_plate_hint = next.job.gcode_file;
  if (next.job.name.empty() && !next.job.gcode_file.empty()) {
    next.job.name = file_stem(next.job.gcode_file);
  }
  if (next.job.phase != core::JobPhase::idle) {
    next.job.kind = is_calibration_job(next.job.name, next.job.gcode_file)
                        ? core::JobKind::calibration
                        : core::JobKind::print;
  }

  double activity_value = 0;
  bool stage_reported = false;
  if (read_number(print, "stg_cur", activity_value)) {
    stage_reported = true;
    next.job.activity = activity_for_stage(static_cast<int>(activity_value));
  }
  if (read_number(print, "ams_status", activity_value)) {
    const core::PrinterActivity ams_activity =
        activity_for_ams_status(static_cast<int>(activity_value));
    if (ams_activity != core::PrinterActivity::unknown) {
      next.job.activity = ams_activity;
    } else if (!stage_reported && filament_activity(next.job.activity)) {
      next.job.activity = core::PrinterActivity::unknown;
    }
  }

  double value = 0;
  if (read_number(print, "mc_percent", value)) next.job.completion = static_cast<float>(value);
  if (read_number(print, "mc_remaining_time", value)) {
    next.job.remaining_seconds = static_cast<std::uint32_t>(std::max(0.0, value) * 60.0);
  }
  if (read_number(print, "layer_num", value)) {
    next.job.current_layer = static_cast<std::uint16_t>(std::clamp(value, 0.0, 65535.0));
  }
  if (read_number(print, "total_layer_num", value)) {
    next.job.total_layers = static_cast<std::uint16_t>(std::clamp(value, 0.0, 65535.0));
  }
  if (read_number(print, "nozzle_temper", value)) {
    next.job.temperatures.nozzle_c = static_cast<float>(value);
  }
  if (read_number(print, "nozzle_target_temper", value)) {
    next.job.temperatures.nozzle_target_c = static_cast<float>(value);
  }
  if (read_number(print, "bed_temper", value)) {
    next.job.temperatures.bed_c = static_cast<float>(value);
  }
  if (read_number(print, "bed_target_temper", value)) {
    next.job.temperatures.bed_target_c = static_cast<float>(value);
  }
  if (read_number(print, "chamber_temper", value)) {
    next.job.temperatures.chamber_c = static_cast<float>(value);
    next.job.temperatures.chamber_known = true;
  }
  if (read_number(print, "spd_mag", value)) {
    next.job.motion.speed_multiplier = static_cast<float>(value);
    next.job.motion.speed_multiplier_known = true;
  }
  if (read_number(print, "cooling_fan_speed", value)) {
    next.job.motion.fan_percent = static_cast<float>(value * (100.0 / 15.0));
    next.job.motion.fan_percent_known = true;
  }
  if (const cJSON* hms = member(print, "hms"); hms != nullptr) {
    next.job.hms_codes = hms_codes_from(hms);
  }
  const cJSON* lights = member(print, "lights_report");
  if (cJSON_IsArray(lights)) {
    const cJSON* light = nullptr;
    cJSON_ArrayForEach(light, lights) {
      std::string node;
      std::string mode;
      if (read_text(light, "node", node) && node == "chamber_light" &&
          read_text(light, "mode", mode)) {
        next.job.chamber_light_supported = true;
        next.job.chamber_light_on = mode == "on";
        if (next.job.chamber_light_pending &&
            next.job.chamber_light_on == next.job.chamber_light_target_on) {
          next.job.chamber_light_pending = false;
          result.chamber_light_confirmed = true;
        }
        break;
      }
    }
  }
  merge_materials(print, next.job.materials);
  return result;
}

}  // namespace printdeck::platform
