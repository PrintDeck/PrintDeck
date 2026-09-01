#include "printdeck/platform/web_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "printdeck/core/printer_driver.hpp"
#include "printdeck/core/localization.hpp"
#include "printdeck/core/timezone.hpp"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/reset_diagnostics.hpp"
#include "printdeck/platform/web_assets.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "printdeck/platform/usb_developer_service.hpp"
#include "img/printer_brand_logos_web.h"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "web_config";
constexpr std::size_t kMaximumFormBytes = 1024;
constexpr std::int64_t kRestartDelayUs = 750'000;
constexpr std::size_t kOtaReceiveBufferBytes = 4096;
constexpr std::uint64_t kOtaReceiveDeadlineMs = 120'000;

const char* job_phase_id(core::JobPhase phase) {
  switch (phase) {
    case core::JobPhase::idle: return "idle";
    case core::JobPhase::preparing: return "preparing";
    case core::JobPhase::printing: return "printing";
    case core::JobPhase::paused: return "paused";
    case core::JobPhase::completed: return "completed";
    case core::JobPhase::failed: return "failed";
    case core::JobPhase::cancelled: return "cancelled";
    case core::JobPhase::unknown: return "unknown";
  }
  return "unknown";
}

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

bool decode_form_component(std::string_view input, std::string& output) {
  output.clear();
  output.reserve(input.size());
  for (std::size_t index = 0; index < input.size(); ++index) {
    if (input[index] == '+') {
      output.push_back(' ');
    } else if (input[index] == '%') {
      if (index + 2 >= input.size()) return false;
      const int high = hex_value(input[index + 1]);
      const int low = hex_value(input[index + 2]);
      if (high < 0 || low < 0) return false;
      const char decoded = static_cast<char>((high << 4) | low);
      if (decoded == '\0' || decoded == '\r' || decoded == '\n') return false;
      output.push_back(decoded);
      index += 2;
    } else {
      output.push_back(input[index]);
    }
  }
  return true;
}

bool form_value(std::string_view form, std::string_view wanted, std::string& value) {
  std::size_t cursor = 0;
  while (cursor <= form.size()) {
    const std::size_t end = form.find('&', cursor);
    const std::string_view pair = form.substr(
        cursor, end == std::string_view::npos ? form.size() - cursor : end - cursor);
    const std::size_t separator = pair.find('=');
    if (separator != std::string_view::npos && pair.substr(0, separator) == wanted) {
      return decode_form_component(pair.substr(separator + 1), value);
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1;
  }
  return false;
}

bool parse_id(std::string_view text, std::uint32_t& value) {
  value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_int(std::string_view text, int& value) {
  value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_color(std::string_view text, std::uint32_t& value) {
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);
  if (text.size() != 6) return false;
  value = 0;
  for (const char digit : text) {
    const int nibble = hex_value(digit);
    if (nibble < 0) return false;
    value = (value << 4U) | static_cast<std::uint32_t>(nibble);
  }
  return true;
}

std::string canonical_brand(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  for (const auto& [needle, brand] : {
           std::pair{"creality", "creality"}, std::pair{"snapmaker", "snapmaker"},
           std::pair{"prusa", "prusa"}, std::pair{"bambu", "bambu"},
           std::pair{"anycubic", "anycubic"}, std::pair{"elegoo", "elegoo"},
           std::pair{"qidi", "qidi"}, std::pair{"sovol", "sovol"},
           std::pair{"flashforge", "flashforge"}, std::pair{"ankermake", "ankermake"},
           std::pair{"voron", "voron"}, std::pair{"ratrig", "ratrig"},
           std::pair{"rat rig", "ratrig"}, std::pair{"klipper", "klipper"}}) {
    if (value.find(needle) != std::string::npos) return brand;
  }
  return "generic";
}

void append_json_string(std::string& target, std::string_view value) {
  target.push_back('"');
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') {
      target.push_back('\\');
      target.push_back(static_cast<char>(byte));
    } else if (byte == '\n') {
      target += "\\n";
    } else if (byte >= 0x20) {
      target.push_back(static_cast<char>(byte));
    }
  }
  target.push_back('"');
}

void append_theme_catalog(std::string& target, const core::ThemeColors& custom) {
  static constexpr std::array<std::string_view, 12> ids = {
      "green", "banana", "sunset", "ice", "cyberpunk",
      "ember", "mono", "red", "ios_glass", "fluent_dark",
      "retro_terminal", "custom"};
  target += "{\"schema\":2,\"themes\":{";
  bool first = true;
  for (const std::string_view id : ids) {
    if (!first) target.push_back(',');
    first = false;
    append_json_string(target, id);
    target += ":{";
    const core::ThemeColors colors = core::resolved_theme(id, custom);
    const core::ThemeStyle style = core::resolved_theme_style(id, colors);
    const auto value = [&](const char* name, std::uint32_t color, bool leading = true) {
      if (leading) target.push_back(',');
      target.push_back('"');
      target += name;
      target += "\":" + std::to_string(color & 0xFFFFFFU);
    };
    value("printing", colors.printing, false);
    value("done", colors.done);
    value("error", colors.error);
    value("idle", colors.idle);
    value("preparing", colors.preparing);
    value("paused", colors.paused);
    value("filament", colors.filament);
    value("setup", colors.setup);
    value("offline", colors.offline);
    value("unknown", colors.unknown);
    value("preview_background", colors.preview_background);
    value("background", style.background);
    value("background_secondary", style.background_secondary);
    value("surface", style.surface);
    value("surface_raised", style.surface_raised);
    value("surface_soft", style.surface_soft);
    value("border", style.border);
    value("track", style.track);
    value("text_primary", style.text_primary);
    value("text_secondary", style.text_secondary);
    value("text_muted", style.text_muted);
    value("accent", style.accent);
    value("accent_secondary", style.accent_secondary);
    value("on_accent", style.on_accent);
    value("corner_radius", style.corner_radius);
    value("terminal_typography", style.terminal_typography ? 1U : 0U);
    value("glass_effect", style.glass_effect ? 1U : 0U);
    target.push_back('}');
  }
  target += "}}";
}

bool receive_form(httpd_req_t* request, std::string& body) {
  if (request->content_len <= 0 ||
      request->content_len > static_cast<int>(kMaximumFormBytes)) return false;
  body.assign(static_cast<std::size_t>(request->content_len), '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received, body.size() - received);
    if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (count <= 0) return false;
    received += static_cast<std::size_t>(count);
  }
  return true;
}

bool query_value(httpd_req_t* request, const char* key, std::string& value) {
  const std::size_t length = httpd_req_get_url_query_len(request);
  if (length == 0 || length > 255) return false;
  std::array<char, 256> query{};
  std::array<char, 96> result{};
  if (httpd_req_get_url_query_str(request, query.data(), query.size()) != ESP_OK ||
      httpd_query_key_value(query.data(), key, result.data(), result.size()) != ESP_OK) {
    return false;
  }
  value = result.data();
  return true;
}

esp_err_t send_json(httpd_req_t* request, const char* status, const char* body) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_sendstr(request, body);
}

}  // namespace

