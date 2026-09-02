#include "printdeck/core/unified_printer_api.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "printdeck/core/printer_driver.hpp"

namespace printdeck::core {
namespace {

void append_json_string(std::string& output, std::string_view value) {
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b"; break;
      case '\f': output += "\\f"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (character < 0x20U) {
          std::array<char, 7> escaped{};
          std::snprintf(escaped.data(), escaped.size(), "\\u%04x", character);
          output += escaped.data();
        } else {
          output.push_back(static_cast<char>(character));
        }
    }
  }
  output.push_back('"');
}

void append_float(std::string& output, float value) {
  if (!std::isfinite(value)) {
    output += "null";
    return;
  }
  std::array<char, 32> text{};
  std::snprintf(text.data(), text.size(), "%.2f", static_cast<double>(value));
  std::string_view view(text.data());
  while (view.size() > 1 && view.back() == '0') view.remove_suffix(1);
  if (!view.empty() && view.back() == '.') view.remove_suffix(1);
  output.append(view);
}

const char* link_id(LinkState state) {
  switch (state) {
    case LinkState::stopped: return "stopped";
    case LinkState::waiting_for_network: return "waiting_for_network";
    case LinkState::connecting: return "connecting";
    case LinkState::online: return "online";
    case LinkState::failed: return "offline";
  }
  return "unknown";
}

const char* reachability_id(PrinterReachability state) {
  switch (state) {
    case PrinterReachability::unknown: return "unknown";
    case PrinterReachability::online: return "online";
    case PrinterReachability::offline: return "offline";
  }
  return "unknown";
}

const char* detail_id(UnifiedApiDetailLevel level) {
  return level == UnifiedApiDetailLevel::full ? "full" : "summary";
}

const char* kind_id(JobKind kind) {
  return kind == JobKind::calibration ? "calibration" : "print";
}

const char* phase_id(JobPhase phase) {
  switch (phase) {
    case JobPhase::unknown: return "unknown";
    case JobPhase::idle: return "idle";
    case JobPhase::preparing: return "preparing";
    case JobPhase::printing: return "printing";
    case JobPhase::paused: return "paused";
    case JobPhase::completed: return "completed";
    case JobPhase::failed: return "failed";
    case JobPhase::cancelled: return "cancelled";
  }
  return "unknown";
}

const char* activity_id(PrinterActivity activity) {
  switch (activity) {
    case PrinterActivity::unknown: return "unknown";
    case PrinterActivity::standby: return "standby";
    case PrinterActivity::preparing: return "preparing";
    case PrinterActivity::nozzle_heating: return "nozzle_heating";
    case PrinterActivity::bed_heating: return "bed_heating";
    case PrinterActivity::homing: return "homing";
    case PrinterActivity::bed_leveling: return "bed_leveling";
    case PrinterActivity::nozzle_cleaning: return "nozzle_cleaning";
    case PrinterActivity::calibrating: return "calibrating";
    case PrinterActivity::filament_changing: return "filament_changing";
    case PrinterActivity::filament_unloading: return "filament_unloading";
    case PrinterActivity::filament_loading: return "filament_loading";
    case PrinterActivity::filament_purging: return "filament_purging";
    case PrinterActivity::printing: return "printing";
    case PrinterActivity::paused: return "paused";
    case PrinterActivity::completed: return "completed";
    case PrinterActivity::failed: return "failed";
    case PrinterActivity::cancelled: return "cancelled";
  }
  return "unknown";
}

void append_nullable_float(std::string& output, bool known, float value) {
  if (known) append_float(output, value);
  else output += "null";
}

void append_nullable_bool(std::string& output, bool known, bool value) {
  if (!known) output += "null";
  else output += value ? "true" : "false";
}

void append_connection(std::string& output, const UnifiedPrinterView& printer) {
  output += "{\"state\":\"";
  output += link_id(printer.snapshot.link);
  output += "\",\"reachability\":\"";
  output += reachability_id(printer.reachability);
  output += "\",\"detail_level\":\"";
  output += detail_id(printer.detail_level);
  output += "\",\"stale\":";
  output += printer.stale ? "true" : "false";
  output += ",\"updated_at_ms\":" + std::to_string(printer.snapshot.updated_at_ms) + "}";
}

void append_job(std::string& output, const JobState& job) {
  output += "{\"phase\":";
  append_json_string(output, phase_id(job.phase));
  output += ",\"kind\":\"";
  output += kind_id(job.kind);
  output += "\",\"activity\":\"";
  output += activity_id(effective_printer_activity(job));
  output += "\",\"name\":";
  if (job.name.empty()) output += "null";
  else append_json_string(output, job.name);
  output += ",\"progress_percent\":";
  append_float(output, std::clamp(job.completion, 0.0F, 100.0F));
  output += ",\"elapsed_seconds\":" + std::to_string(job.elapsed_seconds);
  output += ",\"remaining_seconds\":" + std::to_string(job.remaining_seconds);
  output += ",\"current_layer\":" + std::to_string(job.current_layer);
  output += ",\"total_layers\":" + std::to_string(job.total_layers) + "}";
}

