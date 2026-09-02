#include "printdeck/platform/bambu_report_parser.hpp"

#include <algorithm>
#include <array>
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

bool read_uint64_value(const cJSON* value, std::uint64_t& result,
                       bool prefer_hex_for_long_text = false) {
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
  bool contains_hex_alpha = false;
  std::size_t digits = 0;
  for (const char* cursor = text + (explicit_hex ? 2 : 0); *cursor != '\0'; ++cursor) {
    if (std::isspace(static_cast<unsigned char>(*cursor)) != 0) break;
    if (std::isxdigit(static_cast<unsigned char>(*cursor)) == 0) {
      hex_like = false;
      break;
    }
    contains_hex_alpha = contains_hex_alpha ||
                         std::isalpha(static_cast<unsigned char>(*cursor)) != 0;
    ++digits;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(
      text, &end,
      explicit_hex || contains_hex_alpha ||
              (prefer_hex_for_long_text && hex_like && digits >= 8)
          ? 16
          : 10);
  if (end == text || errno == ERANGE) return false;
  while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)) != 0) ++end;
  if (*end != '\0' || parsed > std::numeric_limits<std::uint64_t>::max()) return false;
  result = static_cast<std::uint64_t>(parsed);
  return true;
}

bool read_uint64_field(const cJSON* object, std::initializer_list<const char*> keys,
                       std::uint64_t& result) {
  for (const char* key : keys) {
    if (read_uint64_value(member(object, key), result, true)) return true;
  }
  return false;
}

bool hms_code_from(const cJSON* item, std::uint64_t& result) {
  if (read_uint64_value(item, result, true) && result > 0xFFFFFFFFULL) return true;
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
  if (value == "CANCEL" || value == "CANCELLED") return core::JobPhase::cancelled;
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

bool valid_temperature(double value) {
  return std::isfinite(value) && value >= -50.0 && value <= 500.0;
}

struct TemperaturePair {
  bool current_known = false;
  bool target_known = false;
  float current = 0.0F;
  float target = 0.0F;
};

TemperaturePair temperature_pair_from(const cJSON* value) {
  TemperaturePair result;
  if (cJSON_IsObject(value)) {
    double number = 0.0;
    for (const char* key : {"current", "cur", "temperature", "temper"}) {
      if (read_number(value, key, number) && valid_temperature(number)) {
        result.current_known = true;
        result.current = static_cast<float>(number);
        break;
      }
    }
    for (const char* key : {"target", "tar", "target_temperature", "target_temper"}) {
      if (read_number(value, key, number) && valid_temperature(number)) {
        result.target_known = true;
        result.target = static_cast<float>(number);
        break;
      }
    }
    return result;
  }

  if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
      value->valuedouble >= -50.0 && value->valuedouble <= 500.0) {
    result.current_known = true;
    result.current = static_cast<float>(value->valuedouble);
    return result;
  }

  std::uint64_t raw = 0;
  if (!read_uint64_value(value, raw) || raw > 0xFFFFFFFFULL) return result;
  if (raw <= 1000U) {
    if (valid_temperature(static_cast<double>(raw))) {
      result.current_known = true;
      result.current = static_cast<float>(raw);
    }
    return result;
  }
  const std::uint16_t current_raw = static_cast<std::uint16_t>(raw & 0xFFFFU);
  const std::uint16_t target_raw = static_cast<std::uint16_t>((raw >> 16U) & 0xFFFFU);
  if (current_raw != 0xFFFFU) {
    const double current = static_cast<std::int16_t>(current_raw);
    if (valid_temperature(current)) {
      result.current_known = true;
      result.current = static_cast<float>(current);
    }
  }
  if (target_raw != 0xFFFFU) {
    const double target = static_cast<std::int16_t>(target_raw);
    if (valid_temperature(target)) {
      result.target_known = true;
      result.target = static_cast<float>(target);
    }
  }
  return result;
}