esp_err_t WebConfig::start(const core::DeviceSettings& settings, const SettingsStore& store,
                           NetworkService& network,
                           MoonrakerConnectionProbe& moonraker_probe,
                           PrinterDiscoveryService& printer_discovery,
                           FirmwareUpdateService& firmware_update,
                           ReactionAssetService& reaction_assets,
                           BambuCompatibilityProbe& compatibility_probe,
                           const InactivePrinterPoller& inactive_printer_poller) {
  if (server_ != nullptr) return ESP_ERR_INVALID_STATE;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
  }
  store_ = &store;
  network_ = &network;
  moonraker_probe_ = &moonraker_probe;
  printer_discovery_ = &printer_discovery;
  firmware_update_ = &firmware_update;
  reaction_assets_ = &reaction_assets;
  compatibility_probe_ = &compatibility_probe;
  inactive_printer_poller_ = &inactive_printer_poller;

  const esp_timer_create_args_t timer_args = {
      .callback = restart_entry,
      .arg = this,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "config_restart",
      .skip_unhandled_events = true,
  };
  esp_err_t result = esp_timer_create(&timer_args, &restart_timer_);
  if (result != ESP_OK) return result;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.core_id = kServiceCore;
  config.max_uri_handlers = 42;
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;
  result = httpd_start(&server_, &config);
  if (result != ESP_OK) return result;

  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = root_entry, .user_ctx = this},
      {.uri = "/world-map.svg", .method = HTTP_GET, .handler = world_map_entry, .user_ctx = this},
      {.uri = "/localizations.js", .method = HTTP_GET, .handler = localizations_entry, .user_ctx = this},
      {.uri = "/reactions.js", .method = HTTP_GET, .handler = reactions_script_entry, .user_ctx = this},
      {.uri = "/api/reactions/set-preview", .method = HTTP_GET, .handler = reaction_set_preview_entry, .user_ctx = this},
      {.uri = "/api/health", .method = HTTP_GET, .handler = health_entry, .user_ctx = this},
      {.uri = "/api/device", .method = HTTP_GET, .handler = device_info_entry, .user_ctx = this},
      {.uri = "/api/brand-logos", .method = HTTP_GET, .handler = brand_logos_entry, .user_ctx = this},
      {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_entry, .user_ctx = this},
      {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_entry, .user_ctx = this},
      {.uri = "/api/printers", .method = HTTP_POST, .handler = printer_entry, .user_ctx = this},
      {.uri = "/api/printers", .method = HTTP_GET, .handler = printers_get_entry, .user_ctx = this},
      {.uri = "/api/printers/manage", .method = HTTP_POST, .handler = printers_manage_entry, .user_ctx = this},
      {.uri = "/api/printers/discover", .method = HTTP_POST, .handler = printer_discovery_start_entry, .user_ctx = this},
      {.uri = "/api/printers/discover", .method = HTTP_GET, .handler = printer_discovery_status_entry, .user_ctx = this},
      {.uri = "/api/printers/discover/cancel", .method = HTTP_POST, .handler = printer_discovery_cancel_entry, .user_ctx = this},
      {.uri = "/api/update/status", .method = HTTP_GET, .handler = update_status_entry, .user_ctx = this},
      {.uri = "/api/update/check", .method = HTTP_POST, .handler = update_check_entry, .user_ctx = this},
      {.uri = "/api/update/install", .method = HTTP_POST, .handler = update_install_entry, .user_ctx = this},
      {.uri = "/api/update/upload", .method = HTTP_POST, .handler = update_upload_entry, .user_ctx = this},
      {.uri = "/api/update/url", .method = HTTP_POST, .handler = update_url_entry, .user_ctx = this},
      {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_entry, .user_ctx = this},
      {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_entry, .user_ctx = this},
      {.uri = "/api/audio/test", .method = HTTP_POST, .handler = audio_test_entry, .user_ctx = this},
      {.uri = "/api/reactions", .method = HTTP_GET, .handler = reactions_get_entry, .user_ctx = this},
      {.uri = "/api/reactions/set", .method = HTTP_POST, .handler = reactions_set_entry, .user_ctx = this},
      {.uri = "/api/reactions/set/cancel", .method = HTTP_POST, .handler = reactions_set_cancel_entry, .user_ctx = this},
      {.uri = "/api/reactions/event", .method = HTTP_POST, .handler = reactions_event_entry, .user_ctx = this},
      {.uri = "/api/reactions/upload", .method = HTTP_POST, .handler = reactions_upload_entry, .user_ctx = this},
      {.uri = "/api/reactions/gif", .method = HTTP_GET, .handler = reactions_gif_entry, .user_ctx = this},
      {.uri = "/api/moonraker/check/start", .method = HTTP_POST, .handler = moonraker_check_start_entry, .user_ctx = this},
      {.uri = "/api/moonraker/check/status", .method = HTTP_GET, .handler = moonraker_check_status_entry, .user_ctx = this},
      {.uri = "/api/bambu/compatibility/start", .method = HTTP_POST, .handler = compatibility_start_entry, .user_ctx = this},
      {.uri = "/api/bambu/compatibility/status", .method = HTTP_GET, .handler = compatibility_status_entry, .user_ctx = this},
      {.uri = "/api/bambu/compatibility/report", .method = HTTP_GET, .handler = compatibility_report_entry, .user_ctx = this},
      {.uri = "/api/bambu/compatibility/cancel", .method = HTTP_POST, .handler = compatibility_cancel_entry, .user_ctx = this},
      {.uri = "/*", .method = HTTP_GET, .handler = captive_entry, .user_ctx = this},
  };
  for (const auto& route : routes) {
    result = httpd_register_uri_handler(server_, &route);
    if (result != ESP_OK) return result;
  }
  ESP_LOGI(kLogTag, "Web Config started");
  return ESP_OK;
}

void WebConfig::update_selected_printer_status(std::uint32_t profile_id,
                                               core::LinkState link,
                                               core::JobPhase phase,
                                               float completion) {
  const std::lock_guard<std::mutex> lock(mutex_);
  selected_status_profile_ = profile_id;
  selected_link_ = link;
  selected_phase_ = phase;
  selected_completion_ = std::clamp(completion, 0.0F, 100.0F);
}

void WebConfig::set_settings_changed_callback(SettingsChangedCallback callback, void* context) {
  const std::lock_guard<std::mutex> lock(mutex_);
  settings_changed_callback_ = callback;
  settings_changed_context_ = context;
}

void WebConfig::set_audio_test_callback(AudioTestCallback callback, void* context) {
  const std::lock_guard<std::mutex> lock(mutex_);
  audio_test_callback_ = callback;
  audio_test_context_ = context;
}

void WebConfig::synchronize_settings(const core::DeviceSettings& settings) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  const std::lock_guard<std::mutex> lock(mutex_);
  settings_ = settings;
}

void WebConfig::notify_settings_changed(const core::DeviceSettings& settings,
                                        bool play_feedback) {
  SettingsChangedCallback callback = nullptr;
  void* callback_context = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    callback = settings_changed_callback_;
    callback_context = settings_changed_context_;
  }
  if (callback != nullptr) callback(callback_context, settings, play_feedback);
}

const char* WebConfig::localized(std::string_view english) const {
  std::string language;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    language = settings_.language;
  }
  return core::localized_text(language, english);
}

esp_err_t WebConfig::save_brightness(int percent) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  candidate.brightness_percent = static_cast<std::uint8_t>(std::clamp(percent, 5, 100));
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) return result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, false);
  return ESP_OK;
}

esp_err_t WebConfig::save_audio(bool enabled, int volume_percent) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  candidate.audio_enabled = kBoardHasAudio && enabled;
  candidate.audio_volume_percent = kBoardHasAudio
      ? static_cast<std::uint8_t>(std::clamp(volume_percent, 0, 100))
      : 0;
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) return result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, false);
  return ESP_OK;
}

esp_err_t WebConfig::save_audio_preset(std::string_view preset) {
  if (!core::supported_audio_preset(preset)) return ESP_ERR_INVALID_ARG;
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  candidate.audio_preset.assign(preset);
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) return result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, false);
  return ESP_OK;
}

esp_err_t WebConfig::save_camera_mode(bool live) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  candidate.camera_mode = live ? "live" : "snapshots";
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) return result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, false);
  return ESP_OK;
}

esp_err_t WebConfig::save_theme(const char* theme, bool& changed) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  if (theme == nullptr) return ESP_ERR_INVALID_ARG;
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  changed = candidate.theme != theme;
  if (!changed) return ESP_OK;
  candidate.theme = theme;
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) return result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, false);
  return ESP_OK;
}

esp_err_t WebConfig::save_language(std::string_view language) {
  if (!core::supported_language(language)) return ESP_ERR_INVALID_ARG;
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  candidate.language.assign(language);
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) return result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, false);
  return ESP_OK;
}

esp_err_t WebConfig::save_printer_animations(bool enabled) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  candidate.printer_animations_enabled = enabled;
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) return result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, false);
  return ESP_OK;
}

esp_err_t WebConfig::save_last_auto_rotation(int degrees) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
    return ESP_ERR_INVALID_ARG;
  }
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  candidate.last_auto_rotation = static_cast<std::uint16_t>(degrees);
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) return result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, false);
  return ESP_OK;
}

esp_err_t WebConfig::root_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_root(request);
}

esp_err_t WebConfig::world_map_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_world_map(request);
}

esp_err_t WebConfig::localizations_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_localizations(request);
}

esp_err_t WebConfig::reactions_script_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_reactions_script(request);
}

esp_err_t WebConfig::reaction_set_preview_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_reaction_set_preview(request);
}

esp_err_t WebConfig::health_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_health(request);
}

esp_err_t WebConfig::device_info_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_device_info(request);
}

esp_err_t WebConfig::brand_logos_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_brand_logos(request);
}

esp_err_t WebConfig::wifi_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->save_wifi(request);
}

esp_err_t WebConfig::wifi_scan_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_wifi_scan(request);
}

esp_err_t WebConfig::printer_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->save_printer(request);
}

esp_err_t WebConfig::printers_get_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_printers(request);
}

esp_err_t WebConfig::printers_manage_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->manage_printer(request);
}

esp_err_t WebConfig::printer_discovery_start_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->start_printer_discovery(request);
}

esp_err_t WebConfig::printer_discovery_status_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_printer_discovery(request);
}

esp_err_t WebConfig::printer_discovery_cancel_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->cancel_printer_discovery(request);
}
esp_err_t WebConfig::update_status_entry(httpd_req_t* request) { return static_cast<WebConfig*>(request->user_ctx)->serve_update_status(request); }
esp_err_t WebConfig::update_check_entry(httpd_req_t* request) { return static_cast<WebConfig*>(request->user_ctx)->request_update_check(request); }
esp_err_t WebConfig::update_install_entry(httpd_req_t* request) { return static_cast<WebConfig*>(request->user_ctx)->request_update_install(request); }
esp_err_t WebConfig::update_upload_entry(httpd_req_t* request) { return static_cast<WebConfig*>(request->user_ctx)->upload_update(request); }
esp_err_t WebConfig::update_url_entry(httpd_req_t* request) { return static_cast<WebConfig*>(request->user_ctx)->install_update_url(request); }

esp_err_t WebConfig::settings_get_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_settings(request);
}

esp_err_t WebConfig::settings_post_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->save_settings(request);
}

esp_err_t WebConfig::audio_test_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->test_audio(request);
}

esp_err_t WebConfig::reactions_get_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_reactions(request);
}

esp_err_t WebConfig::reactions_set_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->select_reaction_set(request);
}

esp_err_t WebConfig::reactions_set_cancel_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->cancel_reaction_set(request);
}

esp_err_t WebConfig::reactions_event_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->manage_reaction_event(request);
}

esp_err_t WebConfig::reactions_upload_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->upload_reaction_gif(request);
}

esp_err_t WebConfig::reactions_gif_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_reaction_gif(request);
}

esp_err_t WebConfig::moonraker_check_start_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->start_moonraker_check(request);
}

esp_err_t WebConfig::moonraker_check_status_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_moonraker_check_status(request);
}

