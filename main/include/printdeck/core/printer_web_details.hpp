#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include "printdeck/core/job_state.hpp"

namespace printdeck::core {
namespace web_detail {
inline void string(std::string& out, std::string_view value, std::size_t limit = 160) {
  if (value.size() > limit) {
    while (limit && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80) --limit;
    value = value.substr(0, limit);
  }
  out += '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 32) { char escaped[7]; std::snprintf(escaped, sizeof(escaped), "\\u%04x", c); out += escaped; }
    else out += static_cast<char>(c);
  }
  out += '"';
}
inline void number(std::string& out, bool known, float value) {
  if (!known || !std::isfinite(value)) { out += "null"; return; }
  char text[48]; std::snprintf(text, sizeof(text), "%.3f", static_cast<double>(value)); out += text;
}
inline void number(std::string& out, std::optional<float> value) {
  number(out, value.has_value(), value.value_or(0));
}
} // namespace web_detail

// A bounded, read-only view of the selected printer's existing telemetry.
inline std::string printer_web_details_json(const JobState& job, bool resin) {
  using web_detail::number;
  std::string out = "{\"temperatures\":{\"nozzle_c\":";
  const auto& t = job.temperatures;
  number(out, !resin && t.nozzle_known, t.nozzle_c);
  out += ",\"nozzle_target_c\":"; number(out, !resin && t.nozzle_target_known, t.nozzle_target_c);
  out += ",\"bed_c\":"; number(out, !resin && t.bed_known, t.bed_c);
  out += ",\"bed_target_c\":"; number(out, !resin && t.bed_target_known, t.bed_target_c);
  out += ",\"chamber_c\":"; number(out, t.chamber_known, t.chamber_c);
  out += ",\"chamber_target_c\":"; number(out, resin ? job.resin_telemetry.chamber_target_c : std::nullopt);
  out += "}";
  if (resin) {
    const auto& s = job.resin_settings;
    const auto& r = job.resin_telemetry;
    out += ",\"resin\":{\"profile_name\":";
    web_detail::string(out, s.profile_name);
    out += ",\"layer_height_mm\":"; number(out, s.layer_height_mm);
    out += ",\"bottom_layers\":"; out += s.bottom_layers ? std::to_string(*s.bottom_layers) : "null";
    out += ",\"transition_layers\":"; out += s.transition_layers ? std::to_string(*s.transition_layers) : "null";
    out += ",\"volume_ml\":"; number(out, s.volume_ml);
    out += ",\"weight_g\":"; number(out, s.weight_g);
    out += ",\"bottle_ml\":"; number(out, r.bottle_ml);
    out += ",\"feeder_enabled\":"; out += r.feeder_enabled ? (*r.feeder_enabled ? "true" : "false") : "null";
    const auto layer = [&](const char* key, const ResinLayerSettings& settings) {
      out += ",\""; out += key; out += "\":{";
      bool first = true;
      const auto pair = [&](const char* name, const auto& values) {
        if (!first) out += ',';
        first = false;
        out += "\""; out += name; out += "\":[";
        number(out, values[0]); out += ','; number(out, values[1]); out += ']';
      };
      pair("lift_mm", settings.lift_mm); pair("retract_mm", settings.retract_mm);
      pair("lift_mm_s", settings.lift_mm_s); pair("retract_mm_s", settings.retract_mm_s);
      out += '}';
    };
    layer("bottom", s.bottom); layer("normal", s.normal);
    out += "}";
  } else {
    out += ",\"nozzles\":[";
    bool first = true;
    const auto append_tool = [&](const ToolheadState& tool, std::size_t index) {
      if (!first) out += ',';
      first = false;
      out += "{\"id\":" + std::to_string(index) + ",\"active\":" + (tool.active ? "true" : "false");
      out += ",\"current_c\":"; number(out, tool.temperature_known, tool.temperature_c);
      out += ",\"target_c\":"; number(out, tool.target_known, tool.target_c);
      out += ",\"diameter_mm\":"; number(out, tool.nozzle_diameter_mm > 0, tool.nozzle_diameter_mm);
      out += ",\"material\":"; web_detail::string(out, tool.material, 48);
      out += ",\"color\":";
      if (tool.material_rgba) { char color[8]; std::snprintf(color, sizeof(color), "#%06x", static_cast<unsigned>(tool.material_rgba >> 8)); web_detail::string(out, color); }
      else out += "null";
      out += ",\"filament_detected\":"; out += tool.filament_state_known ? (tool.filament_detected ? "true" : "false") : "null";
      out += '}';
    };
    for (std::size_t i = 0; i < std::min<std::size_t>(job.toolhead_count, job.toolheads.size()); ++i)
      if (job.toolheads[i].present) append_tool(job.toolheads[i], i);
    if (first && (t.nozzle_known || t.nozzle_target_known)) {
      ToolheadState fallback;
      fallback.active = true; fallback.temperature_known = t.nozzle_known;
      fallback.target_known = t.nozzle_target_known;
      fallback.temperature_c = t.nozzle_c; fallback.target_c = t.nozzle_target_c;
      append_tool(fallback, 0);
    }
    out += ']';
    const auto& m = job.motion;
    out += ",\"motion\":{\"speed_percent\":"; number(out, m.speed_multiplier_known, m.speed_multiplier);
    out += ",\"flow_percent\":"; number(out, m.extrusion_multiplier_known, m.extrusion_multiplier);
    out += ",\"fan_percent\":"; number(out, m.fan_percent_known, m.fan_percent);
    const auto active = job.active_toolhead >= 0 ? static_cast<std::size_t>(job.active_toolhead) : 0;
    const bool nozzle_power_known = active < std::min<std::size_t>(job.toolhead_count, job.toolheads.size()) &&
        job.toolheads[active].present && job.toolheads[active].heater_power_known;
    out += ",\"nozzle_power_percent\":";
    number(out, nozzle_power_known, nozzle_power_known ? job.toolheads[active].heater_power * 100 : 0);
    out += ",\"bed_power_percent\":"; number(out, job.bed_heater_power_known, job.bed_heater_power * 100);
    out += ",\"x_mm\":"; number(out, m.position_known || m.x_known, m.x_mm);
    out += ",\"y_mm\":"; number(out, m.position_known || m.y_known, m.y_mm);
    out += ",\"z_mm\":"; number(out, m.position_known || m.z_known, m.z_mm);
    out += '}';
  }
  out += '}';
  return out;
}
} // namespace printdeck::core