core::MaterialSlot material_slot_from(const cJSON* source, int unit, int slot_id) {
  core::MaterialSlot slot;
  slot.source_unit = unit;
  slot.source_slot = slot_id;
  slot.installed = member(source, "tray_type") != nullptr ||
                   member(source, "tray_color") != nullptr ||
                   member(source, "type") != nullptr ||
                   member(source, "color") != nullptr;
  std::string text;
  if (read_text(source, "tray_type", text) || read_text(source, "type", text)) {
    slot.material = text;
  }
  if (read_text(source, "tray_color", text) || read_text(source, "color", text)) {
    slot.rgba = rgba(text);
  }
  double remaining = -1.0;
  if (read_number(source, "remain", remaining) ||
      read_number(source, "remaining", remaining)) {
    slot.remaining_percent = static_cast<int>(std::clamp(remaining, 0.0, 100.0));
  }
  return slot;
}

bool active_tray_matches(int active, int unit, int tray) {
  if (active < 0) return false;
  if (unit >= 0 && unit < 64 && tray >= 0 && tray < 4 && active == unit * 4 + tray) {
    return true;
  }
  return active == ((unit & 0xFF) << 8 | (tray & 0xFF));
}

void select_external_compatibility_slot(core::MaterialSystem& materials) {
  materials.external_spool = {};
  for (const auto& slot : materials.external_spools) {
    if (slot.installed && slot.feeding) {
      materials.external_spool = slot;
      return;
    }
  }
  for (const auto& slot : materials.external_spools) {
    if (slot.installed) {
      materials.external_spool = slot;
      return;
    }
  }
}

void merge_materials(const cJSON* print, core::MaterialSystem& materials) {
  const cJSON* ams = member(print, "ams");
  double active_value = -1;
  if (cJSON_IsObject(ams)) read_number(ams, "tray_now", active_value);
  const int active = static_cast<int>(active_value);
  const cJSON* units = member(ams, "ams");
  if (cJSON_IsArray(units)) {
    std::vector<core::MaterialSlot> next_slots;
    next_slots.reserve(std::min(cJSON_GetArraySize(units) * 4, 64));
    const cJSON* unit = nullptr;
    int fallback_unit = 0;
    cJSON_ArrayForEach(unit, units) {
      double unit_value = fallback_unit++;
      read_number(unit, "id", unit_value);
      const int unit_id = static_cast<int>(unit_value);
      if (unit_id < 0 || unit_id > 255) continue;
      const cJSON* trays = member(unit, "tray");
      if (!cJSON_IsArray(trays)) continue;
      const cJSON* tray = nullptr;
      int fallback_tray = 0;
      cJSON_ArrayForEach(tray, trays) {
        double tray_value = fallback_tray++;
        read_number(tray, "id", tray_value);
        const int tray_id = static_cast<int>(tray_value);
        if (tray_id < 0 || tray_id > 15 || next_slots.size() >= 64) continue;
        core::MaterialSlot slot = material_slot_from(tray, unit_id, tray_id);
        slot.feeding = slot.installed && active_tray_matches(active, unit_id, tray_id);
        next_slots.push_back(std::move(slot));
      }
    }
    materials.slots = std::move(next_slots);
  } else if (cJSON_IsNull(units)) {
    materials.slots.clear();
  }
  const cJSON* external = member(print, "vt_tray");
  if (cJSON_IsObject(external)) {
    core::MaterialSlot slot = material_slot_from(external, 255, 0);
    slot.feeding = slot.installed && active == 254;
    materials.external_spools.assign(1, std::move(slot));
  } else if (cJSON_IsNull(external)) {
    materials.external_spools.clear();
  }

  const cJSON* device = member(print, "device");
  const cJSON* virtual_slots = member(print, "vir_slot");
  if (!cJSON_IsArray(virtual_slots)) virtual_slots = member(device, "vir_slot");
  if (!cJSON_IsArray(virtual_slots)) {
    virtual_slots = member(member(device, "ams"), "vir_slot");
  }
  if (cJSON_IsArray(virtual_slots)) {
    std::vector<core::MaterialSlot> next_external;
    next_external.reserve(std::min(cJSON_GetArraySize(virtual_slots), 16));
    const cJSON* virtual_slot = nullptr;
    int fallback_slot = 0;
    cJSON_ArrayForEach(virtual_slot, virtual_slots) {
      if (next_external.size() >= 16) break;
      double id_value = fallback_slot++;
      read_number(virtual_slot, "id", id_value);
      const int id = static_cast<int>(id_value);
      if (id < 0 || id > 255) continue;
      core::MaterialSlot slot = material_slot_from(virtual_slot, 255, id);
      slot.feeding = slot.installed && (active == 254 || active == id);
      next_external.push_back(std::move(slot));
    }
    materials.external_spools = std::move(next_external);
  } else if (cJSON_IsNull(virtual_slots)) {
    materials.external_spools.clear();
  }
  select_external_compatibility_slot(materials);
}