esp_err_t WebConfig::compatibility_start_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->start_compatibility_probe(request);
}

esp_err_t WebConfig::compatibility_status_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_compatibility_status(request);
}

esp_err_t WebConfig::compatibility_report_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_compatibility_report(request);
}

esp_err_t WebConfig::compatibility_cancel_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->cancel_compatibility_probe(request);
}

esp_err_t WebConfig::captive_entry(httpd_req_t* request) {
  return static_cast<WebConfig*>(request->user_ctx)->serve_captive_request(request);
}

void WebConfig::restart_entry(void*) { esp_restart(); }

esp_err_t WebConfig::serve_root(httpd_req_t* request) const {
  const std::string_view page = web_config_page();
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, page.data(), page.size());
}

esp_err_t WebConfig::serve_captive_request(httpd_req_t* request) const {
  if (network_ != nullptr && network_->status().recovery_ap_active) {
    return serve_root(request);
  }
  return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
}

esp_err_t WebConfig::serve_world_map(httpd_req_t* request) const {
  const std::string_view map = world_map_svg();
  httpd_resp_set_type(request, "image/svg+xml");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, map.data(), map.size());
}

esp_err_t WebConfig::serve_localizations(httpd_req_t* request) const {
  const std::string_view script = web_localizations_script();
  httpd_resp_set_type(request, "application/javascript; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, script.data(), script.size());
}

esp_err_t WebConfig::serve_reactions_script(httpd_req_t* request) const {
  const std::string_view script = reactions_script();
  httpd_resp_set_type(request, "application/javascript; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, script.data(), script.size());
}

esp_err_t WebConfig::serve_reaction_set_preview(httpd_req_t* request) const {
  std::string id;
  if (!query_value(request, "id", id)) {
    return send_json(request, "400 Bad Request", "{\"error\":\"Choose a valid reaction set.\"}");
  }
  const std::string_view preview = reaction_set_preview(id);
  if (preview.empty()) {
    return send_json(request, "404 Not Found", "{\"error\":\"Reaction set preview not found.\"}");
  }
  httpd_resp_set_type(request, "image/webp");
  httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
  return httpd_resp_send(request, preview.data(), preview.size());
}

esp_err_t WebConfig::serve_health(httpd_req_t* request) const {
  const NetworkStatus network = network_->status();
  core::DeviceSettings current;
  core::JobPhase selected_phase = core::JobPhase::unknown;
  float selected_completion = 0.0F;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    current = settings_;
    selected_phase = selected_phase_;
    selected_completion = selected_completion_;
  }
  std::string body = "{\"product\":\"PrintDeck\",\"version\":\"" PRINTDECK_VERSION
      "\",\"hardware\":\"" + std::string(kBoardVariant) +
      "\",\"display_width\":" + std::to_string(kDisplayWidth) +
      ",\"display_height\":" + std::to_string(kDisplayHeight) +
      ",\"display_round\":" + (kDisplayIsRound ? std::string("true") : std::string("false")) +
      ",\"audio_available\":" +
          (kBoardHasAudio ? std::string("true") : std::string("false")) +
      ",\"wifi_connected\":";
  body += network.station_connected ? "true" : "false";
  body += ",\"wifi_name\":";
  append_json_string(body, network.station_name);
  body += ",\"setup_access_point\":";
  body += network.recovery_ap_active ? "true" : "false";
  body += ",\"configured_printers\":" + std::to_string(current.profiles.size());
  body += ",\"theme\":";
  append_json_string(body, current.theme);
  body += ",\"brightness\":" + std::to_string(current.brightness_percent);
  body += ",\"printer_animations_enabled\":";
  body += current.printer_animations_enabled ? "true" : "false";
  body += ",\"reaction_progress_bar_enabled\":";
  body += current.reaction_progress_bar_enabled ? "true" : "false";
  body += ",\"reaction_progress_percent_enabled\":";
  body += current.reaction_progress_percent_enabled ? "true" : "false";
  body += ",\"audio_enabled\":";
  body += kBoardHasAudio && current.audio_enabled ? "true" : "false";
  body += ",\"audio_volume\":" +
          std::to_string(kBoardHasAudio ? current.audio_volume_percent : 0);
  body += ",\"audio_preset\":";
  append_json_string(body, current.audio_preset);
  body += ",\"audio_muted_events\":" +
          std::to_string(current.audio_muted_events);
  body += ",\"rotation\":";
  append_json_string(body, current.rotation);
  body += ",\"language\":";
  append_json_string(body, current.language);
  body += ",\"selected_job_phase\":";
  append_json_string(body, job_phase_id(selected_phase));
  body += ",\"selected_job_progress\":" +
          std::to_string(static_cast<int>(selected_completion + 0.5F));
  body += ",\"usb_capture\":";
  append_json_string(body, usb_developer_status());
  body.push_back('}');
  return send_json(request, "200 OK", body.c_str());
}

esp_err_t WebConfig::serve_device_info(httpd_req_t* request) const {
  esp_chip_info_t chip_info{};
  esp_chip_info(&chip_info);
  const std::size_t internal_total =
      heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const std::size_t internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const std::size_t internal_minimum_free =
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const std::size_t psram_total =
      heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const std::size_t psram_free =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  std::uint32_t flash_bytes = 0;
  if (esp_flash_get_size(nullptr, &flash_bytes) != ESP_OK) flash_bytes = 0;
  const esp_partition_t* running_partition = esp_ota_get_running_partition();
  const esp_partition_t* assets_partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
  wifi_ap_record_t access_point{};
  const bool wifi_details_available = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
  const std::string_view reset_reason = reset_reason_name(esp_reset_reason());

  constexpr std::size_t kResponseBytes = 768;
  char* raw = static_cast<char*>(
      heap_caps_malloc(kResponseBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (raw == nullptr) {
    return send_json(request, "503 Service Unavailable",
                     "{\"error\":\"Device information is temporarily unavailable.\"}");
  }
  std::unique_ptr<char, decltype(&heap_caps_free)> body(raw, heap_caps_free);
  const int length = std::snprintf(
      body.get(), kResponseBytes,
      "{\"cpu_model\":\"ESP32-S3\",\"cpu_cores\":%u,\"cpu_frequency_mhz\":%u,"
      "\"internal_ram_total_bytes\":%u,\"internal_ram_free_bytes\":%u,"
      "\"internal_ram_minimum_free_bytes\":%u,\"psram_total_bytes\":%u,"
      "\"psram_free_bytes\":%u,\"flash_total_bytes\":%u,"
      "\"firmware_partition_bytes\":%u,\"storage_total_bytes\":%u,"
      "\"storage_usage_available\":false,\"uptime_seconds\":%llu,"
      "\"reset_reason\":\"%.*s\",\"idf_version\":\"%s\","
      "\"wifi_rssi_dbm\":%d,\"wifi_channel\":%u}",
      static_cast<unsigned>(chip_info.cores),
      static_cast<unsigned>(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
      static_cast<unsigned>(internal_total), static_cast<unsigned>(internal_free),
      static_cast<unsigned>(internal_minimum_free), static_cast<unsigned>(psram_total),
      static_cast<unsigned>(psram_free), static_cast<unsigned>(flash_bytes),
      static_cast<unsigned>(running_partition == nullptr ? 0 : running_partition->size),
      static_cast<unsigned>(assets_partition == nullptr ? 0 : assets_partition->size),
      static_cast<unsigned long long>(esp_timer_get_time() / 1'000'000),
      static_cast<int>(reset_reason.size()), reset_reason.data(), esp_get_idf_version(),
      wifi_details_available ? static_cast<int>(access_point.rssi) : 0,
      wifi_details_available ? static_cast<unsigned>(access_point.primary) : 0U);
  if (length < 0 || static_cast<std::size_t>(length) >= kResponseBytes) {
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"Device information is unavailable.\"}");
  }
  httpd_resp_set_status(request, "200 OK");
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.get(), static_cast<ssize_t>(length));
}

esp_err_t WebConfig::serve_brand_logos(httpd_req_t* request) const {
  return send_json(request, "200 OK", assets::kPrinterBrandLogosWebJson);
}

esp_err_t WebConfig::serve_settings(httpd_req_t* request) const {
  core::DeviceSettings current;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    current = settings_;
  }
  std::string body = "{\"hardware\":\"" + std::string(kBoardVariant) +
      "\",\"audio_available\":" +
      (kBoardHasAudio ? std::string("true") : std::string("false")) +
      ",\"brightness\":" + std::to_string(current.brightness_percent) +
      ",\"theme\":";
  append_json_string(body, current.theme);
  body += ",\"timezone\":";
  append_json_string(body, current.timezone);
  body += ",\"language\":";
  append_json_string(body, current.language);
  body += ",\"rotation\":";
  append_json_string(body, current.rotation);
  body += ",\"printer_animations_enabled\":";
  body += current.printer_animations_enabled ? "true" : "false";
  body += ",\"reaction_progress_bar_enabled\":";
  body += current.reaction_progress_bar_enabled ? "true" : "false";
  body += ",\"reaction_progress_percent_enabled\":";
  body += current.reaction_progress_percent_enabled ? "true" : "false";
  body += ",\"audio_enabled\":";
  body += kBoardHasAudio && current.audio_enabled ? "true" : "false";
  body += ",\"audio_volume\":" +
          std::to_string(kBoardHasAudio ? current.audio_volume_percent : 0);
  body += ",\"audio_preset\":";
  append_json_string(body, current.audio_preset);
  body += ",\"audio_muted_events\":" +
          std::to_string(current.audio_muted_events);
  body += ",\"printer_list_poll_s\":" +
          std::to_string(current.inactive_printer_poll_interval_s);
  body += ",\"camera_mode\":";
  append_json_string(body, current.camera_mode);
  body += ",\"camera_snapshot_fps\":" +
          std::to_string(current.camera_snapshot_fps);
  body += ",\"dim_enabled\":";
  body += current.display_power.dim_enabled ? "true" : "false";
  body += ",\"dim_brightness\":" +
          std::to_string(current.display_power.dim_brightness_percent);
  body += ",\"off_enabled\":";
  body += current.display_power.screen_off_enabled ? "true" : "false";
  body += ",\"dim_idle\":" + std::to_string(current.display_power.dim_timeout_idle_s);
  body += ",\"dim_active\":" + std::to_string(current.display_power.dim_timeout_active_s);
  body += ",\"off_idle\":" + std::to_string(current.display_power.off_timeout_idle_s);
  body += ",\"off_active\":" + std::to_string(current.display_power.off_timeout_active_s);
  body += ",\"usb_power_save\":";
  body += current.display_power.usb_power_save_enabled ? "true" : "false";
  body += ",\"wake_on_orientation_change\":";
  body += current.display_power.wake_on_orientation_change ? "true" : "false";
  body += ",\"custom_theme\":{";
  body += "\"printing\":" + std::to_string(current.custom_theme.printing);
  body += ",\"done\":" + std::to_string(current.custom_theme.done);
  body += ",\"error\":" + std::to_string(current.custom_theme.error);
  body += ",\"idle\":" + std::to_string(current.custom_theme.idle);
  body += ",\"preparing\":" + std::to_string(current.custom_theme.preparing);
  body += ",\"paused\":" + std::to_string(current.custom_theme.paused);
  body += ",\"filament\":" + std::to_string(current.custom_theme.filament);
  body += ",\"setup\":" + std::to_string(current.custom_theme.setup);
  body += ",\"offline\":" + std::to_string(current.custom_theme.offline);
  body += ",\"unknown\":" + std::to_string(current.custom_theme.unknown);
  body += ",\"background\":" + std::to_string(current.custom_theme.background);
  body += ",\"preview\":" + std::to_string(current.custom_theme.preview_background) + "}";
  body += ",\"theme_catalog\":";
  append_theme_catalog(body, current.custom_theme);
  body += ",\"wifi_name\":";
  append_json_string(body, current.wifi_name);
  body += ",\"wifi_setup_active\":";
  body += network_->status().recovery_ap_active ? "true" : "false";
  body.push_back('}');
  return send_json(request, "200 OK", body.c_str());
}

esp_err_t WebConfig::test_audio(httpd_req_t* request) {
  std::string body;
  if (!receive_form(request, body)) {
    return send_json(request, "413 Payload Too Large",
                     "{\"error\":\"The sound test request is too long.\"}");
  }
  std::string preset;
  std::string event;
  std::string volume_text;
  int volume = 0;
  const bool valid_preset =
      form_value(body, "preset", preset) && core::supported_audio_preset(preset);
  const bool valid_event = form_value(body, "event", event) &&
      (event == "startup" || event == "navigation" || event == "orientation" ||
       event == "print_started" || event == "progress_25" ||
       event == "progress_50" || event == "progress_75" ||
       event == "print_paused" ||
       event == "print_finished" || event == "print_error" ||
       event == "hms_alert" || event == "filament_attention" ||
       event == "shutdown_countdown" || event == "shutdown");
  const bool valid_volume = form_value(body, "volume", volume_text) &&
                            parse_int(volume_text, volume) && volume > 0 && volume <= 100;
  if (!valid_preset || !valid_event || !valid_volume) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Choose a valid sound preset and event.\"}");
  }
  AudioTestCallback callback = nullptr;
  void* context = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    callback = audio_test_callback_;
    context = audio_test_context_;
  }
  if (callback == nullptr) {
    return send_json(request, "503 Service Unavailable",
                     "{\"error\":\"Sound testing is unavailable.\"}");
  }
  if (!callback(context, preset, event, volume)) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Turn up the sound volume and try again.\"}");
  }
  return send_json(request, "200 OK", "{\"played\":true}");
}

