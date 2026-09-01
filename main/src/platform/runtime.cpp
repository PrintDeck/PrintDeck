#include "printdeck/platform/runtime.hpp"

#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "printdeck/platform/bambu_local_connection.hpp"
#include "printdeck/platform/reset_diagnostics.hpp"
#include "printdeck/core/printer_driver.hpp"
#include "printdeck/core/timezone.hpp"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/task_affinity.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace printdeck::platform {
namespace {
constexpr char kLogTag[] = "printdeck";
constexpr std::uint64_t kInitialPrinterConnectionGraceMs = 30'000;
constexpr std::uint64_t kUnavailablePrinterClearDelayMs = 30'000;
// Rendering a printer dashboard can include libpng decoding plus a complete
// LVGL tree rebuild in the same monitor iteration. Four KiB was reproducibly
// exhausted when opening an active Bambu printer. Keep this worker in PSRAM,
// but give the nested rendering path enough stack headroom.
constexpr std::uint32_t kDisplayStateTaskStackBytes = 12U * 1024U;
constexpr std::uint64_t kA1PreviewAfterPrintWakeDelayMs = 2500;
constexpr std::uint64_t kBambuCompletedReactionHoldMs = 45'000;
// Bambu publishes the initial state as several partial MQTT reports. Absorb
// that short burst as one baseline so selecting an already-running printer
// cannot announce its existing filament and progress states as new events.
constexpr std::uint64_t kBambuAudioBaselineSettleMs = 5'000;

void verify_heap(const char* stage) {
  if (!heap_caps_check_integrity_all(true)) {
    ESP_LOGE(kLogTag, "Heap corruption detected after %s", stage);
    abort();
  }
  ESP_LOGI(kLogTag, "Heap OK after %s; internal free=%u, largest=%u", stage,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

bool active_phase(core::JobPhase phase) {
  return phase == core::JobPhase::preparing || phase == core::JobPhase::printing ||
         phase == core::JobPhase::paused;
}

bool terminal_phase(core::JobPhase phase) {
  return phase == core::JobPhase::idle || phase == core::JobPhase::completed ||
         phase == core::JobPhase::failed || phase == core::JobPhase::cancelled;
}

const core::PrinterProfile* selected_profile(const core::DeviceSettings& settings) {
  if (settings.selected_profile == 0) return nullptr;
  const auto selected = std::find_if(
      settings.profiles.begin(), settings.profiles.end(),
      [&settings](const core::PrinterProfile& profile) {
        return profile.id == settings.selected_profile;
      });
  return selected == settings.profiles.end() ? nullptr : &*selected;
}

bool same_printer_configuration(const core::DeviceSettings& current,
                                const core::DeviceSettings& next) {
  if (current.profiles.size() != next.profiles.size() ||
      current.selected_profile != next.selected_profile) {
    return false;
  }
  for (std::size_t index = 0; index < next.profiles.size(); ++index) {
    const auto& old_profile = current.profiles[index];
    const auto& next_profile = next.profiles[index];
    if (old_profile.id != next_profile.id ||
        old_profile.protocol != next_profile.protocol ||
        old_profile.display_name != next_profile.display_name ||
        old_profile.endpoint != next_profile.endpoint ||
        old_profile.api_key != next_profile.api_key ||
        old_profile.serial != next_profile.serial ||
        old_profile.access_code != next_profile.access_code ||
        old_profile.manufacturer != next_profile.manufacturer ||
        old_profile.model != next_profile.model ||
        old_profile.brand != next_profile.brand) {
      return false;
    }
  }
  return true;
}

int protocol_id(const core::PrinterProfile* profile) {
  return profile == nullptr ? -1 : static_cast<int>(profile->protocol);
}

std::uint8_t progress_milestone_mask(float completion) {
  std::uint8_t mask = 0;
  if (completion >= 25.0F) mask |= 0x01;
  if (completion >= 50.0F) mask |= 0x02;
  if (completion >= 75.0F) mask |= 0x04;
  return mask;
}
}

Runtime::Runtime() = default;

void Runtime::start() {
  const esp_err_t board_result = board_early_init();
  if (board_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Early board initialization failed: %s",
             esp_err_to_name(board_result));
    return;
  }
  initialize_reset_diagnostics();
  const esp_err_t nvs_result = nvs_flash_init();
  if (nvs_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Persistent storage initialization failed: %s",
             esp_err_to_name(nvs_result));
    return;
  }
  const esp_err_t settings_result = settings_store_.load(settings_);
  if (settings_result == ESP_OK) {
    if constexpr (!kBoardHasAudio) {
      // KNOMI2 has no speaker or audio output path. Treat it exactly like a
      // permanently muted device while leaving the shared audio service intact.
      settings_.audio_enabled = false;
      settings_.audio_volume_percent = 0;
    }
    display_.set_language(settings_.language);
    display_.set_theme(settings_.theme, settings_.custom_theme);
    display_.set_clock_date_format(core::calendar_date_format(settings_.timezone));
  }
  const int initial_rotation = settings_.rotation == "90" ? 90
                             : settings_.rotation == "180" ? 180
                             : settings_.rotation == "270" ? 270
                             : settings_.rotation == "auto" ? settings_.last_auto_rotation : 0;
  if (display_.start(initial_rotation) != ESP_OK) {
    ESP_LOGE(kLogTag, "Display service could not be started");
    return;
  }
  const esp_err_t developer_result = usb_developer_.start(display_);
  if (developer_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Physical USB developer service is unavailable: %s",
             esp_err_to_name(developer_result));
  }
  verify_heap("display startup");
  if (settings_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Settings could not be loaded: %s", esp_err_to_name(settings_result));
    display_.show_boot_status("Settings need attention");
    return;
  }
  setenv("TZ", core::posix_timezone(settings_.timezone), 1);
  tzset();
  display_.set_language(settings_.language);
  display_.set_theme(settings_.theme, settings_.custom_theme);
  display_.set_printer_animations_enabled(settings_.printer_animations_enabled);
  display_.set_reaction_progress_visibility(
      settings_.reaction_progress_bar_enabled,
      settings_.reaction_progress_percent_enabled);
  display_.set_clock_date_format(core::calendar_date_format(settings_.timezone));
  display_.set_brightness(settings_.brightness_percent);
  display_.set_power_save_policy(settings_.display_power);
  display_.set_audio_state(settings_.audio_enabled, settings_.audio_volume_percent,
                           settings_.audio_preset);
  display_.set_camera_preferences(settings_.camera_mode == "live",
                                  settings_.camera_snapshot_fps);
  audio_.set_language(settings_.language);
  const esp_err_t audio_result = audio_.start(
      settings_.audio_enabled, settings_.audio_volume_percent, settings_.audio_preset,
      settings_.audio_muted_events);
  if (audio_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Audio service is unavailable: %s", esp_err_to_name(audio_result));
  }
  verify_heap("audio startup");
  const esp_err_t power_result = power_.start();
  if (power_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Power service is unavailable: %s", esp_err_to_name(power_result));
  } else if (xTaskCreatePinnedToCore(power_entry, "power_key", 4096, this, 6, &power_task_,
                                     kServiceCore) != pdPASS) {
    power_task_ = nullptr;
    ESP_LOGE(kLogTag, "Power-key task could not be started");
  }
  verify_heap("power startup");
  const esp_err_t orientation_result =
      orientation_.start(display_, settings_.rotation, settings_.last_auto_rotation,
                         rotation_feedback_entry, this);
  if (orientation_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Orientation service is unavailable: %s",
             esp_err_to_name(orientation_result));
  }
  const esp_err_t network_result = network_.start(settings_);
  if (network_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Network could not be started: %s", esp_err_to_name(network_result));
    display_.show_boot_status("Network startup failed");
    return;
  }
  verify_heap("network startup");
  const esp_err_t reactions_result = reaction_assets_.start(network_);
  if (reactions_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Reaction asset service is unavailable: %s",
             esp_err_to_name(reactions_result));
  }
  display_.set_reaction_asset_service(&reaction_assets_);
  firmware_update_.set_background_activity_probe(background_update_blocked_entry, this);
  const esp_err_t update_result = firmware_update_.start(network_);
  if (update_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Firmware update service is unavailable: %s",
             esp_err_to_name(update_result));
  }
  web_config_.set_settings_changed_callback(settings_changed_entry, this);
  web_config_.set_audio_test_callback(audio_test_entry, this);
  const esp_err_t web_result =
      web_config_.start(settings_, settings_store_, network_, moonraker_probe_, printer_discovery_,
                        firmware_update_, reaction_assets_,
                        bambu_compatibility_, inactive_printer_poller_);
  if (web_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Web Config could not be started: %s", esp_err_to_name(web_result));
    display_.show_boot_status("Web Config startup failed");
    return;
  }
  const core::PrinterProfile* selected = selected_profile(settings_);
  selected_printer_protocol_.store(protocol_id(selected), std::memory_order_release);
  if (settings_.selected_profile != 0) {
    connection_grace_until_ms_ =
        static_cast<std::uint64_t>(esp_timer_get_time() / 1000) +
        kInitialPrinterConnectionGraceMs;
  }
  if (settings_.wifi_name.empty()) {
    const NetworkStatus network = network_.status();
    display_.show_wifi_setup(network.setup_network_name.c_str(),
                             network.local_hostname.c_str());
  } else {
    const std::string connecting_status =
        std::string("Connecting to Wi-Fi\n") + settings_.wifi_name;
    display_.show_boot_status(connecting_status.c_str());
  }
  // Camera transports are intentionally configured but not started here. Their
  // PSRAM-backed control workers are created on first real use and may remain
  // asleep afterwards; network, TLS, peer and decoder resources are still
  // enabled only while the CAMERA page is visible.
  moonraker_camera_.configure(selected);
  moonraker_camera_.set_mode(settings_.camera_mode == "live",
                             settings_.camera_snapshot_fps);
  const BambuLocalConnection bambu_connection =
      bambu_local_connection(selected);
  bambu_a1_preview_.configure(bambu_connection);
  bambu_a1_camera_.configure(bambu_connection);
  // Reserve the display-state worker before printer-specific services allocate
  // their larger stacks. Web Config starts earlier, so exhausting task memory
  // here would otherwise leave a healthy, connected device permanently showing
  // the boot screen even though the rest of the application is reachable.
  if (xTaskCreatePinnedToCoreWithCaps(monitor_entry, "display_state",
                                      kDisplayStateTaskStackBytes, this, 3,
                                      &monitor_task_, kServiceCore,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    monitor_task_ = nullptr;
    ESP_LOGE(kLogTag, "Display state task could not be started");
    display_.show_boot_status("Display service unavailable\nOpen Web Config to restart");
  }
  if (xTaskCreatePinnedToCore(ui_settings_entry, "ui_settings", 4096, this, 4,
                              &ui_settings_task_, kServiceCore) != pdPASS) {
    ui_settings_task_ = nullptr;
    ESP_LOGE(kLogTag, "UI settings task could not be started");
  } else {
    display_.set_brightness_changed_callback(brightness_changed_entry, this);
    display_.set_audio_changed_callback(audio_changed_entry, this);
    display_.set_audio_preset_changed_callback(audio_preset_changed_entry, this);
    display_.set_theme_changed_callback(theme_changed_entry, this);
    display_.set_language_changed_callback(language_changed_entry, this);
    display_.set_printer_animations_changed_callback(
        printer_animations_changed_entry, this);
    display_.set_printer_selected_callback(printer_selected_entry, this);
    display_.set_navigation_feedback_callback(navigation_feedback_entry, this);
    display_.set_page_refresh_callback(page_refresh_entry, this);
    display_.set_chamber_light_changed_callback(chamber_light_changed_entry, this);
    display_.set_camera_mode_changed_callback(camera_mode_changed_entry, this);
  }
  display_.set_update_check_callback(update_check_entry, this);
  display_.set_update_install_callback(update_install_entry, this);
  // Full protocol connections are also demand-loaded. My Printers uses the
  // bounded status poller; entering a printer starts exactly one full adapter.
  const esp_err_t inactive_result = inactive_printer_poller_.start(settings_, network_);
  if (inactive_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Inactive-printer refresh could not be started: %s",
             esp_err_to_name(inactive_result));
  }
  ESP_LOGI(kLogTag, "PrintDeck %s starting", PRINTDECK_VERSION);
  mark_reset_checkpoint(ResetCheckpoint::kRunning);
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
  if (running != nullptr && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
      ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
    const esp_err_t confirm_result = esp_ota_mark_app_valid_cancel_rollback();
    if (confirm_result == ESP_OK) ESP_LOGI(kLogTag, "Firmware startup confirmed");
    else ESP_LOGE(kLogTag, "Firmware startup confirmation failed: %s",
                  esp_err_to_name(confirm_result));
  }