struct MaterialSelection {
  int unit = -1;
  int slot = -1;
};

void apply_material_selections(const std::vector<MaterialSelection>& selections,
                               core::JobState& job) {
  if (selections.empty()) return;
  for (auto& slot : job.materials.slots) slot.feeding = false;
  for (auto& slot : job.materials.external_spools) slot.feeding = false;
  for (const MaterialSelection& selection : selections) {
    for (auto& slot : job.materials.slots) {
      if (slot.installed && slot.source_unit == selection.unit &&
          slot.source_slot == selection.slot) {
        slot.feeding = true;
      }
    }
    for (auto& slot : job.materials.external_spools) {
      if (slot.installed &&
          (selection.unit == 255 || selection.unit == slot.source_unit) &&
          (selection.slot == slot.source_slot || selection.slot == 254)) {
        slot.feeding = true;
      }
    }
  }
  select_external_compatibility_slot(job.materials);

  for (std::size_t index = 0; index < job.toolhead_count; ++index) {
    auto& tool = job.toolheads[index];
    if (!tool.active) continue;
    for (const auto& slot : job.materials.slots) {
      if (!slot.feeding) continue;
      tool.material = slot.material;
      tool.material_rgba = slot.rgba;
      tool.filament_state_known = true;
      tool.filament_detected = true;
      break;
    }
    for (const auto& slot : job.materials.external_spools) {
      if (!slot.feeding) continue;
      tool.material = slot.material;
      tool.material_rgba = slot.rgba;
      tool.filament_state_known = true;
      tool.filament_detected = true;
      break;
    }
  }
}