esp_err_t WebConfig::serve_reactions(httpd_req_t* request) const {
  if (reaction_assets_ == nullptr) {
    return send_json(request, "503 Service Unavailable",
                     "{\"error\":\"Reaction storage is unavailable.\"}");
  }
  const ReactionAssetSnapshot state = reaction_assets_->snapshot();
  std::string body = "{\"schema\":1,\"available\":";
  body += state.available ? "true" : "false";
  body += ",\"busy\":";
  body += state.busy ? "true" : "false";
  body += ",\"cancellable\":";
  body += state.cancellable ? "true" : "false";
  body += ",\"progress\":" + std::to_string(state.progress_percent);
  body += ",\"detail\":";
  append_json_string(body, state.detail);
  body += ",\"active_set\":{\"id\":";
  append_json_string(body, state.active_set_id);
  body += ",\"name\":";
  append_json_string(body, state.active_set_name);
  body += ",\"version\":";
  append_json_string(body, state.active_set_version);
  body += "},\"installing_set\":{\"id\":";
  append_json_string(body, state.installing_set_id);
  body += ",\"name\":";
  append_json_string(body, state.installing_set_name);
  body += "},\"limits\":{\"maximum_dimension\":" +
          std::to_string(kDisplayWidth) + ",\"maximum_file_bytes\":" +
          std::to_string(state.maximum_file_bytes) +
          ",\"maximum_set_bytes\":" + std::to_string(state.maximum_set_bytes) +
          ",\"maximum_custom_bytes\":" +
          std::to_string(state.maximum_custom_bytes) +
          ",\"active_bytes\":" + std::to_string(state.active_bytes) +
          ",\"storage_total\":" + std::to_string(state.storage_total) +
          ",\"storage_used\":" + std::to_string(state.storage_used) + "}";
  body += ",\"sets\":[";
  bool first = true;
  for (const auto& set : ReactionAssetService::sets()) {
    if (!first) body.push_back(',');
    first = false;
    body += "{\"id\":";
    append_json_string(body, set.id);
    body += ",\"name\":";
    append_json_string(body, set.name);
    body += ",\"version\":";
    append_json_string(body, set.version);
    body += ",\"active\":";
    body += state.active_set_id == set.id && state.active_set_version == set.version
                ? "true"
                : "false";
    body += "}";
  }
  body += "],\"events\":[";
  first = true;
  for (const auto& event : core::reaction_events()) {
    if (!first) body.push_back(',');
    first = false;
    body += "{\"id\":";
    append_json_string(body, event.id);
    body += ",\"label\":";
    append_json_string(body, event.label);
    body += ",\"enabled\":";
    body += reaction_assets_->event_enabled(event.id) ? "true" : "false";
    body += ",\"custom\":";
    body += reaction_assets_->custom_override(event.id) ? "true" : "false";
    body += "}";
  }
  body += "],\"generation\":" + std::to_string(state.generation) + "}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t WebConfig::select_reaction_set(httpd_req_t* request) {
  std::string body;
  std::string id;
  if (!receive_form(request, body) || !form_value(body, "id", id) ||
      reaction_assets_ == nullptr) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Choose a valid reaction set.\"}");
  }
  if (!reaction_assets_->request_set(id)) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Another reaction change is already in progress.\"}");
  }
  return send_json(request, "202 Accepted", "{\"started\":true}");
}

esp_err_t WebConfig::cancel_reaction_set(httpd_req_t* request) {
  if (reaction_assets_ == nullptr) {
    return send_json(request, "503 Service Unavailable",
                     "{\"error\":\"Reaction storage is unavailable.\"}");
  }
  if (!reaction_assets_->cancel_set()) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"The reaction set can no longer be cancelled.\"}");
  }
  return send_json(request, "202 Accepted", "{\"cancelling\":true}");
}

esp_err_t WebConfig::manage_reaction_event(httpd_req_t* request) {
  std::string body;
  std::string event;
  std::string action;
  if (!receive_form(request, body) || !form_value(body, "event", event) ||
      !form_value(body, "action", action) || reaction_assets_ == nullptr) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Choose a valid reaction.\"}");
  }
  if (reaction_assets_->snapshot().busy) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Another reaction change is already in progress.\"}");
  }
  esp_err_t result = ESP_ERR_INVALID_ARG;
  if (action == "reset") {
    result = reaction_assets_->reset_custom(event);
  } else if (action == "enabled") {
    std::string value;
    if (form_value(body, "value", value) && (value == "0" || value == "1")) {
      result = reaction_assets_->set_event_enabled(event, value == "1");
    }
  }
  if (result != ESP_OK) {
    if (result == ESP_ERR_INVALID_STATE) {
      return send_json(request, "409 Conflict",
                       "{\"error\":\"Another reaction change is already in progress.\"}");
    }
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"The reaction change could not be saved.\"}");
  }
  return send_json(request, "200 OK", "{\"saved\":true}");
}

