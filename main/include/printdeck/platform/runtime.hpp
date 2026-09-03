#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include "printdeck/core/device_state.hpp"
#include "printdeck/platform/display_shell.hpp"
#include "printdeck/platform/settings_store.hpp"
#include "printdeck/platform/network_service.hpp"
#include "printdeck/platform/web_config.hpp"
#include "printdeck/platform/moonraker_adapter.hpp"
#include "printdeck/platform/moonraker_connection_probe.hpp"
#include "printdeck/platform/moonraker_camera_client.hpp"
#include "printdeck/platform/inactive_printer_poller.hpp"
#include "printdeck/platform/printer_discovery_service.hpp"
#include "printdeck/platform/firmware_update_service.hpp"
#include "printdeck/platform/reaction_asset_service.hpp"
#include "printdeck/platform/bambu_lan_adapter.hpp"
#include "printdeck/platform/bambu_a1_preview_client.hpp"
#include "printdeck/platform/bambu_a1_camera_client.hpp"
#include "printdeck/platform/bambu_compatibility_probe.hpp"
#include "printdeck/platform/orientation_service.hpp"
#include "printdeck/platform/audio_service.hpp"
#include "printdeck/platform/power_service.hpp"
#include "printdeck/platform/usb_developer_service.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace printdeck::platform {

class Runtime {
 public:
  Runtime();
  void start();