#endif
  audio_.play(AudioService::Event::startup);
}

void Runtime::monitor_entry(void* context) {
  static_cast<Runtime*>(context)->monitor_loop();
}

void Runtime::power_entry(void* context) {
  static_cast<Runtime*>(context)->power_loop();
}

void Runtime::ui_settings_entry(void* context) {
  static_cast<Runtime*>(context)->ui_settings_loop();
}

bool Runtime::ensure_moonraker_started(const core::PrinterProfile* selected) {
  const bool was_running = moonraker_.running();
  const esp_err_t result = moonraker_.start(selected, network_);
  if (result == ESP_ERR_INVALID_STATE) return false;
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Moonraker adapter could not be started: %s",
             esp_err_to_name(result));
    return false;
  }
  if (!was_running) ESP_LOGI(kLogTag, "Moonraker adapter started on demand");
  return true;
}

bool Runtime::ensure_bambu_lan_started(const core::PrinterProfile* selected) {
  // start() configures the selected profile before creating the worker. Calling
  // it again for an already-running adapter therefore requests a full MQTT
  // reconfiguration on every monitor pass and prevents the first report from
  // ever settling. Profile changes are delivered explicitly by apply_settings().
  if (bambu_lan_.running()) return true;
  const esp_err_t result = bambu_lan_.start(selected, network_);
  if (result == ESP_ERR_INVALID_STATE) return false;
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Bambu LAN adapter could not be started: %s",
             esp_err_to_name(result));
    return false;
  }
  ESP_LOGI(kLogTag, "Bambu LAN adapter started on demand");
  return true;
}

bool Runtime::ensure_moonraker_camera_started() {
  const bool was_running = moonraker_camera_.running();
  const esp_err_t result = moonraker_camera_.start();
  if (result == ESP_ERR_INVALID_STATE) return false;
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Moonraker camera service could not be started: %s",
             esp_err_to_name(result));
    return false;
  }
  if (!was_running) ESP_LOGI(kLogTag, "Moonraker camera service started on demand");
  return true;
}

bool Runtime::ensure_bambu_preview_started() {
  const bool was_running = bambu_a1_preview_.running();
  const esp_err_t result = bambu_a1_preview_.start();
  if (result == ESP_ERR_INVALID_STATE) return false;
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Bambu print-preview service could not be started: %s",
             esp_err_to_name(result));
    return false;
  }
  if (!was_running) ESP_LOGI(kLogTag, "Bambu print-preview service started on demand");
  return true;
}

bool Runtime::ensure_bambu_camera_started() {
  const bool was_running = bambu_a1_camera_.running();
  const esp_err_t result = bambu_a1_camera_.start();
  if (result == ESP_ERR_INVALID_STATE) return false;
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Bambu camera service could not be started: %s",
             esp_err_to_name(result));
    return false;
  }
  if (!was_running) ESP_LOGI(kLogTag, "Bambu camera service started on demand");
  return true;
}