esp_err_t WebConfig::upload_reaction_gif(httpd_req_t* request) {
  std::string event;
  if (!query_value(request, "event", event) || core::reaction_event(event) == nullptr ||
      reaction_assets_ == nullptr) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Choose a valid reaction.\"}");
  }
  const ReactionAssetSnapshot state = reaction_assets_->snapshot();
  if (state.busy) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Another reaction change is already in progress.\"}");
  }
  if (request->content_len <= 0 ||
      static_cast<std::size_t>(request->content_len) > state.maximum_file_bytes) {
    return send_json(request, "413 Payload Too Large",
                     "{\"error\":\"The GIF is larger than this device allows.\"}");
  }
  const std::size_t total = static_cast<std::size_t>(request->content_len);
  auto* raw = static_cast<std::uint8_t*>(
      heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (raw == nullptr) {
    return send_json(request, "503 Service Unavailable",
                     "{\"error\":\"There is not enough memory to receive this GIF.\"}");
  }
  std::unique_ptr<std::uint8_t, decltype(&heap_caps_free)> bytes(raw, heap_caps_free);
  std::size_t received = 0;
  while (received < total) {
    const int count = httpd_req_recv(
        request, reinterpret_cast<char*>(bytes.get() + received), total - received);
    if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (count <= 0) {
      return send_json(request, "400 Bad Request",
                       "{\"error\":\"The GIF upload was incomplete.\"}");
    }
    received += static_cast<std::size_t>(count);
  }
  const esp_err_t result = reaction_assets_->install_custom(
      event, std::span<const std::uint8_t>(bytes.get(), total));
  if (result != ESP_OK) {
    if (result == ESP_ERR_INVALID_STATE) {
      return send_json(request, "409 Conflict",
                       "{\"error\":\"Another reaction change is already in progress.\"}");
    }
    return send_json(request, result == ESP_ERR_NO_MEM ? "413 Payload Too Large"
                                                       : "400 Bad Request",
                     "{\"error\":\"The GIF did not pass device validation.\"}");
  }
  return send_json(request, "200 OK", "{\"saved\":true}");
}

esp_err_t WebConfig::serve_reaction_gif(httpd_req_t* request) const {
  std::string event;
  std::string scope;
  if (!query_value(request, "event", event) || reaction_assets_ == nullptr) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Choose a valid reaction.\"}");
  }
  query_value(request, "scope", scope);
  const std::string path = scope == "set" ? reaction_assets_->set_vfs_path(event)
                                           : reaction_assets_->preview_vfs_path(event);
  if (path.empty()) {
    return send_json(request, "404 Not Found",
                     "{\"error\":\"No GIF is available for this reaction.\"}");
  }
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return send_json(request, "404 Not Found",
                     "{\"error\":\"No GIF is available for this reaction.\"}");
  }
  httpd_resp_set_type(request, "image/gif");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  std::array<char, 1024> buffer{};
  esp_err_t result = ESP_OK;
  while (true) {
    const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), file);
    if (count > 0 && httpd_resp_send_chunk(request, buffer.data(), count) != ESP_OK) {
      result = ESP_FAIL;
      break;
    }
    if (count < buffer.size()) {
      if (std::ferror(file) != 0) result = ESP_FAIL;
      break;
    }
  }
  std::fclose(file);
  if (result == ESP_OK) result = httpd_resp_send_chunk(request, nullptr, 0);
  return result;
}

esp_err_t WebConfig::save_settings(httpd_req_t* request) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  std::string body;
  if (!receive_form(request, body)) {
    return send_json(request, "413 Payload Too Large",
                     "{\"error\":\"The entered values are too long. Please shorten them and try again.\"}");
  }
  std::string brightness_text;
  std::string printer_animations_enabled_text;
  std::string reaction_progress_bar_enabled_text;
  std::string reaction_progress_percent_enabled_text;
  std::string audio_enabled_text;
  std::string audio_volume_text;
  std::string audio_preset;
  std::string audio_muted_events_text;
  std::string printer_list_poll_text;
  std::string camera_mode;
  std::string camera_snapshot_fps_text;
  std::string dim_enabled_text;
  std::string dim_brightness_text;
  std::string off_enabled_text;
  std::string dim_idle_text;
  std::string dim_active_text;
  std::string off_idle_text;
  std::string off_active_text;
  std::string usb_power_save_text;
  std::string wake_on_orientation_change_text;
  std::string theme;
  std::string timezone;
  std::string language;
  std::string rotation;
  int brightness = 0;
  int audio_volume = 0;
  int audio_muted_events = 0;
  int printer_list_poll = 0;
  int camera_snapshot_fps = 0;
  int dim_brightness = 0;
  int dim_idle = 0;
  int dim_active = 0;
  int off_idle = 0;
  int off_active = 0;
  if (!form_value(body, "brightness", brightness_text) ||
      !parse_int(brightness_text, brightness) ||
      !form_value(body, "printer_animations_enabled", printer_animations_enabled_text) ||
      (printer_animations_enabled_text != "0" && printer_animations_enabled_text != "1") ||
      !form_value(body, "reaction_progress_bar_enabled", reaction_progress_bar_enabled_text) ||
      (reaction_progress_bar_enabled_text != "0" &&
       reaction_progress_bar_enabled_text != "1") ||
      !form_value(body, "reaction_progress_percent_enabled",
                  reaction_progress_percent_enabled_text) ||
      (reaction_progress_percent_enabled_text != "0" &&
       reaction_progress_percent_enabled_text != "1") ||
      !form_value(body, "theme", theme) ||
      !form_value(body, "timezone", timezone) || !core::supported_timezone(timezone) ||
      !form_value(body, "language", language) || !core::supported_language(language) ||
      !form_value(body, "rotation", rotation) ||
      !form_value(body, "audio_enabled", audio_enabled_text) ||
      (audio_enabled_text != "0" && audio_enabled_text != "1") ||
      !form_value(body, "audio_volume", audio_volume_text) ||
      !parse_int(audio_volume_text, audio_volume) ||
      !form_value(body, "audio_preset", audio_preset) ||
      !core::supported_audio_preset(audio_preset) ||
      !form_value(body, "audio_muted_events", audio_muted_events_text) ||
      !parse_int(audio_muted_events_text, audio_muted_events) ||
      audio_muted_events < 0 ||
      audio_muted_events > core::kAudioEventMuteMask ||
      !form_value(body, "printer_list_poll_s", printer_list_poll_text) ||
      !parse_int(printer_list_poll_text, printer_list_poll) ||
      !form_value(body, "camera_mode", camera_mode) ||
      (camera_mode != "snapshots" && camera_mode != "live") ||
      !form_value(body, "camera_snapshot_fps", camera_snapshot_fps_text) ||
      !parse_int(camera_snapshot_fps_text, camera_snapshot_fps) ||
      (camera_snapshot_fps != 1 && camera_snapshot_fps != 2 && camera_snapshot_fps != 5) ||
      !form_value(body, "dim_enabled", dim_enabled_text) ||
      (dim_enabled_text != "0" && dim_enabled_text != "1") ||
      !form_value(body, "dim_brightness", dim_brightness_text) ||
      !parse_int(dim_brightness_text, dim_brightness) ||
      !form_value(body, "off_enabled", off_enabled_text) ||
      (off_enabled_text != "0" && off_enabled_text != "1") ||
      !form_value(body, "dim_idle", dim_idle_text) || !parse_int(dim_idle_text, dim_idle) ||
      !form_value(body, "dim_active", dim_active_text) ||
      !parse_int(dim_active_text, dim_active) ||
      !form_value(body, "off_idle", off_idle_text) || !parse_int(off_idle_text, off_idle) ||
      !form_value(body, "off_active", off_active_text) ||
      !parse_int(off_active_text, off_active) ||
      !form_value(body, "usb_power_save", usb_power_save_text) ||
      (usb_power_save_text != "0" && usb_power_save_text != "1") ||
      !form_value(body, "wake_on_orientation_change", wake_on_orientation_change_text) ||
      (wake_on_orientation_change_text != "0" &&
       wake_on_orientation_change_text != "1")) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Some device settings could not be read. Please review the form and try again.\"}");
  }
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  const bool restart_required = candidate.timezone != timezone;
  candidate.brightness_percent = static_cast<std::uint8_t>(std::clamp(brightness, 5, 100));
  candidate.printer_animations_enabled = printer_animations_enabled_text == "1";
  candidate.reaction_progress_bar_enabled = reaction_progress_bar_enabled_text == "1";
  candidate.reaction_progress_percent_enabled =
      reaction_progress_percent_enabled_text == "1";
  candidate.audio_enabled = kBoardHasAudio && audio_enabled_text == "1";
  candidate.audio_volume_percent = kBoardHasAudio
      ? static_cast<std::uint8_t>(std::clamp(audio_volume, 0, 100))
      : 0;
  candidate.audio_preset = std::move(audio_preset);
  candidate.audio_muted_events = static_cast<std::uint16_t>(audio_muted_events);
  candidate.inactive_printer_poll_interval_s =
      printer_list_poll < 0 ? UINT32_MAX : static_cast<std::uint32_t>(printer_list_poll);
  candidate.camera_mode = std::move(camera_mode);
  candidate.camera_snapshot_fps = static_cast<std::uint8_t>(camera_snapshot_fps);
  candidate.display_power.dim_enabled = dim_enabled_text == "1";
  candidate.display_power.dim_brightness_percent =
      static_cast<std::uint8_t>(std::clamp(dim_brightness, 0, 100));
  candidate.display_power.screen_off_enabled = off_enabled_text == "1";
  candidate.display_power.dim_timeout_idle_s = static_cast<std::uint32_t>(dim_idle);
  candidate.display_power.dim_timeout_active_s = static_cast<std::uint32_t>(dim_active);
  candidate.display_power.off_timeout_idle_s = static_cast<std::uint32_t>(off_idle);
  candidate.display_power.off_timeout_active_s = static_cast<std::uint32_t>(off_active);
  candidate.display_power.usb_power_save_enabled = usb_power_save_text == "1";
  candidate.display_power.wake_on_orientation_change =
      wake_on_orientation_change_text == "1";
  candidate.theme = std::move(theme);
  candidate.timezone = std::move(timezone);
  candidate.language = std::move(language);
  candidate.rotation = std::move(rotation);
  struct ColorField {
    const char* name;
    std::uint32_t* destination;
  };
  const ColorField colors[] = {
      {"color_printing", &candidate.custom_theme.printing},
      {"color_done", &candidate.custom_theme.done},
      {"color_error", &candidate.custom_theme.error},
      {"color_idle", &candidate.custom_theme.idle},
      {"color_preparing", &candidate.custom_theme.preparing},
      {"color_paused", &candidate.custom_theme.paused},
      {"color_filament", &candidate.custom_theme.filament},
      {"color_setup", &candidate.custom_theme.setup},
      {"color_offline", &candidate.custom_theme.offline},
      {"color_unknown", &candidate.custom_theme.unknown},
      {"color_background", &candidate.custom_theme.background},
      {"color_preview", &candidate.custom_theme.preview_background},
  };
  for (const ColorField& color : colors) {
    std::string text;
    if (form_value(body, color.name, text) && !parse_color(text, *color.destination)) {
      return send_json(request, "400 Bad Request",
                       "{\"error\":\"One of the palette colors is not valid. Please choose it again.\"}");
    }
  }
  if (!core::validate(candidate).empty()) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"One or more device settings need attention.\"}");
  }
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) {
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"PrintDeck could not save these changes. Please try again.\"}");
  }
  SettingsChangedCallback callback = nullptr;
  void* callback_context = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
    callback = settings_changed_callback_;
    callback_context = settings_changed_context_;
  }
  if (callback != nullptr) callback(callback_context, candidate, true);
  const esp_err_t response = send_json(
      request, "200 OK", restart_required ? "{\"saved\":true,\"restart_required\":true}"
                                           : "{\"saved\":true,\"restart_required\":false}");
  if (restart_required) schedule_restart();
  return response;
}