void merge_v2_device(const cJSON* print, core::JobState& job,
                     bool legacy_nozzle_current, bool legacy_nozzle_target,
                     bool legacy_bed_current, bool legacy_bed_target,
                     bool legacy_chamber, std::vector<MaterialSelection>& selections) {
  const cJSON* device = member(print, "device");
  if (!cJSON_IsObject(device)) return;

  const auto component_temperature = [device](const char* component) {
    const cJSON* object = member(device, component);
    const cJSON* info = member(object, "info");
    const cJSON* temperature = member(info, "temp");
    if (temperature == nullptr) temperature = member(object, "temp");
    return temperature_pair_from(temperature);
  };

  TemperaturePair bed = component_temperature("bed");
  if (!bed.current_known && !bed.target_known) {
    bed = temperature_pair_from(member(device, "bed_temp"));
  }
  if (!legacy_bed_current && bed.current_known) job.temperatures.bed_c = bed.current;
  if (!legacy_bed_target && bed.target_known) job.temperatures.bed_target_c = bed.target;

  TemperaturePair chamber = component_temperature("ctc");
  if (!chamber.current_known && !chamber.target_known) {
    chamber = component_temperature("chamber");
  }
  if (!legacy_chamber && chamber.current_known) {
    job.temperatures.chamber_c = chamber.current;
    job.temperatures.chamber_known = true;
  }

  const cJSON* extruder = member(device, "extruder");
  const cJSON* extruder_info = member(extruder, "info");
  std::array<bool, core::kMaximumToolheads> target_known{};
  std::uint64_t extruder_state = 0;
  const bool state_known = read_uint64_value(member(extruder, "state"), extruder_state);
  int stated_count = state_known ? static_cast<int>(extruder_state & 0x0FU) : 0;
  int active = state_known ? static_cast<int>((extruder_state >> 4U) & 0x0FU) : -1;
  if (stated_count < 0 || stated_count > static_cast<int>(core::kMaximumToolheads)) {
    stated_count = 0;
  }
  int observed_count = 0;
  if (cJSON_IsArray(extruder_info)) {
    const cJSON* item = nullptr;
    int fallback_id = 0;
    cJSON_ArrayForEach(item, extruder_info) {
      double id_value = fallback_id++;
      read_number(item, "id", id_value);
      const int id = static_cast<int>(id_value);
      // H2C Vortek rack metadata uses IDs 16-21. Those are stored hotends,
      // not simultaneously live extruders, and must not become UI toolheads.
      if (id < 0 || id >= static_cast<int>(core::kMaximumToolheads)) continue;
      observed_count = std::max(observed_count, id + 1);
      core::ToolheadState& tool = job.toolheads[static_cast<std::size_t>(id)];
      tool.present = true;
      const cJSON* temp_value = member(item, "temp");
      if (temp_value == nullptr) temp_value = member(member(item, "info"), "temp");
      const TemperaturePair temperature = temperature_pair_from(temp_value);
      if (temperature.current_known) {
        tool.temperature_known = true;
        tool.temperature_c = temperature.current;
      }
      if (temperature.target_known) {
        tool.target_c = temperature.target;
        target_known[static_cast<std::size_t>(id)] = true;
      }

      std::uint64_t snow = 0;
      if (read_uint64_value(member(item, "snow"), snow) && snow <= 0xFFFFU &&
          snow != 0xFFFFU && snow != 0x00FFU) {
        const MaterialSelection selection{
            static_cast<int>((snow >> 8U) & 0xFFU),
            static_cast<int>(snow & 0xFFU)};
        selections.push_back(selection);
      }
    }
  }

  const int count = std::max(stated_count, observed_count);
  if (count > 0) {
    job.toolhead_count = static_cast<std::uint8_t>(count);
    for (int index = 0; index < count; ++index) {
      job.toolheads[static_cast<std::size_t>(index)].present = true;
    }
    for (std::size_t index = static_cast<std::size_t>(count);
         index < job.toolheads.size(); ++index) {
      job.toolheads[index] = {};
    }
  }
  if (active < 0 || active >= job.toolhead_count) active = job.toolhead_count > 0 ? 0 : -1;
  job.active_toolhead = active;
  for (std::size_t index = 0; index < job.toolhead_count; ++index) {
    job.toolheads[index].active = static_cast<int>(index) == active;
  }

  const cJSON* nozzle_info = member(member(device, "nozzle"), "info");
  if (cJSON_IsArray(nozzle_info)) {
    const cJSON* item = nullptr;
    int fallback_id = 0;
    cJSON_ArrayForEach(item, nozzle_info) {
      double id_value = fallback_id++;
      read_number(item, "id", id_value);
      const int id = static_cast<int>(id_value);
      if (id < 0 || id >= job.toolhead_count) continue;
      double diameter = 0.0;
      if ((read_number(item, "diameter", diameter) ||
           read_number(item, "nozzle_diameter", diameter)) &&
          diameter >= 0.1 && diameter <= 2.0) {
        job.toolheads[static_cast<std::size_t>(id)].nozzle_diameter_mm =
            static_cast<float>(diameter);
      }
    }
  }

  if (job.toolhead_count > 0) {
    const int primary = job.active_toolhead >= 0 ? job.active_toolhead : 0;
    const core::ToolheadState& tool = job.toolheads[static_cast<std::size_t>(primary)];
    if (!legacy_nozzle_current && tool.temperature_known) {
      job.temperatures.nozzle_c = tool.temperature_c;
    }
    if (!legacy_nozzle_target && target_known[static_cast<std::size_t>(primary)]) {
      job.temperatures.nozzle_target_c = tool.target_c;
    }
  }
}

void parse_identity(const cJSON* document, BambuReportParseResult& result) {
  const cJSON* info = member(document, "info");
  if (!cJSON_IsObject(info)) return;
  std::string direct;
  if (read_text(info, "product_name", direct) && !direct.empty()) {
    result.product_name = direct;
  }
  const cJSON* modules = member(info, "module");
  if (cJSON_IsArray(modules)) {
    const cJSON* module = nullptr;
    cJSON_ArrayForEach(module, modules) {
      std::string product;
      if (!read_text(module, "product_name", product) || product.empty()) continue;
      std::string name;
      read_text(module, "name", name);
      if (result.product_name.empty() || name == "ota" || name == "mc") {
        result.product_name = std::move(product);
      }
      if (name == "ota" || name == "mc") break;
    }
  }
  result.identity_report = !result.product_name.empty() || member(info, "module") != nullptr;
}

}  // namespace