void Runtime::power_loop() {
  while (true) {
    switch (power_.poll_button()) {
      case PowerButtonAction::wake: display_.reset_inactivity_and_wake(); break;
      case PowerButtonAction::home: display_.return_to_printer_list(); break;
      case PowerButtonAction::show_3:
        display_.show_shutdown_countdown(3);
        audio_.play(AudioService::Event::shutdown_countdown);
        break;
      case PowerButtonAction::show_2:
        display_.show_shutdown_countdown(2);
        audio_.play(AudioService::Event::shutdown_countdown);
        break;
      case PowerButtonAction::show_1:
        display_.show_shutdown_countdown(1);
        audio_.play(AudioService::Event::shutdown_countdown);
        break;
      case PowerButtonAction::cancel: display_.cancel_shutdown_countdown(); break;
      case PowerButtonAction::shutdown: perform_shutdown();
      case PowerButtonAction::none: break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

[[noreturn]] void Runtime::perform_shutdown() {
  ESP_LOGI(kLogTag, "Graceful shutdown started");
  display_.show_shutdown_screen();
  audio_.play(AudioService::Event::shutdown);
  vTaskDelay(pdMS_TO_TICKS(1000));
  const esp_err_t result = power_.power_off();
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Software power-off failed: %s; keep holding POWER",
             esp_err_to_name(result));
  }
  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

void Runtime::brightness_changed_entry(void* context, int percent) {
  auto* runtime = static_cast<Runtime*>(context);
  runtime->pending_brightness_.store(percent, std::memory_order_release);
  if (runtime->ui_settings_task_ != nullptr) xTaskNotifyGive(runtime->ui_settings_task_);
}

void Runtime::audio_changed_entry(void* context, bool enabled, int volume_percent) {
  auto* runtime = static_cast<Runtime*>(context);
  runtime->pending_audio_enabled_.store(enabled, std::memory_order_relaxed);
  runtime->pending_audio_volume_.store(volume_percent, std::memory_order_release);
  if (runtime->ui_settings_task_ != nullptr) xTaskNotifyGive(runtime->ui_settings_task_);
}

void Runtime::audio_preset_changed_entry(void* context, const char* preset) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr || preset == nullptr) return;
  static constexpr const char* ids[]{"modern", "soft", "oldschool",
                                      "arcade", "scifi", "clean"};
  for (int index = 0; index < 6; ++index) {
    if (std::strcmp(preset, ids[index]) == 0) {
      runtime->pending_audio_preset_.store(index, std::memory_order_release);
      if (runtime->ui_settings_task_ != nullptr) xTaskNotifyGive(runtime->ui_settings_task_);
      return;
    }
  }
}

void Runtime::update_check_entry(void* context) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr) return;
  runtime->firmware_update_.request_check();
  if (runtime->monitor_task_ != nullptr) xTaskNotifyGive(runtime->monitor_task_);
}

void Runtime::update_install_entry(void* context) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr) return;
  if (!runtime->firmware_update_.request_install()) {
    ESP_LOGW(kLogTag, "Firmware install request was rejected");
  }
  if (runtime->monitor_task_ != nullptr) xTaskNotifyGive(runtime->monitor_task_);
}

bool Runtime::background_update_blocked_entry(void* context) {
  auto* runtime = static_cast<Runtime*>(context);
  return runtime != nullptr && runtime->background_update_blocked();
}

bool Runtime::background_update_blocked() const {
  const NetworkStatus network = network_.status();
  if (network.recovery_ap_active || printer_discovery_.running() ||
      display_.camera_page_active()) {
    return true;
  }
  if (moonraker_probe_.snapshot().running) return true;
  const BambuCompatibilityState compatibility = bambu_compatibility_.snapshot().state;
  return compatibility == BambuCompatibilityState::kConnecting ||
         compatibility == BambuCompatibilityState::kCollecting ||
         compatibility == BambuCompatibilityState::kProbingServices;
}

void Runtime::theme_changed_entry(void* context, const char* theme) {
  auto* runtime = static_cast<Runtime*>(context);
  static constexpr const char* ids[]{"green", "banana", "sunset", "ice",
                                      "cyberpunk", "ember", "mono", "red",
                                      "ios_glass", "fluent_dark", "retro_terminal", "custom"};
  for (int index = 0; index < 12; ++index) {
    if (theme != nullptr && std::strcmp(theme, ids[index]) == 0) {
      runtime->pending_theme_.store(index, std::memory_order_release);
      if (runtime->ui_settings_task_ != nullptr) xTaskNotifyGive(runtime->ui_settings_task_);
      return;
    }
  }
}

void Runtime::language_changed_entry(void* context, const char* language) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr || language == nullptr) return;
  static constexpr const char* ids[]{"en", "pl", "es", "fr", "de", "zh-CN"};
  for (int index = 0; index < 6; ++index) {
    if (std::strcmp(language, ids[index]) == 0) {
      runtime->pending_language_.store(index, std::memory_order_release);
      if (runtime->ui_settings_task_ != nullptr) xTaskNotifyGive(runtime->ui_settings_task_);
      return;
    }
  }
}

void Runtime::printer_animations_changed_entry(void* context, bool enabled) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr) return;
  runtime->pending_printer_animations_.store(enabled ? 1 : 0,
                                             std::memory_order_release);
  if (runtime->ui_settings_task_ != nullptr) {
    xTaskNotifyGive(runtime->ui_settings_task_);
  }
}

bool Runtime::printer_selected_entry(void* context, std::uint32_t profile_id) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr || profile_id == 0) return false;
  const InactivePrinterSnapshot inactive = runtime->inactive_printer_poller_.snapshot();
  const auto status = std::find_if(
      inactive.printers.begin(), inactive.printers.end(),
      [profile_id](const InactivePrinterStatus& candidate) {
        return candidate.profile_id == profile_id;
      });
  // A confirmed-offline profile remains blocked. Unknown and currently
  // checking profiles are accepted: selecting one starts the bounded full
  // connection attempt after any lightweight probe has yielded ownership.
  if (status != inactive.printers.end() && status->available &&
      !status->checking && !status->connected) {
    return false;
  }
  std::uint32_t expected = 0;
  if (!runtime->pending_selected_profile_.compare_exchange_strong(
          expected, profile_id, std::memory_order_acq_rel)) {
    return false;
  }
  if (runtime->monitor_task_ != nullptr) xTaskNotifyGive(runtime->monitor_task_);
  return true;
}

void Runtime::chamber_light_changed_entry(void* context, bool enabled) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr) return;
  const int protocol =
      runtime->selected_printer_protocol_.load(std::memory_order_acquire);
  if (protocol == static_cast<int>(core::PrinterProtocol::moonraker)) {
    runtime->moonraker_.request_chamber_light(enabled);
  } else if (protocol == static_cast<int>(core::PrinterProtocol::bambu_lan)) {
    runtime->bambu_lan_.request_chamber_light(enabled);
  }
}

void Runtime::camera_mode_changed_entry(void* context, bool live) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr) return;
  runtime->pending_camera_mode_.store(live ? 1 : 0, std::memory_order_release);
  if (runtime->ui_settings_task_ != nullptr) xTaskNotifyGive(runtime->ui_settings_task_);
}

void Runtime::navigation_feedback_entry(void* context) {
  static_cast<Runtime*>(context)->audio_.play(AudioService::Event::navigation);
}

void Runtime::rotation_feedback_entry(void* context, int degrees) {
  auto* runtime = static_cast<Runtime*>(context);
  runtime->audio_.play(AudioService::Event::orientation);
  if (runtime->settings_.rotation == "auto" &&
      runtime->settings_.display_power.wake_on_orientation_change) {
    runtime->display_.reset_inactivity_and_wake();
  }
  runtime->pending_auto_rotation_.store(degrees, std::memory_order_release);
  if (runtime->ui_settings_task_ != nullptr) xTaskNotifyGive(runtime->ui_settings_task_);
}