esp_err_t WebConfig::start_moonraker_check(httpd_req_t* request) {
  if (!network_->status().station_connected) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Connect PrintDeck to Wi-Fi before testing a printer.\"}");
  }
  std::string body;
  if (!receive_form(request, body)) {
    return send_json(request, "413 Payload Too Large",
                     "{\"error\":\"The connection details are too long. Please review them and try again.\"}");
  }
  std::string profile_id_text;
  std::string endpoint;
  std::string api_key;
  std::uint32_t profile_id = 0;
  if (!form_value(body, "profile_id", profile_id_text) ||
      !parse_id(profile_id_text, profile_id) || !form_value(body, "endpoint", endpoint) ||
      !form_value(body, "api_key", api_key) || endpoint.empty() || endpoint.size() > 128 ||
      api_key.size() > 128 ||
      !core::is_local_printer_endpoint(endpoint, core::PrinterProtocol::moonraker)) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Enter a valid local printer address, such as 192.168.1.50:7125.\"}");
  }

  core::PrinterProfile profile;
  profile.id = profile_id;
  profile.protocol = core::PrinterProtocol::moonraker;
  profile.endpoint = std::move(endpoint);
  profile.api_key = std::move(api_key);
  if (profile.api_key.empty() && profile_id != 0) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = std::find_if(settings_.profiles.begin(), settings_.profiles.end(),
        [profile_id](const core::PrinterProfile& value) { return value.id == profile_id; });
    if (existing != settings_.profiles.end() &&
        existing->protocol == core::PrinterProtocol::moonraker) {
      profile.api_key = existing->api_key;
    }
  }

  const esp_err_t result = moonraker_probe_->start(std::move(profile));
  if (result == ESP_ERR_INVALID_STATE) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"A Moonraker connection test is already in progress.\"}");
  }
  if (result != ESP_OK) {
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"The Moonraker connection test could not start. Please try again.\"}");
  }
  return send_json(request, "202 Accepted", "{\"started\":true}");
}

esp_err_t WebConfig::serve_moonraker_check_status(httpd_req_t* request) const {
  const MoonrakerProbeSnapshot snapshot = moonraker_probe_->snapshot();
  const char* state = "idle";
  switch (snapshot.state) {
    case MoonrakerProbeState::connecting: state = "connecting"; break;
    case MoonrakerProbeState::ready: state = "ready"; break;
    case MoonrakerProbeState::authorization_required: state = "authorization_required"; break;
    case MoonrakerProbeState::unavailable: state = "unavailable"; break;
    case MoonrakerProbeState::idle: break;
  }
  std::string body = "{\"state\":\"";
  body += state;
  body += "\",\"running\":";
  body += snapshot.running ? "true" : "false";
  body += ",\"progress\":" + std::to_string(snapshot.progress_percent);
  body += ",\"detail\":";
  append_json_string(body, localized(snapshot.detail));
  body += ",\"version\":";
  append_json_string(body, snapshot.version);
  body += ",\"klipper_state\":";
  append_json_string(body, snapshot.klipper_state);
  body += ",\"manufacturer\":";
  append_json_string(body, snapshot.manufacturer);
  body += ",\"model\":";
  append_json_string(body, snapshot.model);
  body += ",\"brand\":";
  append_json_string(body, snapshot.brand);
  body += ",\"evidence\":";
  append_json_string(body, snapshot.evidence);
  body.push_back('}');
  return send_json(request, "200 OK", body.c_str());
}

esp_err_t WebConfig::serve_wifi_scan(httpd_req_t* request) {
  const std::vector<std::string> networks = network_->scan_visible_networks();
  std::string body = "{\"networks\":[";
  bool first = true;
  for (const std::string& name : networks) {
    if (!first) body.push_back(',');
    first = false;
    body.push_back('"');
    for (const unsigned char byte : name) {
      if (byte == '"' || byte == '\\') {
        body.push_back('\\');
        body.push_back(static_cast<char>(byte));
      } else if (byte >= 0x20) {
        body.push_back(static_cast<char>(byte));
      }
    }
    body.push_back('"');
  }
  body += "]}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t WebConfig::serve_printers(httpd_req_t* request) const {
  core::DeviceSettings current;
  std::uint32_t selected_status_profile = 0;
  core::LinkState selected_link = core::LinkState::stopped;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    current = settings_;
    selected_status_profile = selected_status_profile_;
    selected_link = selected_link_;
  }
  const InactivePrinterSnapshot inactive = inactive_printer_poller_ != nullptr
      ? inactive_printer_poller_->snapshot() : InactivePrinterSnapshot{};
  std::string body = "{\"printers\":[";
  bool first = true;
  for (const auto& profile : current.profiles) {
    if (!first) body.push_back(',');
    first = false;
    body += "{\"id\":" + std::to_string(profile.id) + ",\"selected\":";
    body += profile.id == current.selected_profile ? "true" : "false";
    core::PrinterReachability reachability = core::PrinterReachability::unknown;
    if (profile.id == current.selected_profile && profile.id == selected_status_profile) {
      if (selected_link == core::LinkState::online) {
        reachability = core::PrinterReachability::online;
      } else if (selected_link == core::LinkState::failed) {
        reachability = core::PrinterReachability::offline;
      }
    } else {
      const auto status = std::find_if(
          inactive.printers.begin(), inactive.printers.end(),
          [&profile](const InactivePrinterStatus& value) {
            return value.profile_id == profile.id && value.available;
          });
      if (status != inactive.printers.end()) {
        reachability = status->connected ? core::PrinterReachability::online
                                         : core::PrinterReachability::offline;
      }
    }
    body += ",\"reachability\":\"";
    body += reachability == core::PrinterReachability::online ? "online"
          : reachability == core::PrinterReachability::offline ? "offline" : "unknown";
    body += "\"";
    body += ",\"protocol\":\"";
    body += core::printer_driver(profile.protocol).id;
    body += "\",\"name\":";
    append_json_string(body, profile.display_name);
    body += ",\"endpoint\":";
    append_json_string(body, profile.endpoint);
    body += ",\"serial\":";
    append_json_string(body, profile.serial);
    body += ",\"manufacturer\":";
    append_json_string(body, profile.manufacturer);
    body += ",\"model\":";
    append_json_string(body, profile.model);
    body += ",\"brand\":";
    append_json_string(body, profile.brand);
    body.push_back('}');
  }
  body += "]}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t WebConfig::serve_printer_discovery(httpd_req_t* request,
                                             std::optional<bool> started) const {
  const PrinterDiscoverySnapshot snapshot = printer_discovery_->snapshot();
  const char* state = "idle";
  if (snapshot.state == PrinterDiscoveryState::scanning) state = "scanning";
  else if (snapshot.state == PrinterDiscoveryState::complete) state = "complete";
  else if (snapshot.state == PrinterDiscoveryState::failed) state = "failed";
  std::string body = "{\"state\":\"";
  body += state;
  body += "\",\"running\":";
  body += snapshot.state == PrinterDiscoveryState::scanning ? "true" : "false";
  body += ",\"scan_id\":" + std::to_string(snapshot.scan_id);
  if (started.has_value()) {
    body += ",\"started\":";
    body += *started ? "true" : "false";
  }
  body += ",\"progress\":" + std::to_string(snapshot.progress_percent) + ",\"network\":";
  append_json_string(body, snapshot.network_name);
  body += ",\"detail\":";
  append_json_string(body, localized(snapshot.detail));
  body += ",\"printers\":[";
  bool first = true;
  for (const auto& printer : snapshot.printers) {
    if (!first) body.push_back(',');
    first = false;
    body += "{\"protocol\":\"";
    body += core::printer_driver(printer.protocol).id;
    body += "\",\"name\":";
    append_json_string(body, printer.name);
    body += ",\"model\":";
    append_json_string(body, printer.model);
    body += ",\"host\":";
    append_json_string(body, printer.host);
    body += ",\"serial\":";
    append_json_string(body, printer.serial);
    body += ",\"port\":" + std::to_string(printer.port) + "}";
  }
  body += "]}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t WebConfig::start_printer_discovery(httpd_req_t* request) {
  const NetworkStatus network = network_->status();
  if (!network.station_connected) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Connect PrintDeck to the same Wi-Fi network as your printers before searching.\"}");
  }
  core::DeviceSettings current;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    current = settings_;
  }
  const esp_err_t result = printer_discovery_->start(network, current);
  if (result == ESP_ERR_INVALID_STATE) {
    if (printer_discovery_->snapshot().state == PrinterDiscoveryState::scanning) {
      return serve_printer_discovery(request, false);
    }
    return send_json(request, "503 Service Unavailable",
                     "{\"error\":\"PrintDeck could not start the network search. Please try again.\"}");
  }
  if (result != ESP_OK) {
    return send_json(request, "503 Service Unavailable",
                     "{\"error\":\"PrintDeck could not start the network search. Please try again.\"}");
  }
  httpd_resp_set_status(request, "202 Accepted");
  return serve_printer_discovery(request, true);
}

