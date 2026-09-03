#pragma once

#include <atomic>
#include <array>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "printdeck/core/settings.hpp"
#include "printdeck/core/configuration_backup.hpp"
#include "printdeck/core/unified_printer_api.hpp"
#include "printdeck/platform/network_service.hpp"
#include "printdeck/platform/settings_store.hpp"
#include "printdeck/platform/moonraker_connection_probe.hpp"
#include "printdeck/platform/printer_discovery_service.hpp"
#include "printdeck/platform/firmware_update_service.hpp"
#include "printdeck/platform/bambu_compatibility_probe.hpp"
#include "printdeck/platform/inactive_printer_poller.hpp"
#include "printdeck/platform/reaction_asset_service.hpp"

namespace printdeck::platform {

class WebConfig {
 public:
  using SettingsChangedCallback =
      void (*)(void*, const core::DeviceSettings&, bool play_feedback);
  using AudioTestCallback =
      bool (*)(void*, std::string_view preset, std::string_view event, int volume_percent);
  using ConfigurationBackupActivityCallback =
      void (*)(void*, core::ConfigurationBackupActivity activity, bool play_feedback);
  using RestartRequestedCallback = bool (*)(void* context);
  using SelectedPrinterSnapshotCallback =
      bool (*)(void* context, core::PrinterSnapshot& destination);
  using UnifiedApiActivityCallback = void (*)(void* context);
  using PrinterLightCallback = bool (*)(void* context, std::uint32_t profile_id, bool enabled);

  esp_err_t start(const core::DeviceSettings& settings, const SettingsStore& store,
                  NetworkService& network, MoonrakerConnectionProbe& moonraker_probe,
                  PrinterDiscoveryService& printer_discovery,
                  FirmwareUpdateService& firmware_update,
                  ReactionAssetService& reaction_assets,
                  BambuCompatibilityProbe& compatibility_probe,
                  const InactivePrinterPoller& inactive_printer_poller);
  void set_settings_changed_callback(SettingsChangedCallback callback, void* context);
  void set_audio_test_callback(AudioTestCallback callback, void* context);
  void set_configuration_backup_activity_callback(
      ConfigurationBackupActivityCallback callback, void* context);
  void set_restart_requested_callback(RestartRequestedCallback callback, void* context);
  void set_selected_printer_snapshot_callback(
      SelectedPrinterSnapshotCallback callback, void* context);
  void set_unified_api_activity_callback(UnifiedApiActivityCallback callback, void* context);
  void set_printer_controls_callbacks(UnifiedApiActivityCallback activity,
                                     PrinterLightCallback light, void* context);
  void synchronize_settings(const core::DeviceSettings& settings);
  void update_selected_printer_status(const core::PrinterSnapshot& snapshot);
  esp_err_t save_brightness(int percent);
  esp_err_t save_audio(bool enabled, int volume_percent);
  esp_err_t save_audio_preset(std::string_view preset);
  esp_err_t save_camera_mode(bool live);
  esp_err_t save_theme(const char* theme, bool& changed);
  esp_err_t save_language(std::string_view language);
  esp_err_t save_printer_animations(bool enabled);
  esp_err_t save_last_auto_rotation(int degrees);