void append_temperatures(std::string& output, const UnifiedPrinterView& printer) {
  const bool full = printer.detail_level == UnifiedApiDetailLevel::full;
  const Temperatures& temperatures = printer.snapshot.job.temperatures;
  output += "{\"nozzle_current_c\":";
  append_nullable_float(output, full, temperatures.nozzle_c);
  output += ",\"nozzle_target_c\":";
  append_nullable_float(output, full, temperatures.nozzle_target_c);
  output += ",\"bed_current_c\":";
  append_nullable_float(output, full, temperatures.bed_c);
  output += ",\"bed_target_c\":";
  append_nullable_float(output, full, temperatures.bed_target_c);
  output += ",\"chamber_current_c\":";
  append_nullable_float(output, full && temperatures.chamber_known, temperatures.chamber_c);
  output += "}";
}

void append_status_object(std::string& output, const UnifiedPrinterView& printer,
                          bool compact) {
  output += "{\"printer_id\":" + std::to_string(printer.id) + ",\"connection\":";
  append_connection(output, printer);
  output += ",\"job\":";
  append_job(output, printer.snapshot.job);
  if (!compact) {
    output += ",\"temperatures\":";
    append_temperatures(output, printer);
  }
  output += "}";
}

void append_printer_object(std::string& output, const UnifiedPrinterView& printer) {
  output += "{\"id\":" + std::to_string(printer.id) + ",\"name\":";
  append_json_string(output, printer.display_name);
  output += ",\"protocol\":";
  append_json_string(output, printer_driver(printer.protocol).id);
  output += ",\"manufacturer\":";
  if (printer.manufacturer.empty()) output += "null";
  else append_json_string(output, printer.manufacturer);
  output += ",\"model\":";
  if (printer.model.empty()) output += "null";
  else append_json_string(output, printer.model);
  output += ",\"endpoint\":";
  append_json_string(output, printer.endpoint);
  output += ",\"selected\":";
  output += printer.selected ? "true" : "false";
  output += ",\"reachability\":\"";
  output += reachability_id(printer.reachability);
  output += "\",\"capabilities\":{\"status\":true,\"nozzles\":true,\"materials\":true}}";
}

void append_color(std::string& output, std::uint32_t rgba) {
  if (rgba == 0) {
    output += "null";
    return;
  }
  std::array<char, 12> text{};
  std::snprintf(text.data(), text.size(), "#%08lX", static_cast<unsigned long>(rgba));
  append_json_string(output, text.data());
}

void append_slot(std::string& output, const MaterialSlot& slot, std::size_t index,
                 bool external) {
  output += "{\"id\":";
  append_json_string(output, external ? "external-" + std::to_string(index)
                                      : "slot-" + std::to_string(index));
  output += ",\"installed\":";
  output += slot.installed ? "true" : "false";
  output += ",\"feeding\":";
  output += slot.feeding ? "true" : "false";
  output += ",\"material\":";
  if (slot.material.empty()) output += "null";
  else append_json_string(output, slot.material);
  output += ",\"color\":";
  append_color(output, slot.rgba);
  output += ",\"remaining_percent\":";
  if (slot.remaining_percent < 0) output += "null";
  else output += std::to_string(std::clamp(slot.remaining_percent, 0, 100));
  output += ",\"source_unit\":";
  if (slot.source_unit < 0) output += "null";
  else output += std::to_string(slot.source_unit);
  output += ",\"source_slot\":";
  if (slot.source_slot < 0) output += "null";
  else output += std::to_string(slot.source_slot);
  output += "}";
}

}  // namespace

std::string unified_api_printers_json(std::span<const UnifiedPrinterView> printers) {
  std::string output = "{\"api_version\":\"v1\",\"printers\":[";
  bool first = true;
  for (const UnifiedPrinterView& printer : printers) {
    if (!first) output.push_back(',');
    first = false;
    append_printer_object(output, printer);
  }
  output += "]}";
  return output;
}

std::string unified_api_statuses_json(std::span<const UnifiedPrinterView> printers) {
  std::string output = "{\"api_version\":\"v1\",\"statuses\":[";
  bool first = true;
  for (const UnifiedPrinterView& printer : printers) {
    if (!first) output.push_back(',');
    first = false;
    append_status_object(output, printer, true);
  }
  output += "]}";
  return output;
}