void Runtime::page_refresh_entry(void* context) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime->monitor_task_ != nullptr) xTaskNotifyGive(runtime->monitor_task_);
}

void Runtime::ui_settings_loop() {
  int auto_rotation_to_save = -1;
  std::uint64_t auto_rotation_save_due_ms = 0;
  while (true) {
    TickType_t wait = portMAX_DELAY;
    if (auto_rotation_to_save >= 0) {
      const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
      wait = now >= auto_rotation_save_due_ms
                 ? 0
                 : pdMS_TO_TICKS(static_cast<std::uint32_t>(auto_rotation_save_due_ms - now));
    }
    ulTaskNotifyTake(pdTRUE, wait);
    const std::uint32_t selected_profile_write =
        pending_selected_profile_write_.exchange(0xffffffffU,
                                                 std::memory_order_acq_rel);
    if (selected_profile_write != 0xffffffffU) {
      const esp_err_t result =
          settings_store_.save_selected_profile(selected_profile_write);
      if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "Selected printer could not be saved: %s",
                 esp_err_to_name(result));
      }
    }
    const int auto_rotation = pending_auto_rotation_.exchange(-1, std::memory_order_acq_rel);
    if (auto_rotation >= 0) {
      auto_rotation_to_save = auto_rotation;
      auto_rotation_save_due_ms =
          static_cast<std::uint64_t>(esp_timer_get_time() / 1000) + 3000ULL;
    }
    const int brightness = pending_brightness_.exchange(-1, std::memory_order_acq_rel);
    if (brightness >= 0) {
      const esp_err_t result = web_config_.save_brightness(brightness);
      if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "Brightness could not be saved: %s", esp_err_to_name(result));
      } else if (audio_.enabled() && audio_.volume() > 0) {
        audio_.play(AudioService::Event::test);
      }
    }
    const int volume = pending_audio_volume_.exchange(-1, std::memory_order_acq_rel);
    if (volume >= 0) {
      const bool enabled = pending_audio_enabled_.load(std::memory_order_relaxed);
      audio_.set_enabled(enabled);
      audio_.set_volume(volume);
      const esp_err_t result = web_config_.save_audio(enabled, volume);
      if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "Audio settings could not be saved: %s", esp_err_to_name(result));
      } else if (enabled && volume > 0) {
        audio_.play(AudioService::Event::test);
      }
    }
    const int requested_audio_preset =
        pending_audio_preset_.exchange(-1, std::memory_order_acq_rel);
    if (requested_audio_preset >= 0 && requested_audio_preset < 6) {
      static constexpr const char* ids[]{"modern", "soft", "oldschool",
                                          "arcade", "scifi", "clean"};
      AudioService::Preset preset = AudioService::Preset::modern;
      AudioService::preset_from_id(ids[requested_audio_preset], preset);
      audio_.set_preset(preset);
      const esp_err_t result = web_config_.save_audio_preset(ids[requested_audio_preset]);
      if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "Audio preset could not be saved: %s", esp_err_to_name(result));
      } else {
        // The preset is already active above, so this confirmation is rendered
        // with the sound set the user has just selected on either display.
        if (audio_.enabled() && audio_.volume() > 0) {
          audio_.play(AudioService::Event::test);
        }
      }
    }
    const int requested_theme = pending_theme_.exchange(-1, std::memory_order_acq_rel);
    if (requested_theme >= 0 && requested_theme < 12) {
      static constexpr const char* ids[]{"green", "banana", "sunset", "ice",
                                          "cyberpunk", "ember", "mono", "red",
                                          "ios_glass", "fluent_dark", "retro_terminal", "custom"};
      bool theme_changed = false;
      const esp_err_t result = web_config_.save_theme(ids[requested_theme], theme_changed);
      if (result == ESP_OK) {
        if (theme_changed && audio_.enabled() && audio_.volume() > 0) {
          audio_.play(AudioService::Event::test);
        }
      } else {
        ESP_LOGE(kLogTag, "Theme could not be saved: %s", esp_err_to_name(result));
      }
    }
    const int requested_language = pending_language_.exchange(-1, std::memory_order_acq_rel);
    if (requested_language >= 0 && requested_language < 6) {
      static constexpr const char* ids[]{"en", "pl", "es", "fr", "de", "zh-CN"};
      const bool language_changed = settings_.language != ids[requested_language];
      const esp_err_t result = web_config_.save_language(ids[requested_language]);
      if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "Interface language could not be saved: %s",
                 esp_err_to_name(result));
      } else if (language_changed && audio_.enabled() && audio_.volume() > 0) {
        audio_.play(AudioService::Event::test);
      }
    }
    const int requested_printer_animations =
        pending_printer_animations_.exchange(-1, std::memory_order_acq_rel);
    if (requested_printer_animations >= 0) {
      const bool enabled = requested_printer_animations != 0;
      const bool animations_changed = settings_.printer_animations_enabled != enabled;
      const esp_err_t result = web_config_.save_printer_animations(
          enabled);
      if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "Printer animation setting could not be saved: %s",
                 esp_err_to_name(result));
      } else if (animations_changed && audio_.enabled() && audio_.volume() > 0) {
        audio_.play(AudioService::Event::test);
      }
    }
    const int camera_mode = pending_camera_mode_.exchange(-1, std::memory_order_acq_rel);
    if (camera_mode >= 0) {
      const esp_err_t result = web_config_.save_camera_mode(camera_mode != 0);
      if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "Camera mode could not be saved: %s", esp_err_to_name(result));
      }
    }
    if (auto_rotation_to_save >= 0 &&
        static_cast<std::uint64_t>(esp_timer_get_time() / 1000) >=
            auto_rotation_save_due_ms) {
      const esp_err_t result = web_config_.save_last_auto_rotation(auto_rotation_to_save);
      if (result != ESP_OK) {
        ESP_LOGW(kLogTag, "Automatic rotation could not be remembered: %s",
                 esp_err_to_name(result));
      }
      auto_rotation_to_save = -1;
      auto_rotation_save_due_ms = 0;
    }
  }
}

void Runtime::settings_changed_entry(void* context, const core::DeviceSettings& settings,
                                     bool play_feedback) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr) return;
  {
    const std::lock_guard<std::mutex> lock(runtime->pending_settings_mutex_);
    runtime->pending_settings_ = settings;
    runtime->pending_settings_feedback_ =
        runtime->pending_settings_feedback_ || play_feedback;
  }
  if (runtime->monitor_task_ != nullptr) xTaskNotifyGive(runtime->monitor_task_);
}

bool Runtime::audio_test_entry(void* context, std::string_view preset_id,
                               std::string_view event_id, int volume_percent) {
  auto* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr) return false;
  AudioService::Preset preset;
  if (!AudioService::preset_from_id(preset_id, preset)) return false;
  AudioService::Event event;
  if (event_id == "startup") event = AudioService::Event::startup;
  else if (event_id == "navigation") event = AudioService::Event::navigation;
  else if (event_id == "orientation") event = AudioService::Event::orientation;
  else if (event_id == "print_started") event = AudioService::Event::print_started;
  else if (event_id == "progress_25") event = AudioService::Event::progress_25;
  else if (event_id == "progress_50") event = AudioService::Event::progress_50;
  else if (event_id == "progress_75") event = AudioService::Event::progress_75;
  else if (event_id == "print_paused") event = AudioService::Event::print_paused;
  else if (event_id == "print_finished") event = AudioService::Event::print_finished;
  else if (event_id == "print_error") event = AudioService::Event::print_error;
  else if (event_id == "hms_alert") event = AudioService::Event::hms_alert;
  else if (event_id == "filament_attention") event = AudioService::Event::filament_attention;
  else if (event_id == "shutdown_countdown") event = AudioService::Event::shutdown_countdown;
  else if (event_id == "shutdown") event = AudioService::Event::shutdown;
  else return false;
  return runtime->audio_.preview(event, preset, volume_percent);
}