 private:
  static esp_err_t root_entry(httpd_req_t* request);
  static esp_err_t world_map_entry(httpd_req_t* request);
  static esp_err_t localizations_entry(httpd_req_t* request);
  static esp_err_t reactions_script_entry(httpd_req_t* request);
  static esp_err_t reaction_set_preview_entry(httpd_req_t* request);
  static esp_err_t health_entry(httpd_req_t* request);
  static esp_err_t device_info_entry(httpd_req_t* request);
  static esp_err_t brand_logos_entry(httpd_req_t* request);
  static esp_err_t wifi_entry(httpd_req_t* request);
  static esp_err_t wifi_scan_entry(httpd_req_t* request);
  static esp_err_t printer_entry(httpd_req_t* request);
  static esp_err_t printers_get_entry(httpd_req_t* request);
  static esp_err_t printers_manage_entry(httpd_req_t* request);
  static esp_err_t printer_light_entry(httpd_req_t* request);
  static esp_err_t printer_discovery_start_entry(httpd_req_t* request);
  static esp_err_t printer_discovery_status_entry(httpd_req_t* request);
  static esp_err_t printer_discovery_cancel_entry(httpd_req_t* request);
  static esp_err_t update_status_entry(httpd_req_t* request);
  static esp_err_t update_check_entry(httpd_req_t* request);
  static esp_err_t update_install_entry(httpd_req_t* request);
  static esp_err_t update_upload_entry(httpd_req_t* request);
  static esp_err_t update_url_entry(httpd_req_t* request);
  static esp_err_t settings_get_entry(httpd_req_t* request);
  static esp_err_t settings_post_entry(httpd_req_t* request);
  static esp_err_t unified_api_settings_get_entry(httpd_req_t* request);
  static esp_err_t unified_api_settings_post_entry(httpd_req_t* request);
  static esp_err_t unified_api_info_entry(httpd_req_t* request);
  static esp_err_t unified_api_snapshot_entry(httpd_req_t* request);
  static esp_err_t unified_api_printers_entry(httpd_req_t* request);
  static esp_err_t unified_api_statuses_entry(httpd_req_t* request);
  static esp_err_t unified_api_printer_entry(httpd_req_t* request);
  static esp_err_t configuration_backup_export_entry(httpd_req_t* request);
  static esp_err_t configuration_backup_check_entry(httpd_req_t* request);
  static esp_err_t configuration_backup_restore_entry(httpd_req_t* request);
  static esp_err_t configuration_backup_activity_entry(httpd_req_t* request);
  static esp_err_t configuration_backup_reaction_export_entry(httpd_req_t* request);
  static esp_err_t configuration_backup_reaction_restore_entry(httpd_req_t* request);
  static esp_err_t audio_test_entry(httpd_req_t* request);
  static esp_err_t reactions_get_entry(httpd_req_t* request);
  static esp_err_t reactions_set_entry(httpd_req_t* request);
  static esp_err_t reactions_set_cancel_entry(httpd_req_t* request);
  static esp_err_t reactions_event_entry(httpd_req_t* request);
  static esp_err_t reactions_upload_entry(httpd_req_t* request);
  static esp_err_t reactions_gif_entry(httpd_req_t* request);
  static esp_err_t moonraker_check_start_entry(httpd_req_t* request);
  static esp_err_t moonraker_check_status_entry(httpd_req_t* request);
  static esp_err_t compatibility_start_entry(httpd_req_t* request);
  static esp_err_t compatibility_status_entry(httpd_req_t* request);
  static esp_err_t compatibility_report_entry(httpd_req_t* request);
  static esp_err_t compatibility_cancel_entry(httpd_req_t* request);
  static esp_err_t captive_entry(httpd_req_t* request);
  static void restart_entry(void* context);

  esp_err_t serve_root(httpd_req_t* request) const;
  esp_err_t serve_world_map(httpd_req_t* request) const;
  esp_err_t serve_localizations(httpd_req_t* request) const;
  esp_err_t serve_reactions_script(httpd_req_t* request) const;
  esp_err_t serve_reaction_set_preview(httpd_req_t* request) const;
  esp_err_t serve_health(httpd_req_t* request) const;
  esp_err_t serve_device_info(httpd_req_t* request) const;
  esp_err_t serve_brand_logos(httpd_req_t* request) const;
  esp_err_t save_wifi(httpd_req_t* request);
  esp_err_t serve_wifi_scan(httpd_req_t* request);
  esp_err_t save_printer(httpd_req_t* request);
  esp_err_t serve_printers(httpd_req_t* request) const;
  esp_err_t manage_printer(httpd_req_t* request);
  esp_err_t set_printer_light(httpd_req_t* request);
  esp_err_t start_printer_discovery(httpd_req_t* request);
  esp_err_t serve_printer_discovery(httpd_req_t* request,
                                    std::optional<bool> started = std::nullopt) const;
  esp_err_t cancel_printer_discovery(httpd_req_t* request);
  esp_err_t serve_update_status(httpd_req_t* request) const;
  esp_err_t request_update_check(httpd_req_t* request);
  esp_err_t request_update_install(httpd_req_t* request);
  esp_err_t upload_update(httpd_req_t* request);
  esp_err_t install_update_url(httpd_req_t* request);
  esp_err_t serve_settings(httpd_req_t* request) const;
  esp_err_t save_settings(httpd_req_t* request);
  esp_err_t serve_unified_api_settings(httpd_req_t* request) const;
  esp_err_t save_unified_api_settings(httpd_req_t* request);
  esp_err_t serve_unified_api_info(httpd_req_t* request) const;
  esp_err_t serve_unified_api_snapshot(httpd_req_t* request) const;
  esp_err_t serve_unified_api_printers(httpd_req_t* request) const;
  esp_err_t serve_unified_api_statuses(httpd_req_t* request) const;
  esp_err_t serve_unified_api_printer(httpd_req_t* request) const;
  bool authorize_unified_api(httpd_req_t* request) const;
  std::vector<core::UnifiedPrinterView> unified_printer_views() const;
  esp_err_t export_configuration_backup(httpd_req_t* request) const;
  esp_err_t check_configuration_backup(httpd_req_t* request) const;
  esp_err_t restore_configuration_backup(httpd_req_t* request);
  esp_err_t update_configuration_backup_activity(httpd_req_t* request);
  esp_err_t export_configuration_backup_reaction(httpd_req_t* request) const;
  esp_err_t restore_configuration_backup_reaction(httpd_req_t* request);
  esp_err_t test_audio(httpd_req_t* request);
  esp_err_t serve_reactions(httpd_req_t* request) const;
  esp_err_t select_reaction_set(httpd_req_t* request);
  esp_err_t cancel_reaction_set(httpd_req_t* request);
  esp_err_t manage_reaction_event(httpd_req_t* request);
  esp_err_t upload_reaction_gif(httpd_req_t* request);
  esp_err_t serve_reaction_gif(httpd_req_t* request) const;
  esp_err_t start_moonraker_check(httpd_req_t* request);
  esp_err_t serve_moonraker_check_status(httpd_req_t* request) const;
  esp_err_t start_compatibility_probe(httpd_req_t* request);
  esp_err_t serve_compatibility_status(httpd_req_t* request) const;
  esp_err_t serve_compatibility_report(httpd_req_t* request) const;
  esp_err_t cancel_compatibility_probe(httpd_req_t* request);
  esp_err_t serve_captive_request(httpd_req_t* request) const;
  esp_err_t request_restart();
  esp_err_t schedule_restart();
  bool derive_configuration_backup_key(std::span<const std::uint8_t> password,
                                       std::span<const std::uint8_t, 16> salt,
                                       std::array<std::uint8_t, 32>& key) const;
  const char* localized(std::string_view english) const;
  void notify_settings_changed(const core::DeviceSettings& settings, bool play_feedback);
  bool notify_configuration_backup_activity(
      core::ConfigurationBackupActivity activity, bool play_feedback) const;