esp_err_t WebConfig::cancel_printer_discovery(httpd_req_t* request) {
  std::string body;
  std::string scan_id_text;
  std::uint32_t scan_id = 0;
  if (!receive_form(request, body) || !form_value(body, "scan_id", scan_id_text) ||
      !parse_id(scan_id_text, scan_id) || scan_id == 0) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"The current printer search could not be stopped. Please try again.\"}");
  }
  printer_discovery_->cancel(scan_id);
  return serve_printer_discovery(request);
}

esp_err_t WebConfig::serve_update_status(httpd_req_t* request) const {
  const FirmwareUpdateSnapshot snapshot = firmware_update_->snapshot();
  const char* state = "idle";
  switch (snapshot.state) {
    case FirmwareUpdateState::checking: state = "checking"; break;
    case FirmwareUpdateState::current: state = "current"; break;
    case FirmwareUpdateState::unavailable: state = "unavailable"; break;
    case FirmwareUpdateState::available: state = "available"; break;
    case FirmwareUpdateState::failed: state = "failed"; break;
    case FirmwareUpdateState::downloading: state = "downloading"; break;
    case FirmwareUpdateState::rebooting: state = "rebooting"; break;
    case FirmwareUpdateState::idle: break;
  }
  std::string body = "{\"state\":\""; body += state; body += "\",\"busy\":";
  body += snapshot.busy ? "true" : "false";
  body += ",\"available\":"; body += snapshot.update_available ? "true" : "false";
  body += ",\"progress\":" + std::to_string(snapshot.progress_percent) + ",\"current\":";
  append_json_string(body, snapshot.current_version); body += ",\"latest\":";
  append_json_string(body, snapshot.latest_version); body += ",\"detail\":";
  append_json_string(body, localized(snapshot.detail)); body.push_back('}');
  httpd_resp_set_type(request, "application/json"); httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t WebConfig::request_update_check(httpd_req_t* request) {
  if (!network_->status().station_connected) return send_json(request, "409 Conflict", "{\"error\":\"Connect PrintDeck to Wi-Fi before checking for updates.\"}");
  if (!firmware_update_->request_check()) return send_json(request, "409 Conflict", "{\"error\":\"An update task is already in progress.\"}");
  httpd_resp_set_status(request, "202 Accepted"); return serve_update_status(request);
}

esp_err_t WebConfig::request_update_install(httpd_req_t* request) {
  if (!firmware_update_->request_install()) return send_json(request, "409 Conflict", "{\"error\":\"No validated PrintDeck update is ready to install.\"}");
  httpd_resp_set_status(request, "202 Accepted"); return serve_update_status(request);
}

esp_err_t WebConfig::install_update_url(httpd_req_t* request) {
  if (!network_->status().station_connected) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Connect PrintDeck to Wi-Fi before installing firmware from a URL.\"}");
  }
  std::string body;
  std::string url;
  if (!receive_form(request, body) || !form_value(body, "url", url)) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Enter the HTTPS address of a PrintDeck firmware image.\"}");
  }
  if (!firmware_update_->request_url_install(std::move(url))) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Use a direct HTTPS firmware URL, or wait for the current update task to finish.\"}");
  }
  httpd_resp_set_status(request, "202 Accepted");
  return serve_update_status(request);
}

esp_err_t WebConfig::upload_update(httpd_req_t* request) {
  const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
  const int total = request->content_len;
  if (partition == nullptr) {
    return send_json(request, "503 Service Unavailable",
                     "{\"error\":\"This firmware layout has no OTA slot.\"}");
  }
  if (total <= 0 || static_cast<std::size_t>(total) > partition->size) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Choose a valid PrintDeck .bin file that fits the update slot.\"}");
  }
  if (!firmware_update_->begin_manual_install()) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Another firmware update task is already in progress.\"}");
  }

  esp_ota_handle_t handle = 0;
  esp_err_t result = esp_ota_begin(partition, static_cast<std::size_t>(total), &handle);
  if (result != ESP_OK) {
    firmware_update_->fail_manual_install("The manual firmware update could not start.");
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"PrintDeck could not open the update slot.\"}");
  }

  struct HeapCapsDeleter {
    void operator()(std::uint8_t* pointer) const { heap_caps_free(pointer); }
  };
  std::unique_ptr<std::uint8_t, HeapCapsDeleter> buffer(static_cast<std::uint8_t*>(
      heap_caps_malloc(kOtaReceiveBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  if (!buffer) {
    esp_ota_abort(handle);
    firmware_update_->fail_manual_install("The manual update ran out of working memory.");
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"PrintDeck could not reserve the update buffer.\"}");
  }

  int received = 0;
  const std::uint64_t deadline =
      static_cast<std::uint64_t>(esp_timer_get_time() / 1000ULL) + kOtaReceiveDeadlineMs;
  while (received < total) {
    const int wanted = std::min<int>(kOtaReceiveBufferBytes, total - received);
    const int count = httpd_req_recv(request, reinterpret_cast<char*>(buffer.get()), wanted);
    if (count == HTTPD_SOCK_ERR_TIMEOUT &&
        static_cast<std::uint64_t>(esp_timer_get_time() / 1000ULL) < deadline) {
      continue;
    }
    if (count <= 0) {
      esp_ota_abort(handle);
      firmware_update_->fail_manual_install("The firmware upload was interrupted.");
      return count == 0 ? ESP_OK : ESP_FAIL;
    }
    result = esp_ota_write(handle, buffer.get(), static_cast<std::size_t>(count));
    if (result != ESP_OK) {
      esp_ota_abort(handle);
      firmware_update_->fail_manual_install("The firmware image could not be written safely.");
      return send_json(request, "500 Internal Server Error",
                       "{\"error\":\"Writing the firmware image failed.\"}");
    }
    received += count;
    firmware_update_->update_manual_progress(received * 100 / total);
  }

  result = esp_ota_end(handle);
  if (result != ESP_OK) {
    firmware_update_->fail_manual_install("The uploaded file is not a valid firmware image.");
    return send_json(request, "422 Unprocessable Entity",
                     "{\"error\":\"This file did not pass firmware image validation.\"}");
  }
  result = esp_ota_set_boot_partition(partition);
  if (result != ESP_OK) {
    firmware_update_->fail_manual_install("The new firmware slot could not be selected.");
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"The firmware was written, but PrintDeck could not select it.\"}");
  }
  firmware_update_->finish_manual_install();
  const esp_err_t response = send_json(request, "200 OK", "{\"installed\":true,\"rebooting\":true}");
  schedule_restart();
  return response;
}

esp_err_t WebConfig::save_wifi(httpd_req_t* request) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  std::string body;
  if (!receive_form(request, body))
    return send_json(request, "413 Payload Too Large",
                     "{\"error\":\"The Wi-Fi details are too long. Please check the network name and password.\"}");

  std::string ssid;
  std::string password;
  std::string detected_timezone;
  std::string detected_language;
  if (!form_value(body, "ssid", ssid) || !form_value(body, "password", password)) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"The Wi-Fi details could not be read. Please enter them again.\"}");
  }

  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  const bool initial_setup = candidate.wifi_name.empty();
  candidate.wifi_name = std::move(ssid);
  if (!password.empty()) candidate.wifi_password = std::move(password);
  if (initial_setup && form_value(body, "timezone", detected_timezone) &&
      core::supported_timezone(detected_timezone)) {
    candidate.timezone = std::move(detected_timezone);
  }
  if (initial_setup && form_value(body, "language", detected_language) &&
      core::supported_language(detected_language)) {
    candidate.language = std::move(detected_language);
  }
  const auto issues = core::validate(candidate);
  if (!issues.empty()) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Please check the Wi-Fi network name and password.\"}");
  }
  const bool verify_before_save = network_->status().recovery_ap_active;
  if (verify_before_save) {
    const esp_err_t test_result =
        network_->test_station_connection(candidate.wifi_name, candidate.wifi_password);
    if (test_result == ESP_ERR_TIMEOUT) {
      return send_json(
          request, "409 Conflict",
          "{\"error\":\"PrintDeck could not join this Wi-Fi network. Check the password and try again.\"}");
    }
    if (test_result != ESP_OK) {
      ESP_LOGW(kLogTag, "Wi-Fi connection test could not run: %s",
               esp_err_to_name(test_result));
      return send_json(
          request, "503 Service Unavailable",
          "{\"error\":\"PrintDeck could not verify this Wi-Fi network. Stay connected to the PrintDeck setup network and try again.\"}");
    }
  }

  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) {
    if (verify_before_save) network_->cancel_tested_station();
    ESP_LOGE(kLogTag, "Wi-Fi settings could not be saved: %s", esp_err_to_name(result));
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"PrintDeck could not save this Wi-Fi network. Please try again.\"}");
  }
  if (verify_before_save) {
    network_->accept_tested_station(candidate.wifi_name, candidate.wifi_password);
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, true);
  const esp_err_t response = send_json(
      request, "200 OK",
      verify_before_save ? "{\"saved\":true,\"verified\":true}"
                         : "{\"saved\":true,\"verified\":false}");
  const esp_err_t restart_result = schedule_restart();
  if (restart_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Restart after saving Wi-Fi could not be scheduled: %s",
             esp_err_to_name(restart_result));
  }
  return response;
}