void Runtime::apply_pending_settings() {
  std::optional<core::DeviceSettings> pending;
  bool play_feedback = false;
  {
    const std::lock_guard<std::mutex> lock(pending_settings_mutex_);
    pending.swap(pending_settings_);
    play_feedback = pending_settings_feedback_;
    pending_settings_feedback_ = false;
  }
  if (pending) apply_settings(*pending, play_feedback);
}

void Runtime::apply_settings(const core::DeviceSettings& settings, bool play_feedback) {
  const bool printer_configuration_changed =
      !same_printer_configuration(settings_, settings);
  const bool rotation_changed = settings_.rotation != settings.rotation;
  const bool brightness_changed =
      settings_.brightness_percent != settings.brightness_percent;
  AudioService::Preset requested_preset = AudioService::Preset::modern;
  AudioService::preset_from_id(settings.audio_preset, requested_preset);
  const bool audio_changed = audio_.enabled() != settings.audio_enabled ||
                             audio_.volume() != settings.audio_volume_percent ||
                             audio_.preset() != requested_preset ||
                             audio_.muted_events() != settings.audio_muted_events;
  settings_ = settings;
  const core::PrinterProfile* selected = selected_profile(settings_);
  selected_printer_protocol_.store(protocol_id(selected), std::memory_order_release);
  display_.set_language(settings.language);
  display_.set_clock_date_format(core::calendar_date_format(settings.timezone));
  // Brightness is a polling CO5300 SPI command. Do not resend it for unrelated
  // settings changes (for example every theme selection) while LVGL may have
  // an asynchronous color transfer in flight.
  if (brightness_changed) display_.set_brightness(settings.brightness_percent);
  display_.set_power_save_policy(settings.display_power);
  display_.set_theme(settings.theme, settings.custom_theme);
  display_.set_printer_animations_enabled(settings.printer_animations_enabled);
  display_.set_reaction_progress_visibility(
      settings.reaction_progress_bar_enabled,
      settings.reaction_progress_percent_enabled);
  display_.set_audio_state(settings.audio_enabled, settings.audio_volume_percent,
                           settings.audio_preset);
  display_.set_camera_preferences(settings.camera_mode == "live",
                                  settings.camera_snapshot_fps);
  if (rotation_changed) {
    const esp_err_t rotation_result = orientation_.configure(settings.rotation);
    if (rotation_result != ESP_OK) {
      ESP_LOGW(kLogTag, "Display rotation could not be applied: %s",
               esp_err_to_name(rotation_result));
    }
  }
  moonraker_camera_.set_mode(settings.camera_mode == "live",
                             settings.camera_snapshot_fps);
  audio_.set_enabled(settings.audio_enabled);
  audio_.set_volume(settings.audio_volume_percent);
  audio_.set_preset(requested_preset);
  audio_.set_muted_events(settings.audio_muted_events);
  audio_.set_language(settings.language);
  // A Web Config save is remote user activity. Apply the complete new state
  // first, then restore the display exactly as a local touch would so the
  // result is visible even when battery power saving made the screen dark.
  if (play_feedback) display_.reset_inactivity_and_wake();
  if ((audio_changed || play_feedback) && settings.audio_enabled &&
      settings.audio_volume_percent > 0) {
    audio_.play(AudioService::Event::test);
  }
  inactive_printer_poller_.configure(settings);
  if (printer_configuration_changed) {
    moonraker_.configure(selected);
    moonraker_camera_.configure(selected);
    bambu_lan_.configure(selected);
    const BambuLocalConnection bambu_connection = bambu_local_connection(selected);
    bambu_a1_preview_.configure(bambu_connection);
    bambu_a1_camera_.configure(bambu_connection);
    bambu_phase_primed_ = false;
    bambu_completed_reaction_until_ms_ = 0;
    bambu_completed_reaction_armed_ = false;
    audio_phase_primed_ = false;
    audio_connection_baseline_pending_ = selected != nullptr;
    audio_connection_baseline_until_ms_ = 0;
    audio_online_profile_id_ = 0;
    display_phase_primed_ = false;
    display_activity_primed_ = false;
    last_display_activity_ = core::PrinterActivity::unknown;
    connection_failure_since_ms_ = 0;
    connection_grace_until_ms_ = settings.selected_profile == 0
        ? 0
        : static_cast<std::uint64_t>(esp_timer_get_time() / 1000) +
              kInitialPrinterConnectionGraceMs;
    pending_dashboard_profile_ = 0;
    display_.return_to_printer_list();
  }
}

void Runtime::apply_pending_printer_selection() {
  const std::uint32_t selected =
      pending_selected_profile_.load(std::memory_order_acquire);
  if (selected == 0) return;
  if (selected == settings_.selected_profile) {
    pending_selected_profile_.store(0, std::memory_order_release);
    return;
  }
  const auto profile = std::find_if(settings_.profiles.begin(), settings_.profiles.end(),
                                    [selected](const core::PrinterProfile& candidate) {
                                      return candidate.id == selected;
                                    });
  if (profile == settings_.profiles.end()) {
    pending_selected_profile_.store(0, std::memory_order_release);
    return;
  }

  const InactivePrinterSnapshot inactive = inactive_printer_poller_.snapshot();
  const auto status = std::find_if(
      inactive.printers.begin(), inactive.printers.end(),
      [selected](const InactivePrinterStatus& candidate) {
        return candidate.profile_id == selected;
      });
  if (status != inactive.printers.end() && status->available &&
      !status->checking && !status->connected) {
    pending_selected_profile_.store(0, std::memory_order_release);
    return;
  }
  if (inactive_printer_poller_.check_in_progress(selected)) return;
  pending_selected_profile_.store(0, std::memory_order_release);

  core::DeviceSettings candidate = settings_;
  candidate.selected_profile = selected;
  pending_selected_profile_write_.store(selected, std::memory_order_release);
  if (ui_settings_task_ != nullptr) xTaskNotifyGive(ui_settings_task_);
  web_config_.synchronize_settings(candidate);
  apply_settings(candidate, false);
  pending_dashboard_profile_ = selected;
}

bool Runtime::clear_unavailable_selection(std::uint32_t profile_id) {
  if (profile_id == 0 || settings_.selected_profile != profile_id) return false;
  core::DeviceSettings candidate = settings_;
  candidate.selected_profile = 0;
  pending_selected_profile_write_.store(0, std::memory_order_release);
  if (ui_settings_task_ != nullptr) xTaskNotifyGive(ui_settings_task_);
  settings_ = std::move(candidate);
  selected_printer_protocol_.store(-1, std::memory_order_release);
  web_config_.synchronize_settings(settings_);
  moonraker_.configure(nullptr);
  moonraker_camera_.configure(nullptr);
  bambu_lan_.configure(nullptr);
  inactive_printer_poller_.configure(settings_);
  inactive_printer_poller_.mark_offline(profile_id);
  bambu_a1_preview_.configure({});
  bambu_a1_camera_.configure({});
  bambu_phase_primed_ = false;
  bambu_completed_reaction_until_ms_ = 0;
  bambu_completed_reaction_armed_ = false;
  audio_phase_primed_ = false;
  audio_connection_baseline_pending_ = false;
  audio_connection_baseline_until_ms_ = 0;
  audio_online_profile_id_ = 0;
  display_phase_primed_ = false;
  display_activity_primed_ = false;
  last_display_activity_ = core::PrinterActivity::unknown;
  connection_failure_since_ms_ = 0;
  connection_grace_until_ms_ = 0;
  pending_dashboard_profile_ = 0;
  web_config_.update_selected_printer_status(
      0, core::LinkState::stopped, core::JobPhase::unknown, 0.0F);
  display_.return_to_printer_list();
  ESP_LOGW(kLogTag,
           "Printer %lu remained unavailable; cleared selection and returned to My Printers",
           static_cast<unsigned long>(profile_id));
  return true;
}

