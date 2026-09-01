#pragma once

#include <mutex>
#include <optional>
#include <string_view>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "printdeck/core/settings.hpp"
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

  esp_err_t start(const core::DeviceSettings& settings, const SettingsStore& store,
                  NetworkService& network, MoonrakerConnectionProbe& moonraker_probe,
                  PrinterDiscoveryService& printer_discovery,
                  FirmwareUpdateService& firmware_update,
                  ReactionAssetService& reaction_assets,
                  BambuCompatibilityProbe& compatibility_probe,
                  const InactivePrinterPoller& inactive_printer_poller);
  void set_settings_changed_callback(SettingsChangedCallback callback, void* context);
  void set_audio_test_callback(AudioTestCallback callback, void* context);
  void synchronize_settings(const core::DeviceSettings& settings);
  void update_selected_printer_status(std::uint32_t profile_id, core::LinkState link,
                                      core::JobPhase phase, float completion);
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
  esp_err_t schedule_restart();
  const char* localized(std::string_view english) const;
  void notify_settings_changed(const core::DeviceSettings& settings, bool play_feedback);

  mutable std::mutex settings_write_mutex_;
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
  httpd_handle_t server_ = nullptr;
  esp_timer_handle_t restart_timer_ = nullptr;
  SettingsChangedCallback settings_changed_callback_ = nullptr;
  void* settings_changed_context_ = nullptr;
  AudioTestCallback audio_test_callback_ = nullptr;
  void* audio_test_context_ = nullptr;
};

}  // namespace printdeck::platform