  mutable std::mutex settings_write_mutex_;
  mutable std::mutex backup_crypto_mutex_;
  mutable std::mutex mutex_;
  core::DeviceSettings settings_;
  const SettingsStore* store_ = nullptr;
  NetworkService* network_ = nullptr;
  MoonrakerConnectionProbe* moonraker_probe_ = nullptr;
  PrinterDiscoveryService* printer_discovery_ = nullptr;
  FirmwareUpdateService* firmware_update_ = nullptr;
  ReactionAssetService* reaction_assets_ = nullptr;
  BambuCompatibilityProbe* compatibility_probe_ = nullptr;
  const InactivePrinterPoller* inactive_printer_poller_ = nullptr;
  std::uint32_t selected_status_profile_ = 0;
  core::LinkState selected_link_ = core::LinkState::stopped;
  core::JobPhase selected_phase_ = core::JobPhase::unknown;
  float selected_completion_ = 0.0F;
  struct PrinterLightState {
    bool supported = false;
    bool on = false;
    bool pending = false;
    bool target_on = false;
  } selected_light_;
  UnifiedApiActivityCallback printer_controls_activity_callback_ = nullptr;
  PrinterLightCallback printer_light_callback_ = nullptr;
  void* printer_controls_context_ = nullptr;
  httpd_handle_t server_ = nullptr;
  esp_timer_handle_t restart_timer_ = nullptr;
  SettingsChangedCallback settings_changed_callback_ = nullptr;
  void* settings_changed_context_ = nullptr;
  AudioTestCallback audio_test_callback_ = nullptr;
  void* audio_test_context_ = nullptr;
  ConfigurationBackupActivityCallback configuration_backup_activity_callback_ = nullptr;
  void* configuration_backup_activity_context_ = nullptr;
  RestartRequestedCallback restart_requested_callback_ = nullptr;
  void* restart_requested_context_ = nullptr;
  SelectedPrinterSnapshotCallback selected_printer_snapshot_callback_ = nullptr;
  void* selected_printer_snapshot_context_ = nullptr;
  UnifiedApiActivityCallback unified_api_activity_callback_ = nullptr;
  void* unified_api_activity_context_ = nullptr;
  mutable std::atomic<std::uint64_t> unified_api_next_request_ms_{0};
  mutable std::array<std::uint8_t, 16> backup_crypto_salt_{};
  mutable std::array<std::uint8_t, 32> backup_crypto_verifier_{};
  mutable std::array<std::uint8_t, 32> backup_crypto_key_{};
  mutable std::uint64_t backup_crypto_expires_at_ms_ = 0;
  std::string configuration_backup_activity_token_;
  core::ConfigurationBackupActivity configuration_backup_activity_ =
      core::ConfigurationBackupActivity::idle;
  std::uint64_t configuration_backup_activity_expires_at_ms_ = 0;
};

}  // namespace printdeck::platform