core::PrinterSnapshot Runtime::update_bambu_snapshot() {
  core::PrinterSnapshot snapshot = bambu_lan_.snapshot();
  const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  const bool active = active_phase(snapshot.job.phase);
  const core::JobPhase previous_phase = last_bambu_phase_;
  if (!bambu_phase_primed_) {
    bambu_phase_primed_ = true;
    last_bambu_phase_ = snapshot.job.phase;
    bambu_completed_reaction_until_ms_ = 0;
    bambu_completed_reaction_armed_ =
        active && snapshot.job.completion < 99.95F;
  } else {
    if (active && snapshot.job.completion < 99.95F) {
      // A real new print re-arms the one-shot completion reaction. Some Bambu
      // firmware briefly repeats a stale RUNNING report at 100% after FINISH;
      // that must not extend the completed reaction indefinitely.
      bambu_completed_reaction_armed_ = true;
    }
    if (active && terminal_phase(previous_phase)) {
      a1_preview_not_before_ms_ = now_ms + kA1PreviewAfterPrintWakeDelayMs;
      mark_reset_checkpoint(ResetCheckpoint::kPrintWake);
    } else if (terminal_phase(snapshot.job.phase)) {
      a1_preview_not_before_ms_ = 0;
    }
    if (snapshot.job.phase == core::JobPhase::completed &&
        active_phase(previous_phase) && bambu_completed_reaction_armed_) {
      // Bambu may keep reporting FINISH indefinitely. Celebrate only the
      // observed active-to-FINISH transition; repeated FINISH reports must not
      // restart the hold. A stale FINISH seen after boot/reconnect has no
      // observed transition and therefore presents as standby immediately.
      bambu_completed_reaction_until_ms_ =
          now_ms + kBambuCompletedReactionHoldMs;
      bambu_completed_reaction_armed_ = false;
    } else if (snapshot.job.phase != core::JobPhase::completed &&
               snapshot.job.phase != core::JobPhase::unknown && !active) {
      bambu_completed_reaction_until_ms_ = 0;
      bambu_completed_reaction_armed_ = false;
    }
    if (snapshot.job.phase != core::JobPhase::unknown) {
      last_bambu_phase_ = snapshot.job.phase;
    }
  }
  if (snapshot.job.phase == core::JobPhase::completed) {
    snapshot.job.activity = bambu_completed_reaction_until_ms_ != 0 &&
                                    now_ms < bambu_completed_reaction_until_ms_
                                ? core::PrinterActivity::completed
                                : core::PrinterActivity::standby;
  }
  const bool preview_allowed = active &&
      (a1_preview_not_before_ms_ == 0 || now_ms >= a1_preview_not_before_ms_);
  bambu_a1_preview_.set_job(snapshot.job.preview_hint, snapshot.job.name,
                            snapshot.job.preview_plate_hint, preview_allowed);
  const BambuA1PreviewSnapshot preview = bambu_a1_preview_.snapshot();
  if (active && preview.image && !preview.image->empty()) {
    snapshot.job.preview = preview.image;
  } else if (!active) {
    snapshot.job.preview.reset();
  }
  const BambuA1CameraSnapshot camera = bambu_a1_camera_.snapshot();
  snapshot.job.camera_supported = camera.supported;
  snapshot.job.camera_detail = camera.detail;
  if (camera.frame && !camera.frame->empty()) {
    snapshot.job.camera_frame = camera.frame;
    snapshot.job.camera_width = camera.width;
    snapshot.job.camera_height = camera.height;
  } else {
    snapshot.job.camera_frame.reset();
    snapshot.job.camera_width = 0;
    snapshot.job.camera_height = 0;
  }
  return snapshot;
}

void Runtime::prime_audio_state(const core::PrinterSnapshot& snapshot) {
  bool material_feeding = snapshot.job.materials.external_spool.feeding;
  for (const auto& slot : snapshot.job.materials.slots) {
    material_feeding = material_feeding || slot.feeding;
  }
  audio_phase_primed_ = true;
  last_audio_phase_ = snapshot.job.phase;
  audio_progress_milestones_ = progress_milestone_mask(snapshot.job.completion);
  last_material_feeding_ = material_feeding;
  last_hms_codes_ = snapshot.job.hms_codes;
}

void Runtime::update_audio_state(const core::PrinterSnapshot& snapshot) {
  const core::JobPhase phase = snapshot.job.phase;
  bool material_feeding = snapshot.job.materials.external_spool.feeding;
  for (const auto& slot : snapshot.job.materials.slots) {
    material_feeding = material_feeding || slot.feeding;
  }
  if (!audio_phase_primed_) {
    prime_audio_state(snapshot);
    return;
  }
  const bool filament_started =
      material_feeding && !last_material_feeding_ && active_phase(phase);
  last_material_feeding_ = material_feeding;
  const bool new_hms = !snapshot.job.hms_codes.empty() &&
                       snapshot.job.hms_codes != last_hms_codes_;
  last_hms_codes_ = snapshot.job.hms_codes;
  if (phase == core::JobPhase::unknown) {
    if (new_hms) audio_.play(AudioService::Event::hms_alert);
    return;
  }
  const bool was_active = active_phase(last_audio_phase_);
  bool primary_event_played = false;
  if (active_phase(phase) && terminal_phase(last_audio_phase_)) {
    audio_progress_milestones_ = progress_milestone_mask(snapshot.job.completion);
    audio_.play(AudioService::Event::print_started);
    primary_event_played = true;
  } else if (phase == core::JobPhase::paused && last_audio_phase_ != core::JobPhase::paused) {
    audio_.play(AudioService::Event::print_paused);
    primary_event_played = true;
  } else if (phase == core::JobPhase::completed && was_active) {
    audio_.play(AudioService::Event::print_finished);
    primary_event_played = true;
  } else if (phase == core::JobPhase::failed && last_audio_phase_ != core::JobPhase::failed) {
    audio_.play(AudioService::Event::print_error);
    primary_event_played = true;
  }
  if (!primary_event_played && filament_started) {
    audio_.play(AudioService::Event::filament_attention);
    primary_event_played = true;
  }
  if (active_phase(phase)) {
    const std::uint8_t reached = progress_milestone_mask(snapshot.job.completion);
    const std::uint8_t newly_reached = reached & ~audio_progress_milestones_;
    if (!primary_event_played) {
      if ((newly_reached & 0x04) != 0) {
        audio_.play(AudioService::Event::progress_75);
        primary_event_played = true;
      } else if ((newly_reached & 0x02) != 0) {
        audio_.play(AudioService::Event::progress_50);
        primary_event_played = true;
      } else if ((newly_reached & 0x01) != 0) {
        audio_.play(AudioService::Event::progress_25);
        primary_event_played = true;
      }
    }
    audio_progress_milestones_ |= reached;
  }
  if (new_hms && !primary_event_played) audio_.play(AudioService::Event::hms_alert);
  last_audio_phase_ = phase;
}