std::string unified_api_snapshot_json(std::span<const UnifiedPrinterView> printers) {
  std::string output = "{\"api_version\":\"v1\",\"printers\":[";
  bool first = true;
  for (const UnifiedPrinterView& printer : printers) {
    if (!first) output.push_back(',');
    first = false;
    output += "{\"printer\":";
    append_printer_object(output, printer);
    output += ",\"status\":";
    append_status_object(output, printer, false);
    output += "}";
  }
  output += "]}";
  return output;
}

std::string unified_api_printer_json(const UnifiedPrinterView& printer) {
  std::string output = "{\"api_version\":\"v1\",\"printer\":";
  append_printer_object(output, printer);
  output += "}";
  return output;
}

std::string unified_api_status_json(const UnifiedPrinterView& printer) {
  std::string output = "{\"api_version\":\"v1\",\"status\":";
  append_status_object(output, printer, false);
  output += "}";
  return output;
}

std::string unified_api_nozzles_json(const UnifiedPrinterView& printer) {
  std::string output = "{\"api_version\":\"v1\",\"printer_id\":" +
      std::to_string(printer.id) + ",\"detail_level\":\"" +
      detail_id(printer.detail_level) + "\",\"stale\":" +
      (printer.stale ? "true" : "false") + ",\"updated_at_ms\":" +
      std::to_string(printer.snapshot.updated_at_ms) + ",\"nozzles\":[";
  const JobState& job = printer.snapshot.job;
  bool first = true;
  const std::size_t count = std::min<std::size_t>(job.toolhead_count, job.toolheads.size());
  for (std::size_t index = 0; index < count; ++index) {
    const ToolheadState& tool = job.toolheads[index];
    if (!tool.present) continue;
    if (!first) output.push_back(',');
    first = false;
    output += "{\"id\":\"T" + std::to_string(index) + "\",\"active\":";
    output += tool.active ? "true" : "false";
    output += ",\"state\":";
    if (tool.state.empty()) output += "null";
    else append_json_string(output, tool.state);
    output += ",\"diameter_mm\":";
    append_nullable_float(output, tool.nozzle_diameter_mm > 0.0F, tool.nozzle_diameter_mm);
    output += ",\"temperature\":{\"current_c\":";
    append_nullable_float(output, tool.temperature_known, tool.temperature_c);
    output += ",\"target_c\":";
    append_nullable_float(output, tool.temperature_known, tool.target_c);
    output += "},\"material\":{\"type\":";
    if (tool.material.empty()) output += "null";
    else append_json_string(output, tool.material);
    output += ",\"color\":";
    append_color(output, tool.material_rgba);
    output += "},\"filament_detected\":";
    append_nullable_bool(output, tool.filament_state_known, tool.filament_detected);
    output += "}";
  }
  if (first && printer.detail_level == UnifiedApiDetailLevel::full) {
    output += "{\"id\":\"T0\",\"active\":true,\"state\":null,\"diameter_mm\":null,"
              "\"temperature\":{\"current_c\":";
    append_float(output, job.temperatures.nozzle_c);
    output += ",\"target_c\":";
    append_float(output, job.temperatures.nozzle_target_c);
    output += "},\"material\":{\"type\":null,\"color\":null},\"filament_detected\":null}";
  }
  output += "]}";
  return output;
}

std::string unified_api_materials_json(const UnifiedPrinterView& printer) {
  const MaterialSystem& materials = printer.snapshot.job.materials;
  const bool available = printer.detail_level == UnifiedApiDetailLevel::full &&
      (!materials.slots.empty() || !materials.external_spools.empty());
  std::string output = "{\"api_version\":\"v1\",\"printer_id\":" +
      std::to_string(printer.id) + ",\"available\":" +
      (available ? "true" : "false") + ",\"detail_level\":\"" +
      detail_id(printer.detail_level) + "\",\"stale\":" +
      (printer.stale ? "true" : "false") + ",\"updated_at_ms\":" +
      std::to_string(printer.snapshot.updated_at_ms) + ",\"system\":";
  if (!available) output += "null";
  else append_json_string(output, printer.protocol == PrinterProtocol::bambu_lan
                                      ? "ams_or_ams_lite" : "material_system");
  output += ",\"slots\":[";
  bool first = true;
  for (std::size_t index = 0; index < materials.slots.size(); ++index) {
    if (!first) output.push_back(',');
    first = false;
    append_slot(output, materials.slots[index], index, false);
  }
  output += "],\"external_spools\":[";
  first = true;
  for (std::size_t index = 0; index < materials.external_spools.size(); ++index) {
    if (!first) output.push_back(',');
    first = false;
    append_slot(output, materials.external_spools[index], index, true);
  }
  output += "]}";
  return output;
}

}  // namespace printdeck::core
