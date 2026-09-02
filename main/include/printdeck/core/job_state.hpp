#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace printdeck::core {

enum class JobPhase : std::uint8_t {
  unknown,
  idle,
  preparing,
  printing,
  paused,
  completed,
  failed,
  cancelled,
};

enum class JobKind : std::uint8_t {
  print,
  calibration,
};

enum class PrinterActivity : std::uint8_t {
  unknown,
  standby,
  preparing,
  nozzle_heating,
  bed_heating,
  homing,
  bed_leveling,
  nozzle_cleaning,
  calibrating,
  filament_changing,
  filament_unloading,
  filament_loading,
  filament_purging,
  printing,
  paused,
  completed,
  failed,
  cancelled,
};

struct Temperatures {
  float nozzle_c = 0.0F;
  float nozzle_target_c = 0.0F;
  float bed_c = 0.0F;
  float bed_target_c = 0.0F;
  float chamber_c = 0.0F;
  bool chamber_known = false;
};

struct MotionMetrics {
  float velocity_mm_s = 0.0F;
  float speed_multiplier = 0.0F;
  float extrusion_multiplier = 0.0F;
  float fan_percent = 0.0F;
  float x_mm = 0.0F;
  float y_mm = 0.0F;
  float z_mm = 0.0F;
  bool velocity_known = false;
  bool speed_multiplier_known = false;
  bool extrusion_multiplier_known = false;
  bool fan_percent_known = false;
  bool position_known = false;
  std::string homed_axes;
};

struct MaterialSlot {
  bool installed = false;
  bool feeding = false;
  int source_unit = -1;
  int source_slot = -1;
  std::string material;
  std::uint32_t rgba = 0;
  int remaining_percent = -1;
};

struct MaterialSystem {
  std::vector<MaterialSlot> slots;
  std::vector<MaterialSlot> external_spools;
  // Compatibility view used by the current material screen. It mirrors the
  // feeding external slot, or the first installed external slot.
  MaterialSlot external_spool;
};

inline constexpr std::size_t kMaximumToolheads = 12;

struct ToolheadState {
  bool present = false;
  bool active = false;
  bool temperature_known = false;
  float temperature_c = 0.0F;
  float target_c = 0.0F;
  float heater_power = 0.0F;
  bool heater_power_known = false;
  float nozzle_diameter_mm = 0.0F;
  std::string state;
  std::string material;
  std::uint32_t material_rgba = 0;
  bool filament_state_known = false;
  bool filament_detected = false;
};

struct JobState {
  JobPhase phase = JobPhase::unknown;
  JobKind kind = JobKind::print;
  PrinterActivity activity = PrinterActivity::unknown;
  std::string name;
  std::string gcode_file;
  std::string preview_hint;
  std::string preview_plate_hint;
  std::string detail;
  Temperatures temperatures;
  MotionMetrics motion;
  float bed_heater_power = 0.0F;
  bool bed_heater_power_known = false;
  MaterialSystem materials;
  std::array<ToolheadState, kMaximumToolheads> toolheads{};
  std::uint8_t toolhead_count = 0;
  int active_toolhead = -1;
  std::vector<std::uint64_t> hms_codes;
  float completion = 0.0F;
  std::uint32_t elapsed_seconds = 0;
  std::uint32_t remaining_seconds = 0;
  std::uint16_t current_layer = 0;
  std::uint16_t total_layers = 0;
  std::shared_ptr<std::vector<std::uint8_t>> preview;
  std::shared_ptr<std::vector<std::uint8_t>> camera_frame;
  std::uint16_t camera_width = 0;
  std::uint16_t camera_height = 0;
  bool camera_supported = false;
  bool camera_live_supported = false;
  bool camera_refreshing = false;
  std::string camera_detail;
  bool chamber_light_supported = false;
  bool chamber_light_on = false;
  bool chamber_light_pending = false;
  bool chamber_light_target_on = false;
  bool reachable = false;

  void normalize();
};

const char* phase_label(JobPhase phase);
const char* job_status_label(const JobState& job);
std::string job_name_for_display(std::string_view name);
PrinterActivity effective_printer_activity(const JobState& job);
const char* printer_activity_label(PrinterActivity activity);
bool display_wake_transition(JobPhase previous_phase, float previous_completion,
                             JobPhase phase, float completion);
bool animation_wake_transition(PrinterActivity previous_activity,
                               PrinterActivity activity);

}  // namespace printdeck::core
