#include "printdeck/core/job_state.hpp"

#include <algorithm>
#include <cmath>

namespace printdeck::core {
namespace {

float finite_or_zero(float value) {
  return std::isfinite(value) ? value : 0.0F;
}

bool decode_utf8(std::string_view text, std::size_t offset,
                 std::uint32_t& codepoint, std::size_t& length) {
  const auto first = static_cast<std::uint8_t>(text[offset]);
  if (first < 0x80U) {
    codepoint = first;
    length = 1;
    return true;
  }
  if ((first & 0xe0U) == 0xc0U) length = 2;
  else if ((first & 0xf0U) == 0xe0U) length = 3;
  else if ((first & 0xf8U) == 0xf0U) length = 4;
  else return false;
  if (offset + length > text.size()) return false;

  codepoint = first & (0x7fU >> length);
  for (std::size_t index = 1; index < length; ++index) {
    const auto continuation = static_cast<std::uint8_t>(text[offset + index]);
    if ((continuation & 0xc0U) != 0x80U) return false;
    codepoint = (codepoint << 6U) | (continuation & 0x3fU);
  }
  const std::uint32_t minimum = length == 2 ? 0x80U : length == 3 ? 0x800U : 0x10000U;
  return codepoint >= minimum && codepoint <= 0x10ffffU &&
         !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
}

bool is_emoji_codepoint(std::uint32_t codepoint) {
  return (codepoint >= 0x1f000U && codepoint <= 0x1faffU) ||
         (codepoint >= 0x1fc00U && codepoint <= 0x1ffffU);
}

}  // namespace

void JobState::normalize() {
  completion = std::clamp(finite_or_zero(completion), 0.0F, 100.0F);
  temperatures.nozzle_c = finite_or_zero(temperatures.nozzle_c);
  temperatures.nozzle_target_c = finite_or_zero(temperatures.nozzle_target_c);
  temperatures.bed_c = finite_or_zero(temperatures.bed_c);
  temperatures.bed_target_c = finite_or_zero(temperatures.bed_target_c);
  temperatures.chamber_c = finite_or_zero(temperatures.chamber_c);
  motion.velocity_mm_s = finite_or_zero(motion.velocity_mm_s);
  motion.speed_multiplier = finite_or_zero(motion.speed_multiplier);
  motion.extrusion_multiplier = finite_or_zero(motion.extrusion_multiplier);
  motion.fan_percent = std::clamp(finite_or_zero(motion.fan_percent), 0.0F, 100.0F);
  motion.x_mm = finite_or_zero(motion.x_mm);
  motion.y_mm = finite_or_zero(motion.y_mm);
  motion.z_mm = finite_or_zero(motion.z_mm);
  bed_heater_power = std::clamp(finite_or_zero(bed_heater_power), 0.0F, 1.0F);

  toolhead_count = std::min<std::uint8_t>(toolhead_count, kMaximumToolheads);
  if (active_toolhead < 0 || active_toolhead >= toolhead_count) active_toolhead = -1;
  for (auto& toolhead : toolheads) {
    toolhead.temperature_c = finite_or_zero(toolhead.temperature_c);
    toolhead.target_c = finite_or_zero(toolhead.target_c);
    toolhead.heater_power = std::clamp(finite_or_zero(toolhead.heater_power), 0.0F, 1.0F);
    toolhead.nozzle_diameter_mm = finite_or_zero(toolhead.nozzle_diameter_mm);
  }

  for (auto& slot : materials.slots) {
    slot.remaining_percent = std::clamp(slot.remaining_percent, -1, 100);
  }
  for (auto& slot : materials.external_spools) {
    slot.remaining_percent = std::clamp(slot.remaining_percent, -1, 100);
  }
  materials.external_spool.remaining_percent =
      std::clamp(materials.external_spool.remaining_percent, -1, 100);

  if (!reachable) {
    phase = JobPhase::unknown;
    activity = PrinterActivity::unknown;
    detail = "Printer unavailable";
    return;
  }

  if (phase == JobPhase::idle) {
    kind = JobKind::print;
    activity = PrinterActivity::unknown;
    completion = 0.0F;
    elapsed_seconds = 0;
    remaining_seconds = 0;
    current_layer = 0;
    total_layers = 0;
  }
}

const char* phase_label(JobPhase phase) {
  switch (phase) {
    case JobPhase::idle: return "Standby";
    case JobPhase::preparing: return "Preparing";
    case JobPhase::printing: return "Printing";
    case JobPhase::paused: return "Paused";
    case JobPhase::completed: return "Complete";
    case JobPhase::failed: return "Failed";
    case JobPhase::cancelled: return "Cancelled";
    case JobPhase::unknown: return "Unavailable";
  }
  return "Unavailable";
}

const char* job_status_label(const JobState& job) {
  const bool active = job.phase == JobPhase::preparing ||
                      job.phase == JobPhase::printing;
  return active && job.kind == JobKind::calibration ? "Calibration"
                                                     : phase_label(job.phase);
}

std::string job_name_for_display(std::string_view name) {
  std::string display;
  display.reserve(name.size());
  bool replaced_emoji = false;
  for (std::size_t offset = 0; offset < name.size();) {
    std::uint32_t codepoint = 0;
    std::size_t length = 1;
    if (!decode_utf8(name, offset, codepoint, length)) {
      display.push_back('?');
      ++offset;
      replaced_emoji = false;
      continue;
    }
    if (codepoint == 0x200dU || codepoint == 0x20e3U ||
        (codepoint >= 0xfe00U && codepoint <= 0xfe0fU) ||
        (codepoint >= 0xe0100U && codepoint <= 0xe01efU)) {
      offset += length;
      continue;
    }
    if (is_emoji_codepoint(codepoint)) {
      if (!replaced_emoji) display.push_back('-');
      replaced_emoji = true;
      offset += length;
      continue;
    }
    display.append(name.substr(offset, length));
    replaced_emoji = false;
    offset += length;
  }
  return display;
}

PrinterActivity effective_printer_activity(const JobState& job) {
  if (job.activity != PrinterActivity::unknown) return job.activity;
  switch (job.phase) {
    case JobPhase::idle: return PrinterActivity::standby;
    case JobPhase::preparing:
      if (job.temperatures.bed_target_c > job.temperatures.bed_c + 2.0F) {
        return PrinterActivity::bed_heating;
      }
      if (job.temperatures.nozzle_target_c > job.temperatures.nozzle_c + 2.0F) {
        return PrinterActivity::nozzle_heating;
      }
      return job.kind == JobKind::calibration ? PrinterActivity::calibrating
                                               : PrinterActivity::preparing;
    case JobPhase::printing: return PrinterActivity::printing;
    case JobPhase::paused: return PrinterActivity::paused;
    case JobPhase::completed: return PrinterActivity::completed;
    case JobPhase::failed: return PrinterActivity::failed;
    case JobPhase::cancelled: return PrinterActivity::cancelled;
    case JobPhase::unknown: return PrinterActivity::unknown;
  }
  return PrinterActivity::unknown;
}

const char* printer_activity_label(PrinterActivity activity) {
  switch (activity) {
    case PrinterActivity::standby: return "Standby";
    case PrinterActivity::preparing: return "Preparing";
    case PrinterActivity::nozzle_heating: return "Heating nozzle";
    case PrinterActivity::bed_heating: return "Heating bed";
    case PrinterActivity::homing: return "Homing toolhead";
    case PrinterActivity::bed_leveling: return "Leveling the bed";
    case PrinterActivity::nozzle_cleaning: return "Cleaning nozzle";
    case PrinterActivity::calibrating: return "Calibrating";
    case PrinterActivity::filament_changing: return "Changing filament";
    case PrinterActivity::filament_unloading: return "Unloading filament";
    case PrinterActivity::filament_loading: return "Loading filament";
    case PrinterActivity::filament_purging: return "Purging filament";
    case PrinterActivity::printing: return "Printing";
    case PrinterActivity::paused: return "Paused";
    case PrinterActivity::completed: return "Complete";
    case PrinterActivity::failed: return "Failed";
    case PrinterActivity::cancelled: return "Cancelled";
    case PrinterActivity::unknown: return "Unavailable";
  }
  return "Unavailable";
}

bool display_wake_transition(JobPhase previous_phase, float previous_completion,
                             JobPhase phase, float completion) {
  const bool print_started =
      (phase == JobPhase::preparing || phase == JobPhase::printing ||
       phase == JobPhase::paused) &&
      (previous_phase == JobPhase::idle || previous_phase == JobPhase::completed ||
       previous_phase == JobPhase::failed || previous_phase == JobPhase::cancelled);
  const bool completed = phase == JobPhase::completed &&
                         previous_phase != JobPhase::completed;
  const bool reached_full_progress = completion >= 99.95F &&
                                     previous_completion < 99.95F;
  return print_started || completed || reached_full_progress;
}

bool animation_wake_transition(PrinterActivity previous_activity,
                               PrinterActivity activity) {
  // The first valid report only establishes the current state. Standby and
  // unavailable reports are deliberately quiet, avoiding a second wake after
  // a completed/failed animation and avoiding wakeups on network recovery.
  if (previous_activity == PrinterActivity::unknown ||
      activity == PrinterActivity::unknown ||
      activity == PrinterActivity::standby) {
    return false;
  }
  return activity != previous_activity;
}

}  // namespace printdeck::core