void Runtime::monitor_loop() {
  while (true) {
    // Physical touch and the transition-switch callback run inside the core-1
    // LVGL task. Coalesce background state while they are active, then render
    // the newest snapshot once the short quiet window closes. This prevents a
    // core-0 refresh from spending a full timeout contending for the LVGL
    // mutex without delaying the foreground gesture or changing animation and
    // camera cadence.
    const std::uint32_t render_delay_ms = display_.background_render_delay_ms();
    if (render_delay_ms > 0) {
      vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(render_delay_ms)));
      continue;
    }
    apply_pending_settings();
    apply_pending_printer_selection();
    const NetworkStatus network = network_.status();
    const PowerSnapshot power = power_.sample();
    const FirmwareUpdateSnapshot update = firmware_update_.snapshot();
    display_.set_update_snapshot(update);
    const core::PrinterProfile* selected = selected_profile(settings_);
    core::PrinterSnapshot selected_snapshot;
    bool selected_snapshot_ready = selected == nullptr;
    bambu_a1_preview_.set_network_ready(network.station_connected);
    bambu_a1_camera_.set_network_ready(network.station_connected);
    moonraker_camera_.set_network_ready(network.station_connected);
    const bool screen_visible = !display_.screen_fully_off();
    const bool printer_detail_active = selected != nullptr &&
        (!display_.printer_list_visible() || pending_dashboard_profile_ == selected->id);
    const bool selected_is_bambu = selected != nullptr &&
        selected->protocol == core::PrinterProtocol::bambu_lan;
    const bool selected_is_moonraker = selected != nullptr &&
        selected->protocol == core::PrinterProtocol::moonraker;
    const bool full_connection_active = network.station_connected && printer_detail_active;
    const bool want_bambu_connection = full_connection_active && selected_is_bambu;
    const bool want_moonraker_connection = full_connection_active && selected_is_moonraker;
    const bool camera_page_visible = display_.camera_page_active() && screen_visible;
    const bool bambu_print_active = selected_is_bambu &&
        active_phase(bambu_lan_.snapshot().job.phase);
    const bool want_bambu_preview =
        want_bambu_connection && screen_visible && bambu_print_active;
    const bool want_bambu_camera = want_bambu_connection && camera_page_visible;
    const bool want_moonraker_camera = want_moonraker_connection && camera_page_visible;

    // Disable camera network/decode work immediately off-page, but keep the
    // small PSRAM-backed control tasks asleep. Re-entering CAMERA is therefore
    // immediate and does not repeatedly allocate/free task stacks.
    if (moonraker_camera_requested_ && !want_moonraker_camera) {
      moonraker_camera_.set_enabled(false);
    }
    if (bambu_camera_requested_ && !want_bambu_camera) {
      bambu_a1_camera_.set_enabled(false);
    }
    if (bambu_preview_requested_ && !want_bambu_preview) {
      bambu_a1_preview_.stop();
    }
    if (moonraker_connection_requested_ && !want_moonraker_connection) {
      moonraker_.stop();
    }
    if (bambu_connection_requested_ && !want_bambu_connection) {
      bambu_lan_.stop();
    }
    if ((moonraker_camera_requested_ || bambu_camera_requested_) &&
        !want_moonraker_camera && !want_bambu_camera) {
      display_.release_camera_frame();
    }
    if (bambu_preview_requested_ && !want_bambu_preview) {
      display_.release_printer_preview();
    }

    moonraker_connection_requested_ = want_moonraker_connection;
    bambu_connection_requested_ = want_bambu_connection;
    moonraker_camera_requested_ = want_moonraker_camera;
    bambu_preview_requested_ = want_bambu_preview;
    bambu_camera_requested_ = want_bambu_camera;
    const std::uint32_t pending_profile =
        pending_selected_profile_.load(std::memory_order_acquire);
    inactive_printer_poller_.set_active_profile(
        full_connection_active && selected != nullptr
            ? selected->id
            : pending_profile);

    bool full_adapter_ready = false;
    if (want_bambu_connection && !moonraker_.running()) {
      full_adapter_ready = ensure_bambu_lan_started(selected);
    } else if (want_moonraker_connection && !bambu_lan_.running()) {
      full_adapter_ready = ensure_moonraker_started(selected);
    }
    if (want_bambu_preview && full_adapter_ready) ensure_bambu_preview_started();
    const bool bambu_camera_ready = !want_bambu_camera ||
        (full_adapter_ready && ensure_bambu_camera_started());
    const bool moonraker_camera_ready = !want_moonraker_camera ||
        (full_adapter_ready && ensure_moonraker_camera_started());
    bambu_a1_camera_.set_enabled(want_bambu_camera && bambu_camera_ready);
    moonraker_camera_.set_enabled(want_moonraker_camera && moonraker_camera_ready);

    const InactivePrinterSnapshot inactive = inactive_printer_poller_.snapshot();
    const auto lightweight_snapshot = [&inactive](const core::PrinterProfile& profile) {
      core::PrinterSnapshot snapshot;
      snapshot.profile_id = profile.id;
      const auto status = std::find_if(
          inactive.printers.begin(), inactive.printers.end(),
          [&profile](const InactivePrinterStatus& candidate) {
            return candidate.profile_id == profile.id;
          });
      if (status == inactive.printers.end() || !status->available) {
        snapshot.link = core::LinkState::connecting;
        snapshot.link_detail = "Waiting for printer status probe";
        return snapshot;
      }
      snapshot.link = status->checking ? core::LinkState::connecting
                                      : status->connected ? core::LinkState::online
                                                          : core::LinkState::failed;
      snapshot.link_detail = status->checking ? "Checking printer status"
                                              : status->connected ? "Printer available"
                                                                  : "Printer unavailable";
      snapshot.job.reachable = status->connected;
      snapshot.job.phase = status->phase;
      snapshot.job.kind = status->kind;
      snapshot.job.name = status->job_name;
      snapshot.job.remaining_seconds = status->remaining_seconds;
      return snapshot;
    };
    if (network.station_connected) {
      if (!time_sync_started_) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, const_cast<char*>("pool.ntp.org"));
        esp_sntp_init();
        time_sync_started_ = true;
      }
      if (selected != nullptr && want_bambu_connection && full_adapter_ready) {
        selected_snapshot = update_bambu_snapshot();
      } else if (selected != nullptr && want_moonraker_connection && full_adapter_ready) {
        selected_snapshot = moonraker_.snapshot();
        const MoonrakerCameraSnapshot camera = moonraker_camera_.snapshot();
        selected_snapshot.job.camera_supported = camera.supported;
        selected_snapshot.job.camera_live_supported = camera.live_supported;
        selected_snapshot.job.camera_refreshing = camera.refreshing;
        selected_snapshot.job.camera_detail = camera.detail;
        if (camera.frame && !camera.frame->empty()) {
          selected_snapshot.job.camera_frame = camera.frame;
          selected_snapshot.job.camera_width = camera.width;
          selected_snapshot.job.camera_height = camera.height;
        } else {
          selected_snapshot.job.camera_frame.reset();
          selected_snapshot.job.camera_width = 0;
          selected_snapshot.job.camera_height = 0;
        }
      } else if (selected != nullptr && !full_connection_active) {
        selected_snapshot = lightweight_snapshot(*selected);
      }
      if (selected != nullptr) {
        selected_snapshot_ready = selected_snapshot.profile_id == selected->id;
        if (!selected_snapshot_ready) {
          // Adapter reconfiguration is asynchronous. Never expose the cached
          // snapshot belonging to the previously selected printer while the
          // new adapter instance is still switching profiles.
          selected_snapshot = {};
          selected_snapshot.profile_id = selected->id;
          selected_snapshot.link = core::LinkState::connecting;
          selected_snapshot.link_detail = "Waiting for selected printer data";
        }
      }
      if (selected != nullptr && full_connection_active &&
          selected_snapshot.link != core::LinkState::online) {
        // My Printers already owns a recent, bounded status probe. Reuse its
        // confirmed job state while the full protocol adapter reconnects so
        // the first reactions frame does not imply that the printer failed.
        const core::PrinterSnapshot last_known = lightweight_snapshot(*selected);
        core::retain_last_known_job_during_reconnect(selected_snapshot, last_known);
      }
      if (selected != nullptr && selected_is_bambu && full_connection_active &&
          selected_snapshot.link != core::LinkState::online &&
          display_phase_primed_) {
        // Returning from the dashboard intentionally unloads the full Bambu
        // MQTT connection. When the user immediately re-enters, retain the
        // last rendered job state during the short reconnect instead of
        // flashing the unknown/unavailable reaction. The real link state is
        // left untouched, so an actual connection failure still follows the
        // normal grace and selection-clear path.
        if (selected_snapshot.job.phase == core::JobPhase::unknown) {
          selected_snapshot.job.phase = last_display_phase_;
        }
        selected_snapshot.job.completion = last_display_completion_;
        if (display_activity_primed_ &&
            selected_snapshot.job.activity == core::PrinterActivity::unknown) {
          selected_snapshot.job.activity = last_display_activity_;
        }
      }
      web_config_.update_selected_printer_status(
          selected != nullptr ? selected->id : 0,
          selected != nullptr ? selected_snapshot.link : core::LinkState::stopped,
          selected != nullptr ? selected_snapshot.job.phase : core::JobPhase::unknown,
          selected != nullptr ? selected_snapshot.job.completion : 0.0F);
      const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
      const bool selected_online = selected != nullptr && selected_snapshot_ready &&
                                   selected_snapshot.link == core::LinkState::online;
      if (!selected_online || !full_connection_active) {
        audio_online_profile_id_ = 0;
        audio_connection_baseline_pending_ = selected != nullptr;
        audio_connection_baseline_until_ms_ = 0;
        audio_phase_primed_ = false;
      }
      if (selected_online && full_connection_active) {
        if (audio_online_profile_id_ != selected->id) {
          audio_online_profile_id_ = selected->id;
          audio_connection_baseline_pending_ = true;
          audio_connection_baseline_until_ms_ = selected_is_bambu
              ? now_ms + kBambuAudioBaselineSettleMs
              : now_ms;
          audio_phase_primed_ = false;
        }
        if (audio_connection_baseline_pending_) {
          prime_audio_state(selected_snapshot);
          if (now_ms >= audio_connection_baseline_until_ms_) {
            audio_connection_baseline_pending_ = false;
            audio_connection_baseline_until_ms_ = 0;
          }
        } else {
          update_audio_state(selected_snapshot);
        }
      }
      const bool selected_unavailable = selected != nullptr &&
          core::printer_selection_unavailable(
              selected->id, selected_snapshot_ready ? &selected_snapshot : nullptr,
              now_ms >= connection_grace_until_ms_);
      if (selected_online) {
        connection_failure_since_ms_ = 0;
        connection_grace_until_ms_ = 0;
      } else if (!selected_unavailable) {
        connection_failure_since_ms_ = 0;
      } else if (connection_failure_since_ms_ == 0) {
        connection_failure_since_ms_ = now_ms;
      } else if (now_ms >= connection_failure_since_ms_ +
                               kUnavailablePrinterClearDelayMs) {
        const std::uint32_t unavailable_id = selected->id;
        if (clear_unavailable_selection(unavailable_id)) {
          // All pointers and snapshots above describe the former selection.
          // Render the persisted no-selection state on the next bounded pass.
          vTaskDelay(pdMS_TO_TICKS(1));
          continue;
        }
        connection_failure_since_ms_ = now_ms;
      }
      bool wake_after_snapshot = false;
      bool focus_reaction_after_snapshot = false;
      if (selected != nullptr && selected_snapshot_ready && full_connection_active) {
        const core::JobPhase phase = selected_snapshot.job.phase;
        const core::PrinterActivity activity =
            core::effective_printer_activity(selected_snapshot.job);
        if (!display_phase_primed_) {
          display_phase_primed_ = true;
          last_display_phase_ = phase;
          last_display_completion_ = selected_snapshot.job.completion;
        } else {
          wake_after_snapshot = core::display_wake_transition(
              last_display_phase_, last_display_completion_, phase,
              selected_snapshot.job.completion);
          if (wake_after_snapshot) mark_reset_checkpoint(ResetCheckpoint::kPrintWake);
          if (phase != core::JobPhase::unknown) last_display_phase_ = phase;
          last_display_completion_ = selected_snapshot.job.completion;
        }
        if (!display_activity_primed_) {
          display_activity_primed_ = true;
          last_display_activity_ = activity;
        } else {
          const bool animation_changed = settings_.printer_animations_enabled &&
              core::animation_wake_transition(last_display_activity_, activity);
          if (animation_changed) mark_reset_checkpoint(ResetCheckpoint::kPrintWake);
          wake_after_snapshot = wake_after_snapshot || animation_changed;
          focus_reaction_after_snapshot = animation_changed;
          if (activity != core::PrinterActivity::unknown) {
            last_display_activity_ = activity;
          }
        }
      }
      if (focus_reaction_after_snapshot) {
        display_.focus_printer_reactions_if_dashboard_visible();
      }
      const int rendered_page = display_.page();
      const bool rendered_printer_list = display_.printer_list_visible();
      switch (rendered_page) {
        case 0:
          if (selected == nullptr || display_.printer_list_visible()) {
            display_.show_my_printers(network.ipv4.c_str(), network.local_hostname.c_str(),
                                      settings_.profiles,
                                      settings_.selected_profile,
                                      inactive,
                                      power,
                                      selected != nullptr ? &selected_snapshot : nullptr);
            if (selected != nullptr && selected_snapshot_ready &&
                selected_snapshot.link == core::LinkState::online &&
                pending_dashboard_profile_ == selected->id) {
              display_.open_printer_when_ready(selected->id);
              pending_dashboard_profile_ = 0;
            }
          } else if (selected->protocol == core::PrinterProtocol::moonraker) {
            display_.show_printer(*selected, selected_snapshot, power, network.ipv4.c_str());
          } else {
            display_.show_printer(*selected, selected_snapshot, power, network.ipv4.c_str());
          }
          break;
        case 1:
          display_.show_system_details(network, power, settings_.profiles.size());
          break;
        case 2:
          display_.show_clock(false, power);
          break;
        case 3:
          display_.show_clock(true, power);
          break;
        case 4:
          display_.show_web_config(network.ipv4.c_str(), network.local_hostname.c_str(),
                                   power);
          break;
      }
      display_.finish_horizontal_transition(rendered_page, rendered_printer_list,
                                            selected != nullptr && selected_snapshot_ready
                                                ? selected->id : 0);
      if (wake_after_snapshot) {
        // Reveal the already-updated dashboard in one wake transition. Waking
        // before rebuilding the active-print view can overlap AMOLED resume
        // traffic with a heavy LVGL redraw.
        mark_reset_checkpoint(ResetCheckpoint::kPrintWakeResume);
        display_.reset_inactivity_and_wake();
        mark_reset_checkpoint(ResetCheckpoint::kRunning);
      }
    } else if (network.station_connection_failed) {
      display_.show_wifi_error(network.station_name.c_str());
    } else if (network.recovery_ap_active) {
      display_.show_wifi_setup(network.setup_network_name.c_str(),
                               network.local_hostname.c_str());
    }
    const bool print_active = selected != nullptr && selected_snapshot_ready &&
                              active_phase(selected_snapshot.job.phase);
    const bool update_installing = update.state == FirmwareUpdateState::downloading ||
                                   update.state == FirmwareUpdateState::rebooting;
    if (update_installing) {
      // Installation is a deliberate exception to human-only inactivity:
      // keep the display fully awake throughout the critical operation.
      display_.reset_inactivity_and_wake();
    } else if (update_installing_previous_) {
      // Give a terminal success/failure message a complete idle interval.
      display_.reset_inactivity_and_wake();
    }
    update_installing_previous_ = update_installing;
    const bool provisioning = network.recovery_ap_active && !network.station_connected;
    const bool keep_awake = provisioning || update_installing;
    display_.update_power_save(power.available && !power.usb_present && !power.charging,
                               keep_awake, print_active);
    if (display_.screen_fully_off()) {
      // Keep the selected camera page in place, but release its network and
      // decoder work while the user cannot see it. Touch or POWER restores the
      // display first; the next bounded monitor pass resumes the same camera
      // mode and refresh cadence.
      bambu_a1_camera_.set_enabled(false);
      moonraker_camera_.set_enabled(false);
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
}

}  // namespace printdeck::platform
