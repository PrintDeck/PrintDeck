#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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

// Resin printing has a layer cycle independent of FDM motion/tool telemetry.
enum class ResinStage : std::uint8_t {
  unknown, standby, homing, lowering, exposing, lifting, pausing, paused,
  stopping, stopped, completed, checking_file, transferring_file, exposure_test, device_test,
  finishing,
};

// A printer/service condition is independent of the outcome of a print job.
enum class PrinterCondition : std::uint8_t {
  normal, ready, busy, attention, error, unknown,
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
  bool nozzle_known = false;
  bool nozzle_target_known = false;
  bool bed_known = false;
  bool bed_target_known = false;
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
  bool x_known = false;
  bool y_known = false;
  bool z_known = false;
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
  bool target_known = false;
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

struct ResinLayerSettings {
  std::array<std::optional<float>, 2> lift_mm, retract_mm;
  std::array<std::optional<float>, 2> lift_mm_s, retract_mm_s;
  std::optional<float> exposure_s;
};

struct ResinTelemetry {
  std::optional<float> chamber_target_c, bottle_ml;
  std::optional<bool> feeder_enabled;
};

struct ResinPrintSettings {
  std::string profile_name;
  ResinLayerSettings bottom, normal;
  std::optional<float> layer_height_mm, volume_ml, weight_g;
  std::optional<std::uint16_t> bottom_layers, transition_layers;
};

// Executed layer duration, anchored to the observed exposure-state transition.
// The monotonic anchor belongs to the adapter, so changing pages cannot restart it.
struct ResinExposureTiming {
  std::uint64_t started_at_ms = 0;
  std::uint32_t duration_ms = 0;
};

inline std::uint32_t resin_exposure_remaining_tenths(const ResinExposureTiming& timing,
                                                    std::uint64_t now_ms) {
  const auto elapsed = now_ms > timing.started_at_ms ? now_ms - timing.started_at_ms : 0;
  return elapsed >= timing.duration_ms ? 0 :
      static_cast<std::uint32_t>((timing.duration_ms - elapsed + 99) / 100);
}

struct JobState {
  JobPhase phase = JobPhase::unknown;
  ResinStage resin_stage = ResinStage::unknown;
  ResinPrintSettings resin_settings;
  ResinTelemetry resin_telemetry;
  std::optional<ResinExposureTiming> resin_exposure;
  JobKind kind = JobKind::print;
  PrinterActivity activity = PrinterActivity::unknown;
  // Optional detail for the current activity; reaction IDs remain unchanged.
  enum class ActivityDetail : std::uint8_t {
    none, bed_detection, tool_check, flow_calibration, bed_preheat, bed_prescan,
  };
  ActivityDetail activity_detail = ActivityDetail::none;
  bool activity_status_available = false;
  int activity_toolhead = -1;
  std::uint16_t activity_current = 0;
  std::uint16_t activity_total = 0;
  int activity_remaining_seconds = -1;
  PrinterCondition condition = PrinterCondition::normal;
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
  bool completion_known = false;
  bool elapsed_known = false;
  bool remaining_known = false;
  std::uint16_t current_layer = 0;
  std::uint16_t total_layers = 0;
  std::shared_ptr<std::vector<std::uint8_t>> preview;
  // Transient exposure image; never stored in the model-thumbnail flash cache.
  std::shared_ptr<std::vector<std::uint8_t>> exposure_preview;
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
const char* resin_status_label(const JobState& job);
std::string job_name_for_display(std::string_view name);
PrinterActivity effective_printer_activity(const JobState& job);
const char* printer_activity_label(PrinterActivity activity);
bool display_wake_transition(JobPhase previous_phase, float previous_completion,
                             JobPhase phase, float completion);
bool animation_wake_transition(PrinterActivity previous_activity,
                               PrinterActivity activity);

}  // namespace printdeck::core
