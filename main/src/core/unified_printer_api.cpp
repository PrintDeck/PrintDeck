#include "printdeck/core/unified_printer_api.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "printdeck/core/printer_driver.hpp"
#include "printdeck/core/job_name.hpp"
#include "printdeck/core/settings.hpp"
#include "printdeck/core/printer_web_details.hpp"

namespace printdeck::core {
namespace {

const char* resin_stage_id(ResinStage stage) {
  switch (stage) {
    case ResinStage::unknown: return "unknown";
    case ResinStage::standby: return "standby";
    case ResinStage::homing: return "homing";
    case ResinStage::lowering: return "lowering";
    case ResinStage::exposing: return "exposing";
    case ResinStage::lifting: return "lifting";
    case ResinStage::pausing: return "pausing";
    case ResinStage::paused: return "paused";
    case ResinStage::stopping: return "stopping";
    case ResinStage::stopped: return "stopped";
    case ResinStage::completed: return "completed";
    case ResinStage::checking_file: return "checking_file";
    case ResinStage::transferring_file: return "transferring_file";
    case ResinStage::exposure_test: return "exposure_test";
    case ResinStage::device_test: return "device_test";
    case ResinStage::finishing: return "finishing";
  }
  return "unknown";
}

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
  output += ",\"condition\":";
  const char* condition = "unknown";
  switch (job.condition) {
    case PrinterCondition::normal: condition = "normal"; break;
    case PrinterCondition::ready: condition = "ready"; break;
    case PrinterCondition::busy: condition = "busy"; break;
    case PrinterCondition::attention: condition = "attention"; break;
    case PrinterCondition::error: condition = "error"; break;
    case PrinterCondition::unknown: break;
  }
  append_json_string(output, condition);
  output += ",\"kind\":\"";
  output += kind_id(job.kind);
  output += "\",\"activity\":\"";
  output += activity_id(effective_printer_activity(job));
  output += "\",\"name\":";
  if (job.name.empty()) output += "null";
  else append_json_string(output, bounded_job_name(job.name));
  output += ",\"progress_percent\":";
  append_nullable_float(output, job.completion_known, std::clamp(job.completion, 0.0F, 100.0F));
  output += ",\"elapsed_seconds\":" + (job.elapsed_known ? std::to_string(job.elapsed_seconds) : "null");
  output += ",\"remaining_seconds\":" + (job.remaining_known ? std::to_string(job.remaining_seconds) : "null");
  output += ",\"current_layer\":" + std::to_string(job.current_layer);
  output += ",\"total_layers\":" + std::to_string(job.total_layers) + "}";
}

void append_temperatures(std::string& output, const UnifiedPrinterView& printer) {
  const bool full = printer.detail_level == UnifiedApiDetailLevel::full;
  const Temperatures& temperatures = printer.snapshot.job.temperatures;
  output += "{\"nozzle_current_c\":";
  append_nullable_float(output, full && temperatures.nozzle_known, temperatures.nozzle_c);
  output += ",\"nozzle_target_c\":";
  append_nullable_float(output, full && temperatures.nozzle_target_known, temperatures.nozzle_target_c);
  output += ",\"bed_current_c\":";
  append_nullable_float(output, full && temperatures.bed_known, temperatures.bed_c);
  output += ",\"bed_target_c\":";
  append_nullable_float(output, full && temperatures.bed_target_known, temperatures.bed_target_c);
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
  const auto& history = printer.print_events;
  if (history.stream) {
    output += ",\"print_events\":{\"stream_id\":\"" + print_stream_id(history.stream) +
        "\",\"sequence\":" + std::to_string(history.sequence) + ",\"job_id\":";
    if (history.session) append_json_string(output, print_job_id(history.stream, history.session));
    else output += "null";
    output += ",\"observed_at_ms\":" + std::to_string(printer.observed_at_ms) + ",\"events\":[";
    for (std::size_t i = 0; i < history.count; ++i) {
      if (i) output += ',';
      output += unified_api_print_event_json(history, history.events[i]);
    }
    output += "]}";
  }
  if (!compact) {
    output += ",\"temperatures\":";
    append_temperatures(output, printer);
  }
  output += "}";
}

void append_printer_network(std::string& output, const UnifiedPrinterView& printer) {
  if (!is_local_printer_endpoint(printer.endpoint, printer.protocol)) {
    output += "{\"address\":null,\"port\":null}";
    return;
  }
  std::string_view address = printer.endpoint;
  unsigned port = printer_driver(printer.protocol).default_port;
  if (address.starts_with("https://")) {
    address.remove_prefix(8);
    port = 443;
  } else if (address.starts_with("http://")) {
    address.remove_prefix(7);
    port = 80;
  }
  const auto colon = address.find(':');
  if (colon != std::string_view::npos) {
    const auto port_text = address.substr(colon + 1);
    // Profile validation has already checked the complete port and its range.
    std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    address = address.substr(0, colon);
  }
  output += "{\"address\":";
  append_json_string(output, address);
  output += ",\"port\":" + (port == 0 ? std::string("null") : std::to_string(port)) + "}";
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
  output += ",\"network\":";
  append_printer_network(output, printer);
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

std::string unified_api_print_event_json(const PrintEventHistory& history, const PrintEvent& event) {
  std::string output = "{\"event_type\":\"" + std::string(print_event_type_id(event.type)) +
      "\",\"stream_id\":\"" + print_stream_id(history.stream) + "\",\"sequence\":" +
      std::to_string(event.sequence) + ",\"job_id\":";
  if (event.session) append_json_string(output, print_job_id(history.stream, event.session));
  else output += "null";
  output += ",\"observed_at_ms\":" + std::to_string(event.observed_at_ms) +
      ",\"job_kind\":\"" + kind_id(event.kind) + "\",\"progress_percent\":";
  append_nullable_float(output, event.progress_known, event.progress);
  output += ",\"milestone\":" + std::to_string(event.milestone);
  const char* condition = "unknown";
  switch (event.condition) {
    case PrinterCondition::normal: condition = "normal"; break;
    case PrinterCondition::ready: condition = "ready"; break;
    case PrinterCondition::busy: condition = "busy"; break;
    case PrinterCondition::attention: condition = "attention"; break;
    case PrinterCondition::error: condition = "error"; break;
    case PrinterCondition::unknown: break;
  }
  output += ",\"condition\":\"" + std::string(condition) + "\"}";
  return output;
}

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

std::string unified_api_snapshot_json(std::span<const UnifiedPrinterView> printers,
                                      const UnifiedDevicePower& power) {
  std::string output = "{\"api_version\":\"v1\",\"device\":{\"power\":{\"available\":";
  output += power.available ? "true" : "false";
  output += ",\"battery_present\":";
  output += power.battery_present ? "true" : "false";
  output += ",\"battery_percent\":";
  if (power.available && power.battery_present) {
    output += std::to_string(std::min<std::uint8_t>(power.battery_percent, 100));
  } else {
    output += "null";
  }
  output += ",\"charging\":";
  output += power.charging ? "true" : "false";
  output += ",\"external_power\":";
  output += power.external_power ? "true" : "false";
  output += "}},\"printers\":[";
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
    append_nullable_float(output, tool.target_known, tool.target_c);
    output += "},\"material\":{\"type\":";
    if (tool.material.empty()) output += "null";
    else append_json_string(output, tool.material);
    output += ",\"color\":";
    append_color(output, tool.material_rgba);
    output += "},\"filament_detected\":";
    append_nullable_bool(output, tool.filament_state_known, tool.filament_detected);
    output += "}";
  }
  if (first && printer.detail_level == UnifiedApiDetailLevel::full && job.active_toolhead >= 0) {
    output += "{\"id\":\"T0\",\"active\":true,\"state\":null,\"diameter_mm\":null,"
              "\"temperature\":{\"current_c\":";
    append_nullable_float(output, job.temperatures.nozzle_known, job.temperatures.nozzle_c);
    output += ",\"target_c\":";
    append_nullable_float(output, job.temperatures.nozzle_target_known, job.temperatures.nozzle_target_c);
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

std::string printer_display_address(const UnifiedPrinterView& printer) {
  if (!is_local_printer_endpoint(printer.endpoint, printer.protocol)) return {};
  std::string_view address = printer.endpoint;
  if (address.starts_with("https://")) address.remove_prefix(8);
  else if (address.starts_with("http://")) address.remove_prefix(7);
  return std::string(address.substr(0, address.find(':')));
}

bool printer_light_available(const UnifiedPrinterView& printer) {
  return printer.selected && !printer.stale &&
      printer.detail_level == UnifiedApiDetailLevel::full &&
      printer.reachability == PrinterReachability::online &&
      printer.snapshot.link == LinkState::online &&
      printer.snapshot.job.chamber_light_supported && !printer.snapshot.job.chamber_light_pending;
}

std::string printer_state_json(const UnifiedPrinterView& p, std::uint64_t now_ms,
                              std::int64_t now_unix, const PrinterStateMedia& media) {
  const auto& job = p.snapshot.job;
  const auto& driver = printer_driver(p.protocol);
  const bool full = p.detail_level == UnifiedApiDetailLevel::full;
  const bool online = p.reachability == PrinterReachability::online;
  const bool has_job = job.phase != JobPhase::idle && job.phase != JobPhase::unknown;
  std::string out = "{\"id\":" + std::to_string(p.id);
  const auto string = [&](const char* key, std::string_view value) {
    out += ",\""; out += key; out += "\":"; web_detail::string(out, value, 192);
  };
  string("name", p.display_name); string("protocol", driver.id);
  string("technology", driver.resin ? "resin" : "fdm");
  string("manufacturer", p.manufacturer); string("model", p.model); string("brand", p.brand);
  string("endpoint", p.endpoint);
  out += ",\"selected\":"; out += p.selected ? "true" : "false";
  out += ",\"dashboard_available\":"; out += driver.dashboard ? "true" : "false";
  out += ",\"printer_control_enabled\":"; out += p.printer_control_enabled ? "true" : "false";
  string("reachability", reachability_id(p.reachability));
  out += ",\"connection\":"; append_connection(out, p);
  out += ",\"capabilities\":{\"light.set\":{\"supported\":";
  out += full && job.chamber_light_supported ? "true" : "false";
  out += ",\"available\":"; out += printer_light_available(p) ? "true" : "false";
  out += "},\"printer.select\":{\"supported\":";
  out += driver.dashboard ? "true" : "false";
  out += ",\"available\":";
  out += driver.dashboard && p.reachability != PrinterReachability::offline ? "true" : "false";
  out += "},\"print.pause\":{\"supported\":false,\"available\":false},"
         "\"print.resume\":{\"supported\":false,\"available\":false},"
         "\"print.stop\":{\"supported\":false,\"available\":false},"
         "\"speed.set\":{\"supported\":false,\"available\":false},"
         "\"camera.view\":{\"supported\":false,\"available\":false}}";
  out += ",\"light\":{\"supported\":";
  out += full && job.chamber_light_supported ? "true" : "false";
  out += ",\"on\":"; out += full && online && job.chamber_light_supported ? (job.chamber_light_on ? "true" : "false") : "null";
  out += ",\"pending\":"; out += full && job.chamber_light_pending ? "true" : "false";
  out += ",\"target_on\":"; out += full && job.chamber_light_pending ? (job.chamber_light_target_on ? "true" : "false") : "null";
  out += "},\"job\":";
  if (!online || job.phase == JobPhase::unknown) out += "null";
  else {
    out += "{\"phase\":"; append_json_string(out, phase_id(job.phase));
    out += ",\"condition\":";
    const char* condition = "unknown";
    switch (job.condition) {
      case PrinterCondition::normal: condition = "normal"; break;
      case PrinterCondition::ready: condition = "ready"; break;
      case PrinterCondition::busy: condition = "busy"; break;
      case PrinterCondition::attention: condition = "attention"; break;
      case PrinterCondition::error: condition = "error"; break;
      case PrinterCondition::unknown: break;
    }
    append_json_string(out, condition);
    out += ",\"activity\":";
    if (full && p.selected && !p.stale && p.snapshot.link == LinkState::online)
      append_json_string(out, activity_id(effective_printer_activity(job)));
    else out += "null";
    out += ",\"progress\":";
    append_nullable_float(out, has_job && job.completion_known, std::clamp(job.completion, 0.0F, 100.0F));
    out += ",\"name\":";
    if (has_job && !job.name.empty()) web_detail::string(out, bounded_job_name(job.name), 192); else out += "null";
    out += ",\"elapsed_seconds\":" + (has_job && job.elapsed_known ? std::to_string(job.elapsed_seconds) : "null");
    out += ",\"remaining_seconds\":" + (has_job && job.remaining_known ? std::to_string(job.remaining_seconds) : "null");
    out += ",\"current_layer\":" + (has_job && job.current_layer > 0 ? std::to_string(job.current_layer) : "null");
    out += ",\"total_layers\":" + (has_job && job.total_layers > 0 ? std::to_string(job.total_layers) : "null");
    out += ",\"estimated_finish_unix\":";
    const auto updated = p.snapshot.updated_at_ms;
    const auto age_s = now_ms >= updated ? (now_ms - updated) / 1000 : 0;
    if (!p.stale && (job.phase == JobPhase::printing || job.phase == JobPhase::preparing) &&
        job.remaining_known && updated > 0 && now_ms >= updated && now_unix >= 1'577'836'800 && age_s <= job.remaining_seconds)
      out += std::to_string(now_unix + job.remaining_seconds - static_cast<std::int64_t>(age_s));
    else out += "null";
    if (driver.resin) {
      out += ",\"resin_stage\":"; append_json_string(out, resin_stage_id(job.resin_stage));
      const bool countdown = !p.stale &&
          (job.phase == JobPhase::printing || job.phase == JobPhase::preparing) &&
          (job.resin_stage == ResinStage::exposing || job.resin_stage == ResinStage::exposure_test) &&
          job.resin_exposure && resin_exposure_countdown_visible(*job.resin_exposure, updated, now_ms);
      out += ",\"exposure_remaining_ms\":";
      out += countdown ? std::to_string(resin_exposure_remaining_tenths(*job.resin_exposure, now_ms) * 100) : "null";
      out += ",\"exposure_valid_for_ms\":" + std::to_string(countdown ?
          (job.resin_exposure->retain_until_stage_change ? 2500 : 2500 - (now_ms - updated)) : 0);
    }
    out += ",\"details\":"; out += full ? printer_web_details_json(job, driver.resin) : "null";
    out += ",\"preview\":{\"model\":";
    if (!media.model.empty()) web_detail::string(out, media.model); else out += "null";
    out += ",\"layer\":";
    if (!media.layer.empty()) web_detail::string(out, media.layer); else out += "null";
    out += ",\"layer_valid_for_ms\":" + std::to_string(media.layer_valid_for_ms) + "}}";
  }
  out += ",\"materials\":" + unified_api_materials_json(p);
  out += '}';
  return out;
}

std::string printer_states_json(std::span<const UnifiedPrinterView> printers,
                               std::uint64_t now_ms, std::int64_t now_unix) {
  std::string out = "{\"schema_version\":1,\"printers\":[";
  bool first = true;
  for (const auto& printer : printers) {
    if (!first) out += ',';
    first = false;
    out += printer_state_json(printer, now_ms, now_unix);
  }
  return out + "]}";
}

}  // namespace printdeck::core