esp_err_t WebConfig::save_printer(httpd_req_t* request) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  std::string body;
  if (!receive_form(request, body))
    return send_json(request, "413 Payload Too Large",
                     "{\"error\":\"The printer details are too long. Please shorten them and try again.\"}");

  std::string protocol;
  std::string profile_id_text;
  std::uint32_t profile_id = 0;
  core::PrinterProfile profile;
  if (!form_value(body, "profile_id", profile_id_text) || !parse_id(profile_id_text, profile_id) ||
      !form_value(body, "protocol", protocol) || !form_value(body, "name", profile.display_name) ||
      !form_value(body, "endpoint", profile.endpoint) ||
      !form_value(body, "api_key", profile.api_key) ||
      !form_value(body, "serial", profile.serial) ||
      !form_value(body, "access_code", profile.access_code) ||
      !form_value(body, "manufacturer", profile.manufacturer) ||
      !form_value(body, "model", profile.model)) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Some printer details are missing or could not be read.\"}");
  }
  if (!core::printer_protocol_from_id(protocol, profile.protocol)) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"This printer connection type is not supported.\"}");
  }
  if (core::printer_supports(profile.protocol, core::PrinterCapability::access_code)) {
    const core::PrinterDriverDescriptor& driver = core::printer_driver(profile.protocol);
    profile.api_key.clear();
    profile.manufacturer = driver.default_manufacturer;
    profile.brand = driver.default_brand;
  }
  else {
    profile.serial.clear();
    profile.access_code.clear();
    profile.brand = canonical_brand(profile.manufacturer);
  }

  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  auto existing = std::find_if(candidate.profiles.begin(), candidate.profiles.end(),
                               [profile_id](const auto& value) { return value.id == profile_id; });
  if (profile_id != 0 && existing == candidate.profiles.end()) {
    return send_json(request, "404 Not Found",
                     "{\"error\":\"This printer is no longer in your saved list. Refresh the page and try again.\"}");
  }
  if (profile_id == 0 && candidate.profiles.size() >= core::kMaximumProfiles) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"Your printer list is full. Remove an unused printer before adding another.\"}");
  }
  if (profile_id == 0) {
    std::uint32_t next_id = 1;
    for (const auto& value : candidate.profiles) next_id = std::max(next_id, value.id + 1);
    profile.id = next_id;
    candidate.profiles.push_back(std::move(profile));
    candidate.selected_profile = next_id;
  } else {
    profile.id = profile_id;
    if (profile.protocol == existing->protocol) {
      if (profile.api_key.empty()) profile.api_key = existing->api_key;
      if (profile.access_code.empty()) profile.access_code = existing->access_code;
    }
    *existing = std::move(profile);
  }
  if (!core::validate(candidate).empty()) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Please check the printer name, network address and connection details.\"}");
  }
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Printer settings could not be saved: %s", esp_err_to_name(result));
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"PrintDeck could not save this printer. Please try again.\"}");
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, true);
  const esp_err_t response = send_json(request, "200 OK", "{\"saved\":true}");
  return response;
}

esp_err_t WebConfig::manage_printer(httpd_req_t* request) {
  const std::lock_guard<std::mutex> write_lock(settings_write_mutex_);
  std::string body;
  if (!receive_form(request, body))
    return send_json(request, "413 Payload Too Large",
                     "{\"error\":\"PrintDeck could not process this request. Refresh the page and try again.\"}");
  std::string action;
  std::string id_text;
  std::uint32_t id = 0;
  if (!form_value(body, "action", action) || !form_value(body, "id", id_text) ||
      !parse_id(id_text, id) || id == 0 || (action != "select" && action != "delete")) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"This action could not be understood. Refresh the page and try again.\"}");
  }
  core::DeviceSettings candidate;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    candidate = settings_;
  }
  const auto found = std::find_if(candidate.profiles.begin(), candidate.profiles.end(),
                                  [id](const auto& value) { return value.id == id; });
  if (found == candidate.profiles.end())
    return send_json(request, "404 Not Found",
                     "{\"error\":\"This printer is no longer in your saved list. Refresh the page and try again.\"}");
  if (action == "select") {
    const InactivePrinterSnapshot inactive = inactive_printer_poller_ != nullptr
        ? inactive_printer_poller_->snapshot() : InactivePrinterSnapshot{};
    const auto status = std::find_if(
        inactive.printers.begin(), inactive.printers.end(),
        [id](const InactivePrinterStatus& value) {
          return value.profile_id == id && value.available;
        });
    if (status != inactive.printers.end() && !status->connected) {
      return send_json(request, "409 Conflict",
                       "{\"error\":\"This printer is offline. Turn it on and wait for the printer list to refresh before selecting it.\"}");
    }
    candidate.selected_profile = id;
  } else {
    candidate.profiles.erase(found);
    if (candidate.selected_profile == id) {
      // Deleting the active printer must not silently activate another profile;
      // it may be confirmed offline and selection requires an explicit choice.
      candidate.selected_profile = 0;
    }
  }
  const esp_err_t result = store_->save(candidate);
  if (result != ESP_OK)
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"PrintDeck could not update the printer list. Please try again.\"}");
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = candidate;
  }
  notify_settings_changed(candidate, true);
  const esp_err_t response = send_json(request, "200 OK", "{\"saved\":true}");
  return response;
}

esp_err_t WebConfig::start_compatibility_probe(httpd_req_t* request) {
  std::string body;
  if (!receive_form(request, body)) {
    return send_json(request, "413 Payload Too Large",
                     "{\"error\":\"The connection details are too long. Please review them and try again.\"}");
  }
  std::string profile_id_text;
  std::string protocol;
  BambuLocalConnection connection;
  std::uint32_t profile_id = 0;
  if (!form_value(body, "profile_id", profile_id_text) ||
      !parse_id(profile_id_text, profile_id) || !form_value(body, "protocol", protocol) ||
      protocol != "bambu_lan" || !form_value(body, "endpoint", connection.host) ||
      !form_value(body, "serial", connection.serial) ||
      !form_value(body, "access_code", connection.access_code)) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Please provide the printer address, serial number and LAN access code.\"}");
  }
  if (connection.access_code.empty() && profile_id != 0) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = std::find_if(settings_.profiles.begin(), settings_.profiles.end(),
        [profile_id](const auto& profile) { return profile.id == profile_id; });
    if (existing != settings_.profiles.end() &&
        existing->protocol == core::PrinterProtocol::bambu_lan) {
      connection.access_code = existing->access_code;
    }
  }
  if (!connection.is_ready()) {
    return send_json(request, "400 Bad Request",
                     "{\"error\":\"Enter the printer address, serial number and LAN access code before testing.\"}");
  }
  const esp_err_t result = compatibility_probe_->start(std::move(connection));
  if (result == ESP_ERR_INVALID_STATE) {
    return send_json(request, "409 Conflict",
                     "{\"error\":\"A connection test is already in progress.\"}");
  }
  if (result != ESP_OK) {
    return send_json(request, "500 Internal Server Error",
                     "{\"error\":\"The connection test could not start. Please try again.\"}");
  }
  return send_json(request, "202 Accepted", "{\"started\":true}");
}

esp_err_t WebConfig::serve_compatibility_status(httpd_req_t* request) const {
  const BambuCompatibilitySnapshot snapshot = compatibility_probe_->snapshot();
  const bool running = snapshot.state == BambuCompatibilityState::kConnecting ||
                       snapshot.state == BambuCompatibilityState::kCollecting ||
                       snapshot.state == BambuCompatibilityState::kProbingServices;
  std::string body = "{\"state\":";
  append_json_string(body, to_string(snapshot.state));
  body += ",\"running\":";
  body += running ? "true" : "false";
  body += ",\"progress\":" + std::to_string(snapshot.progress_percent) + ",\"detail\":";
  append_json_string(body, localized(snapshot.detail));
  body += ",\"report_ready\":";
  body += snapshot.report_ready ? "true" : "false";
  body.push_back('}');
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t WebConfig::serve_compatibility_report(httpd_req_t* request) const {
  const std::string report = compatibility_probe_->report_json();
  if (report.empty()) {
    return send_json(request, "404 Not Found",
                     "{\"error\":\"Run a connection test before downloading a report.\"}");
  }
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Content-Disposition",
                     "attachment; filename=printdeck-bambu-compatibility.json");
  return httpd_resp_send(request, report.data(), report.size());
}

esp_err_t WebConfig::cancel_compatibility_probe(httpd_req_t* request) {
  compatibility_probe_->cancel();
  return send_json(request, "200 OK", "{\"cancelled\":true}");
}

esp_err_t WebConfig::schedule_restart() {
  if (restart_timer_ == nullptr) return ESP_ERR_INVALID_STATE;
  const esp_err_t stop_result = esp_timer_stop(restart_timer_);
  if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) return stop_result;
  return esp_timer_start_once(restart_timer_, kRestartDelayUs);
}

}  // namespace printdeck::platform