 private:
  static void monitor_entry(void* context);
  static void brightness_changed_entry(void* context, int percent);
  static void audio_changed_entry(void* context, bool enabled, int volume_percent);
  static void audio_preset_changed_entry(void* context, const char* preset);
  static void theme_changed_entry(void* context, const char* theme);
  static void language_changed_entry(void* context, const char* language);
  static void printer_animations_changed_entry(void* context, bool enabled);
  static bool printer_selected_entry(void* context, std::uint32_t profile_id);
  static void navigation_feedback_entry(void* context);
  static void rotation_feedback_entry(void* context, int degrees);
  static void page_refresh_entry(void* context);
  static void chamber_light_changed_entry(void* context, bool enabled);
  static void camera_mode_changed_entry(void* context, bool live);
  static void update_check_entry(void* context);
  static void update_install_entry(void* context);
  static bool selected_printer_snapshot_entry(
      void* context, core::PrinterSnapshot& destination);
  static void unified_api_activity_entry(void* context);
  static void printer_controls_activity_entry(void* context);
  static bool printer_light_entry(void* context, std::uint32_t profile_id, bool enabled);
  static bool background_update_blocked_entry(void* context);
  static void settings_changed_entry(void* context, const core::DeviceSettings& settings,
                                     bool play_feedback);
  static bool audio_test_entry(void* context, std::string_view preset,
                               std::string_view event, int volume_percent);
  static void configuration_backup_activity_entry(
      void* context, core::ConfigurationBackupActivity activity, bool play_feedback);
  static bool restart_requested_entry(void* context);
  static void restart_audio_finished_entry(void* context);
  static void power_entry(void* context);
  static void ui_settings_entry(void* context);
  void monitor_loop();
  void power_loop();
  void ui_settings_loop();
  void apply_pending_settings();
  void apply_settings(const core::DeviceSettings& settings, bool play_feedback);
  void apply_pending_printer_selection();
  bool ensure_moonraker_started(const core::PrinterProfile* selected);
  bool ensure_bambu_lan_started(const core::PrinterProfile* selected);
  bool ensure_moonraker_camera_started();
  bool ensure_bambu_preview_started();
  bool ensure_bambu_camera_started();
  [[noreturn]] void perform_shutdown();
  core::PrinterSnapshot update_bambu_snapshot();
  void prime_audio_state(const core::PrinterSnapshot& snapshot);
  void update_audio_state(const core::PrinterSnapshot& snapshot);
  bool clear_unavailable_selection(std::uint32_t profile_id);
  bool background_update_blocked() const;
  DisplayShell display_;
  SettingsStore settings_store_;
  core::DeviceSettings settings_;
  NetworkService network_;
  WebConfig web_config_;
  MoonrakerAdapter moonraker_;
  MoonrakerConnectionProbe moonraker_probe_;
  MoonrakerCameraClient moonraker_camera_;
  InactivePrinterPoller inactive_printer_poller_;
  PrinterDiscoveryService printer_discovery_;
  FirmwareUpdateService firmware_update_;
  ReactionAssetService reaction_assets_;
  BambuLanAdapter bambu_lan_;
  BambuA1PreviewClient bambu_a1_preview_;
  BambuA1CameraClient bambu_a1_camera_;
  BambuCompatibilityProbe bambu_compatibility_;
  OrientationService orientation_;
  AudioService audio_;
  PowerService power_;
  UsbDeveloperService usb_developer_;
  TaskHandle_t monitor_task_ = nullptr;
  TaskHandle_t power_task_ = nullptr;
  TaskHandle_t ui_settings_task_ = nullptr;
  std::atomic<int> pending_brightness_{-1};
  std::atomic<int> pending_audio_volume_{-1};
  std::atomic<int> pending_audio_preset_{-1};
  std::atomic<int> pending_theme_{-1};
  std::atomic<int> pending_language_{-1};
  std::atomic<int> pending_printer_animations_{-1};
  std::atomic<bool> pending_audio_enabled_{true};
  std::atomic<std::uint32_t> pending_selected_profile_{0};
  // The display-state task intentionally has a PSRAM stack and therefore must
  // never access flash/NVS while the cache is disabled. UINT32_MAX means that
  // no selection persistence request is pending; zero is a valid "clear"
  // request.
  std::atomic<std::uint32_t> pending_selected_profile_write_{0xffffffffU};
  std::atomic<int> pending_auto_rotation_{-1};
  std::atomic<int> pending_camera_mode_{-1};
  std::atomic<int> pending_configuration_backup_activity_{-1};
  std::atomic<std::uint8_t> pending_configuration_backup_feedback_{0};
  std::atomic<int> configuration_backup_activity_{
      static_cast<int>(core::ConfigurationBackupActivity::idle)};
  std::atomic<std::uint64_t> configuration_backup_activity_expires_at_ms_{0};
  std::atomic<bool> restart_in_progress_{false};
  std::atomic<bool> pending_restart_audio_{false};
  std::atomic<bool> restart_ready_{false};
  std::atomic<std::uint64_t> restart_expires_at_ms_{0};
  std::atomic<int> selected_printer_protocol_{-1};
  std::atomic<std::uint64_t> unified_api_active_until_ms_{0};
  std::atomic<std::uint64_t> printer_controls_active_until_ms_{0};
  std::atomic<std::uint64_t> pending_web_printer_light_{0};
  std::uint32_t web_printer_light_profile_ = 0;
  bool web_printer_light_target_ = false;
  std::uint64_t web_printer_light_deadline_ms_ = 0;
  std::mutex pending_settings_mutex_;
  std::optional<core::DeviceSettings> pending_settings_;
  bool pending_settings_feedback_ = false;
  bool time_sync_started_ = false;
  bool moonraker_connection_requested_ = false;
  bool bambu_connection_requested_ = false;
  bool moonraker_camera_requested_ = false;
  bool bambu_preview_requested_ = false;
  bool bambu_camera_requested_ = false;
  bool bambu_phase_primed_ = false;
  core::JobPhase last_bambu_phase_ = core::JobPhase::unknown;
  std::uint64_t bambu_completed_reaction_until_ms_ = 0;
  bool bambu_completed_reaction_armed_ = false;
  std::uint64_t a1_preview_not_before_ms_ = 0;
  bool audio_phase_primed_ = false;
  core::JobPhase last_audio_phase_ = core::JobPhase::unknown;
  std::uint8_t audio_progress_milestones_ = 0;
  bool last_material_feeding_ = false;
  std::vector<std::uint64_t> last_hms_codes_;
  bool audio_connection_baseline_pending_ = false;
  std::uint64_t audio_connection_baseline_until_ms_ = 0;
  std::uint32_t audio_online_profile_id_ = 0;
  bool display_phase_primed_ = false;
  core::JobPhase last_display_phase_ = core::JobPhase::unknown;
  float last_display_completion_ = 0.0F;
  bool display_activity_primed_ = false;
  core::PrinterActivity last_display_activity_ = core::PrinterActivity::unknown;
  bool update_installing_previous_ = false;
  std::uint64_t connection_failure_since_ms_ = 0;
  std::uint64_t connection_grace_until_ms_ = 0;
  std::uint32_t pending_dashboard_profile_ = 0;
};

}  // namespace printdeck::platform