BambuReportParseResult parse_bambu_report(const char* payload, std::size_t length,
                                           const core::PrinterSnapshot& previous,
                                           std::uint32_t profile_id,
                                           std::uint64_t updated_at_ms) {
  BambuReportParseResult result;
  if (payload == nullptr || length == 0) return result;
  JsonDocument document(cJSON_ParseWithLength(payload, length));
  if (!document || !cJSON_IsObject(document.get())) return result;
  parse_identity(document.get(), result);
  const cJSON* print = member(document.get(), "print");
  if (!cJSON_IsObject(print)) {
    result.parsed = result.identity_report;
    result.snapshot = previous;
    return result;
  }

  result.parsed = true;
  const bool primary_status = member(print, "gcode_state") != nullptr ||
                              member(print, "mc_percent") != nullptr ||
                              member(print, "mc_remaining_time") != nullptr ||
                              member(print, "nozzle_temper") != nullptr ||
                              member(print, "bed_temper") != nullptr ||
                              member(print, "device") != nullptr;
  const bool delta_status = member(print, "ams") != nullptr ||
                            member(print, "hms") != nullptr ||
                            member(print, "lights_report") != nullptr ||
                            member(print, "stg_cur") != nullptr;
  result.status_report = primary_status ||
                         (previous.link == core::LinkState::online && delta_status);
  std::uint64_t feature_flags = 0;
  if (read_uint64_value(member(print, "fun"), feature_flags, true)) {
    result.restricted_commands = (feature_flags & 0x20000000ULL) != 0;
  }
  result.snapshot = previous;
  core::PrinterSnapshot& next = result.snapshot;
  next.profile_id = profile_id;
  if (result.status_report) {
    next.link = core::LinkState::online;
    next.link_detail = "Connected";
    next.job.reachable = true;
    next.updated_at_ms = updated_at_ms;
  }
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
  const bool legacy_nozzle_current = read_number(print, "nozzle_temper", value) &&
                                     valid_temperature(value);
  if (legacy_nozzle_current) {
    next.job.temperatures.nozzle_c = static_cast<float>(value);
  }
  const bool legacy_nozzle_target = read_number(print, "nozzle_target_temper", value) &&
                                    valid_temperature(value);
  if (legacy_nozzle_target) {
    next.job.temperatures.nozzle_target_c = static_cast<float>(value);
  }
  const bool legacy_bed_current = read_number(print, "bed_temper", value) &&
                                  valid_temperature(value);
  if (legacy_bed_current) {
    next.job.temperatures.bed_c = static_cast<float>(value);
  }
  const bool legacy_bed_target = read_number(print, "bed_target_temper", value) &&
                                 valid_temperature(value);
  if (legacy_bed_target) {
    next.job.temperatures.bed_target_c = static_cast<float>(value);
  }
  const bool legacy_chamber = read_number(print, "chamber_temper", value) &&
                              valid_temperature(value);
  if (legacy_chamber) {
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
  std::vector<MaterialSelection> material_selections;
  merge_v2_device(print, next.job, legacy_nozzle_current, legacy_nozzle_target,
                  legacy_bed_current, legacy_bed_target, legacy_chamber,
                  material_selections);
  apply_material_selections(material_selections, next.job);

  if ((legacy_nozzle_current || legacy_nozzle_target) && next.job.toolhead_count == 0) {
    next.job.toolhead_count = 1;
    next.job.active_toolhead = 0;
    next.job.toolheads[0].present = true;
    next.job.toolheads[0].active = true;
  }
  if (next.job.active_toolhead >= 0 &&
      next.job.active_toolhead < next.job.toolhead_count) {
    core::ToolheadState& active_tool =
        next.job.toolheads[static_cast<std::size_t>(next.job.active_toolhead)];
    if (legacy_nozzle_current) {
      active_tool.temperature_known = true;
      active_tool.temperature_c = next.job.temperatures.nozzle_c;
    }
    if (legacy_nozzle_target) active_tool.target_c = next.job.temperatures.nozzle_target_c;
  }
  return result;
}

}  // namespace printdeck::platform
