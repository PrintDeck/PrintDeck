#include "printdeck/platform/moonraker_adapter.hpp"
#include "printdeck/platform/moonraker_status_parser.hpp"
#include "printdeck/platform/task_affinity.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "mbedtls/base64.h"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "moonraker";
constexpr std::size_t kMaximumStatusBytes = 64 * 1024;
constexpr std::size_t kMaximumConfigBytes = 256 * 1024;
constexpr std::size_t kMaximumThumbnailBytes = 512 * 1024;
constexpr std::size_t kMaximumGcodeHeaderBytes = 256 * 1024;
// HTTP/TLS calls and status/light discovery share this worker's call stack.
// The light-discovery path pushed the previous 10 KiB allocation past its
// canary while an unavailable printer was timing out.
constexpr std::uint32_t kMoonrakerTaskStackBytes = 16U * 1024U;
constexpr TickType_t kActivePollDelay = pdMS_TO_TICKS(1000);
constexpr TickType_t kIdlePollDelay = pdMS_TO_TICKS(3000);
constexpr std::uint64_t kChamberLightTimeoutMs = 8000;
constexpr std::uint8_t kRedChannel = 1U << 0U;
constexpr std::uint8_t kGreenChannel = 1U << 1U;
constexpr std::uint8_t kBlueChannel = 1U << 2U;
constexpr std::uint8_t kWhiteChannel = 1U << 3U;

struct JsonDeleter {
  void operator()(cJSON* value) const { cJSON_Delete(value); }
};
using JsonDocument = std::unique_ptr<cJSON, JsonDeleter>;

struct ResponseBuffer {
  std::string body;
  std::size_t maximum_bytes = kMaximumStatusBytes;
  bool overflow = false;
};

esp_err_t response_event(esp_http_client_event_t* event) {
  if (event == nullptr || event->user_data == nullptr || event->event_id != HTTP_EVENT_ON_DATA ||
      event->data == nullptr || event->data_len <= 0) {
    return ESP_OK;
  }
  auto* response = static_cast<ResponseBuffer*>(event->user_data);
  const std::size_t bytes = static_cast<std::size_t>(event->data_len);
  if (response->body.size() + bytes > response->maximum_bytes) {
    response->overflow = true;
    return ESP_FAIL;
  }
  response->body.append(static_cast<const char*>(event->data), bytes);
  return ESP_OK;
}

std::string base_url(std::string endpoint) {
  while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
  if (endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) {
    endpoint.insert(0, "http://");
  }
  return endpoint;
}

bool http_request(const core::PrinterProfile& profile, const char* path,
                  esp_http_client_method_t method, std::string* body = nullptr,
                  std::size_t maximum_bytes = kMaximumStatusBytes,
                  const char* range = nullptr, const char* accept = "application/json") {
  ResponseBuffer response;
  response.maximum_bytes = maximum_bytes;
  const std::string url = base_url(profile.endpoint) + path;
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.method = method;
  config.timeout_ms = 5000;
  config.event_handler = response_event;
  config.user_data = &response;
  config.buffer_size = 2048;
  config.buffer_size_tx = 512;
  if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return false;
  if (!profile.api_key.empty()) {
    esp_http_client_set_header(client, "X-Api-Key", profile.api_key.c_str());
  }
  if (range != nullptr) esp_http_client_set_header(client, "Range", range);
  esp_http_client_set_header(client, "Accept", accept);
  const esp_err_t result = esp_http_client_perform(client);
  const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
  esp_http_client_cleanup(client);
  if (body != nullptr) *body = std::move(response.body);
  return result == ESP_OK && !response.overflow && status >= 200 && status < 300;
}

std::string url_encode(std::string_view value, bool preserve_slashes) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size() + 16);
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
        character == '~' || (preserve_slashes && character == '/')) {
      encoded.push_back(static_cast<char>(character));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[character >> 4]);
      encoded.push_back(kHex[character & 0x0f]);
    }
  }
  return encoded;
}

bool is_png(std::string_view bytes) {
  static constexpr unsigned char kSignature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  if (bytes.size() < sizeof(kSignature)) return false;
  return std::equal(std::begin(kSignature), std::end(kSignature),
                    reinterpret_cast<const unsigned char*>(bytes.data()));
}

std::shared_ptr<std::vector<std::uint8_t>> extract_embedded_png(const std::string& header) {
  std::size_t scan = 0;
  int best_width = 0;
  std::string best_encoded;
  while ((scan = header.find("; thumbnail begin ", scan)) != std::string::npos) {
    int width = 0;
    int height = 0;
    unsigned int declared_size = 0;
    if (std::sscanf(header.c_str() + scan, "; thumbnail begin %dx%d %u", &width, &height,
                    &declared_size) != 3) {
      ++scan;
      continue;
    }
    const std::size_t data_start = header.find('\n', scan);
    const std::size_t data_end = data_start == std::string::npos
                                     ? std::string::npos
                                     : header.find("; thumbnail end", data_start);
    if (data_start == std::string::npos || data_end == std::string::npos) break;
    if (width > best_width && width <= 320 && height > 0 && height <= 320 &&
        declared_size <= kMaximumGcodeHeaderBytes) {
      std::string encoded;
      encoded.reserve(std::min<std::size_t>(declared_size, data_end - data_start));
      std::size_t line_start = data_start + 1;
      while (line_start < data_end && encoded.size() <= kMaximumGcodeHeaderBytes) {
        std::size_t line_end = header.find('\n', line_start);
        if (line_end == std::string::npos || line_end > data_end) line_end = data_end;
        std::size_t content = line_start;
        if (content < line_end && header[content] == ';') ++content;
        while (content < line_end &&
               (header[content] == ' ' || header[content] == '\r' || header[content] == '\t')) {
          ++content;
        }
        for (std::size_t index = content; index < line_end; ++index) {
          const char character = header[index];
          if (character != ' ' && character != '\r' && character != '\t') {
            encoded.push_back(character);
          }
        }
        line_start = line_end + 1;
      }
      if (encoded.size() <= kMaximumGcodeHeaderBytes) {
        best_width = width;
        best_encoded = std::move(encoded);
      }
    }
    scan = data_end + 1;
  }
  if (best_encoded.empty()) return nullptr;

  auto decoded = std::make_shared<std::vector<std::uint8_t>>(
      (best_encoded.size() * 3U) / 4U + 4U);
  std::size_t decoded_size = 0;
  const int result = mbedtls_base64_decode(
      decoded->data(), decoded->size(), &decoded_size,
      reinterpret_cast<const unsigned char*>(best_encoded.data()), best_encoded.size());
  if (result != 0 || decoded_size > kMaximumThumbnailBytes) return nullptr;
  decoded->resize(decoded_size);
  if (!is_png(std::string_view(reinterpret_cast<const char*>(decoded->data()), decoded->size()))) {
    return nullptr;
  }
  return decoded;
}

const cJSON* member(const cJSON* object, const char* key) {
  return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

std::string string_member(const cJSON* object, const char* key) {
  const cJSON* value = member(object, key);
  return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : "";
}

double number_member(const cJSON* object, const char* key, double fallback = 0.0) {
  const cJSON* value = member(object, key);
  return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) ? value->valuedouble
                                                                   : fallback;
}

std::string lowercase_ascii(std::string_view text) {
  std::string result(text);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return result;
}

const cJSON* case_insensitive_member(const cJSON* object, std::string_view key) {
  if (!cJSON_IsObject(object)) return nullptr;
  const cJSON* item = nullptr;
  cJSON_ArrayForEach(item, object) {
    if (item->string == nullptr || key.size() != std::strlen(item->string)) continue;
    bool equal = true;
    for (std::size_t index = 0; index < key.size(); ++index) {
      if (std::tolower(static_cast<unsigned char>(key[index])) !=
          std::tolower(static_cast<unsigned char>(item->string[index]))) {
        equal = false;
        break;
      }
    }
    if (equal) return item;
  }
  return nullptr;
}

int tool_index_from_name(const std::string& name) {
  constexpr std::string_view prefix = "extruder";
  if (name == prefix) return 0;
  if (name.rfind(prefix, 0) != 0 || name.size() == prefix.size()) return -1;
  for (std::size_t index = prefix.size(); index < name.size(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(name[index]))) return -1;
  }
  return std::atoi(name.c_str() + prefix.size());
}

bool job_is_active(core::JobPhase phase) {
  return phase == core::JobPhase::preparing || phase == core::JobPhase::printing ||
         phase == core::JobPhase::paused;
}

}  // namespace

esp_err_t MoonrakerAdapter::start(const core::PrinterProfile* selected_profile,
                                  const NetworkService& network) {
  configure(selected_profile);
  const std::lock_guard<std::mutex> lock(task_mutex_);
  if (running_.load(std::memory_order_acquire)) {
    return stop_requested_.load(std::memory_order_acquire) ? ESP_ERR_INVALID_STATE
                                                           : ESP_OK;
  }
  network_ = &network;
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  if (xTaskCreatePinnedToCoreWithCaps(
          task_entry, "moonraker", kMoonrakerTaskStackBytes, this, 5, &task_, kServiceCore,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    task_ = nullptr;
    running_.store(false, std::memory_order_release);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void MoonrakerAdapter::stop() {
  stop_requested_.store(true, std::memory_order_release);
  TaskHandle_t task = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_;
  }
  if (task != nullptr) xTaskNotifyGive(task);
}

void MoonrakerAdapter::configure(const core::PrinterProfile* selected_profile) {
  {
    const std::lock_guard<std::mutex> lock(profile_mutex_);
    profile_ = selected_profile != nullptr &&
                       selected_profile->protocol == core::PrinterProtocol::moonraker
                   ? *selected_profile
                   : core::PrinterProfile{};
  }
  TaskHandle_t task = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_;
  }
  if (task != nullptr) xTaskNotifyGive(task);
}

core::PrinterSnapshot MoonrakerAdapter::snapshot() const { return snapshots_.read(); }

bool MoonrakerAdapter::request_chamber_light(bool enabled) {
  TaskHandle_t task = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_;
  }
  if (task == nullptr || stop_requested_.load(std::memory_order_acquire)) return false;
  core::PrinterSnapshot next = snapshots_.read();
  if (next.link != core::LinkState::online || !next.job.chamber_light_supported ||
      next.job.chamber_light_pending) {
    return false;
  }
  next.job.chamber_light_pending = true;
  next.job.chamber_light_target_on = enabled;
  snapshots_.replace(std::move(next));
  pending_chamber_light_.store(enabled ? 1 : 0);
  chamber_light_deadline_ms_.store(
      static_cast<std::uint64_t>(esp_timer_get_time() / 1000) + kChamberLightTimeoutMs);
  xTaskNotifyGive(task);
  return true;
}

void MoonrakerAdapter::task_entry(void* context) {
  auto* adapter = static_cast<MoonrakerAdapter*>(context);
  adapter->task_loop();
  {
    const std::lock_guard<std::mutex> lock(adapter->task_mutex_);
    adapter->task_ = nullptr;
  }
  adapter->running_.store(false, std::memory_order_release);
  vTaskDeleteWithCaps(nullptr);
}

core::PrinterProfile MoonrakerAdapter::profile() const {
  const std::lock_guard<std::mutex> lock(profile_mutex_);
  return profile_;
}

void MoonrakerAdapter::publish(core::LinkState link, const char* detail) {
  core::PrinterSnapshot next = snapshots_.read();
  next.profile_id = profile().id;
  next.link = link;
  next.link_detail = detail;
  next.job.reachable = link == core::LinkState::online;
  next.updated_at_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  snapshots_.replace(std::move(next));
}

void MoonrakerAdapter::task_loop() {
  publish(core::LinkState::stopped, "No Moonraker printer selected");
  while (!stop_requested_.load(std::memory_order_acquire)) {
    const core::PrinterProfile current = profile();
    if (current.id != active_profile_id_) {
      active_profile_id_ = current.id;
      cached_metadata_filename_.clear();
      cached_preview_.reset();
      cached_estimated_seconds_ = 0;
      cached_total_layers_ = 0;
      tool_objects_.clear();
      chamber_sensor_object_.clear();
      chamber_light_ = {};
      chamber_light_channels_ = 0;
      pending_chamber_light_.store(-1);
      chamber_light_deadline_ms_.store(0);
      has_print_task_config_ = false;
      core::PrinterSnapshot fresh;
      fresh.profile_id = current.id;
      fresh.link = current.id == 0 ? core::LinkState::stopped : core::LinkState::connecting;
      fresh.link_detail = current.id == 0 ? "No Moonraker printer selected"
                                          : "Connecting to Moonraker";
      snapshots_.replace(std::move(fresh));
    }
    if (current.id == 0) {
      publish(core::LinkState::stopped, "No Moonraker printer selected");
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
      continue;
    }
    if (network_ == nullptr || !network_->status().station_connected) {
      publish(core::LinkState::waiting_for_network, "Waiting for Wi-Fi");
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
      continue;
    }

    const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    const std::uint64_t light_deadline = chamber_light_deadline_ms_.load();
    if (light_deadline != 0 && now_ms >= light_deadline) {
      pending_chamber_light_.store(-1);
      chamber_light_deadline_ms_.store(0);
      core::PrinterSnapshot timed_out = snapshots_.read();
      timed_out.job.chamber_light_pending = false;
      snapshots_.replace(std::move(timed_out));
    }

    if (tool_objects_.empty()) discover_printer(current);
    const int light_request = pending_chamber_light_.exchange(-1);
    if (light_request >= 0 && !send_chamber_light(current, light_request != 0)) {
      pending_chamber_light_.store(light_request);
    }
    const bool success = poll(current);
    const core::JobPhase phase = snapshots_.read().job.phase;
    ulTaskNotifyTake(pdTRUE, success && job_is_active(phase) ? kActivePollDelay : kIdlePollDelay);
  }
  cached_metadata_filename_.clear();
  cached_preview_.reset();
  cached_estimated_seconds_ = 0;
  cached_total_layers_ = 0;
  tool_objects_.clear();
  chamber_sensor_object_.clear();
  chamber_light_ = {};
  chamber_light_channels_ = 0;
  active_profile_id_ = 0;
  has_print_task_config_ = false;
  {
    const std::lock_guard<std::mutex> lock(profile_mutex_);
    profile_ = {};
  }
  core::PrinterSnapshot stopped;
  stopped.link = core::LinkState::stopped;
  stopped.link_detail = "Moonraker connection unloaded";
  snapshots_.replace(std::move(stopped));
}

bool MoonrakerAdapter::discover_printer(const core::PrinterProfile& profile) {
  std::string body;
  if (!http_request(profile, "/printer/objects/list", HTTP_METHOD_GET, &body)) return false;
  JsonDocument document(cJSON_ParseWithLength(body.data(), body.size()));
  const cJSON* objects = member(member(document.get(), "result"), "objects");
  if (!document || !cJSON_IsArray(objects)) return false;

  tool_objects_.clear();
  chamber_sensor_object_.clear();
  chamber_light_ = {};
  chamber_light_channels_ = 0;
  has_print_task_config_ = false;
  std::vector<std::string> object_names;
  const int count = cJSON_GetArraySize(objects);
  for (int index = 0; index < count; ++index) {
    const cJSON* item = cJSON_GetArrayItem(objects, index);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) continue;
    const std::string name(item->valuestring);
    object_names.push_back(name);
    const int tool_index = tool_index_from_name(name);
    if (tool_index >= 0 && tool_index < static_cast<int>(core::kMaximumToolheads)) {
      tool_objects_.push_back(name);
    }
    if (name == "temperature_sensor cavity" || name == "temperature_sensor chamber_temp" ||
        name == "temperature_sensor chamber" ||
        (name.rfind("temperature_sensor ", 0) == 0 &&
         name.find("chamber") != std::string::npos)) {
      chamber_sensor_object_ = name;
    }
    if (name == "print_task_config") has_print_task_config_ = true;
  }
  chamber_light_ = discover_moonraker_light(object_names);
  if (chamber_light_.kind == MoonrakerLightKind::led) {
    std::string config_body;
    if (http_request(profile, "/printer/objects/query?configfile=config", HTTP_METHOD_GET,
                     &config_body, kMaximumConfigBytes)) {
      JsonDocument config_document(cJSON_ParseWithLength(config_body.data(), config_body.size()));
      const cJSON* settings = member(member(member(config_document.get(), "result"), "status"),
                                     "configfile");
      settings = member(settings, "config");
      const cJSON* light_settings =
          case_insensitive_member(settings, chamber_light_.object_name);
      if (cJSON_IsObject(light_settings)) {
        const std::string prefix = lowercase_ascii(chamber_light_.object_name);
        if (prefix.rfind("led ", 0) == 0) {
          if (member(light_settings, "red_pin") != nullptr) chamber_light_channels_ |= kRedChannel;
          if (member(light_settings, "green_pin") != nullptr) chamber_light_channels_ |= kGreenChannel;
          if (member(light_settings, "blue_pin") != nullptr) chamber_light_channels_ |= kBlueChannel;
          if (member(light_settings, "white_pin") != nullptr) chamber_light_channels_ |= kWhiteChannel;
        } else if (prefix.rfind("dotstar ", 0) == 0) {
          chamber_light_channels_ = kRedChannel | kGreenChannel | kBlueChannel;
        } else {
          const std::string order = lowercase_ascii(string_member(light_settings, "color_order"));
          if (order.find('r') != std::string::npos) chamber_light_channels_ |= kRedChannel;
          if (order.find('g') != std::string::npos) chamber_light_channels_ |= kGreenChannel;
          if (order.find('b') != std::string::npos) chamber_light_channels_ |= kBlueChannel;
          if (order.find('w') != std::string::npos) chamber_light_channels_ |= kWhiteChannel;
          if (chamber_light_channels_ == 0) {
            chamber_light_channels_ = kRedChannel | kGreenChannel | kBlueChannel;
          }
        }
      }
    }
    if (chamber_light_channels_ == 0) chamber_light_ = {};
  }
  std::sort(tool_objects_.begin(), tool_objects_.end(), [](const auto& left, const auto& right) {
    return tool_index_from_name(left) < tool_index_from_name(right);
  });
  ESP_LOGI(kLogTag, "Discovered %u Moonraker toolhead(s)",
           static_cast<unsigned>(tool_objects_.size()));
  if (chamber_light_.kind != MoonrakerLightKind::none) {
    ESP_LOGI(kLogTag, "Discovered Moonraker printer light object: %s",
             chamber_light_.object_name.c_str());
  }
  return !tool_objects_.empty();
}

bool MoonrakerAdapter::poll(const core::PrinterProfile& profile) {
  std::string query =
      "/printer/objects/query?webhooks&print_stats&virtual_sdcard&display_status&"
      "heater_bed&toolhead&gcode_move&motion_report&fan";
  if (tool_objects_.empty()) {
    // Preserve single-extruder status while discovery is being retried after a
    // transient /objects/list failure.
    query += "&extruder";
  } else {
    for (const std::string& tool : tool_objects_) query += "&" + url_encode(tool, false);
  }
  if (!chamber_sensor_object_.empty()) {
    query += "&" + url_encode(chamber_sensor_object_, false);
  }
  if (has_print_task_config_) query += "&print_task_config";
  if (chamber_light_.kind != MoonrakerLightKind::none) {
    query += "&" + url_encode(chamber_light_.object_name, false);
  }
  std::string body;
  if (!http_request(profile, query.c_str(), HTTP_METHOD_GET, &body)) {
    publish(core::LinkState::failed, "Moonraker is unreachable");
    return false;
  }

  const auto context = [this]() {
    const core::PrinterSnapshot previous = snapshots_.read();
    return MoonrakerStatusParseContext{
        .tool_objects = tool_objects_,
        .chamber_sensor_object = chamber_sensor_object_,
        .chamber_light = chamber_light_,
        .chamber_light_pending = previous.job.chamber_light_pending,
        .chamber_light_target_on = previous.job.chamber_light_target_on,
        .preview = cached_preview_,
        .estimated_seconds = cached_estimated_seconds_,
        .total_layers = cached_total_layers_,
    };
  };
  const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  MoonrakerStatusParseResult parsed = parse_moonraker_status(
      body.data(), body.size(), profile.id, now_ms, context());
  if (!parsed.parsed || !parsed.ready) {
    publish(core::LinkState::failed, "Klipper is not ready");
    return false;
  }
  if (parsed.snapshot.job.gcode_file != cached_metadata_filename_) {
    refresh_job_metadata(profile, parsed.snapshot.job.gcode_file);
    parsed = parse_moonraker_status(body.data(), body.size(), profile.id, now_ms, context());
    if (!parsed.parsed || !parsed.ready) {
      publish(core::LinkState::failed, "Klipper status could not be decoded");
      return false;
    }
  }
  if (parsed.snapshot.job.chamber_light_supported &&
      !parsed.snapshot.job.chamber_light_pending) {
    chamber_light_deadline_ms_.store(0);
  }
  snapshots_.replace(std::move(parsed.snapshot));
  return true;
}

bool MoonrakerAdapter::send_chamber_light(const core::PrinterProfile& profile,
                                          bool enabled) {
  if (chamber_light_.kind == MoonrakerLightKind::none) return false;
  std::string command;
  if (chamber_light_.kind == MoonrakerLightKind::output_pin) {
    command = "SET_PIN PIN=" + chamber_light_.config_name +
              " VALUE=" + (enabled ? "1" : "0");
  } else {
    command = "SET_LED LED=" + chamber_light_.config_name;
    const char* value = enabled ? "1" : "0";
    if ((chamber_light_channels_ & kRedChannel) != 0) command += " RED=" + std::string(value);
    if ((chamber_light_channels_ & kGreenChannel) != 0) command += " GREEN=" + std::string(value);
    if ((chamber_light_channels_ & kBlueChannel) != 0) command += " BLUE=" + std::string(value);
    if ((chamber_light_channels_ & kWhiteChannel) != 0) command += " WHITE=" + std::string(value);
    command += " SYNC=0";
  }
  const std::string path = "/printer/gcode/script?script=" + url_encode(command, false);
  return http_request(profile, path.c_str(), HTTP_METHOD_POST);
}

void MoonrakerAdapter::refresh_job_metadata(const core::PrinterProfile& profile,
                                            const std::string& filename) {
  cached_metadata_filename_ = filename;
  cached_preview_.reset();
  cached_estimated_seconds_ = 0;
  cached_total_layers_ = 0;
  if (filename.empty()) return;

  std::string metadata;
  const std::string metadata_path =
      "/server/files/metadata?filename=" + url_encode(filename, true);
  if (!http_request(profile, metadata_path.c_str(), HTTP_METHOD_GET, &metadata)) return;

  JsonDocument document(cJSON_ParseWithLength(metadata.data(), metadata.size()));
  const cJSON* result = member(document.get(), "result");
  if (!document || !cJSON_IsObject(result)) return;
  cached_estimated_seconds_ = static_cast<std::uint32_t>(
      std::clamp(number_member(result, "estimated_time"), 0.0, 4294967295.0));
  cached_total_layers_ = static_cast<std::uint16_t>(
      std::clamp(number_member(result, "layer_count"), 0.0, 65535.0));

  const cJSON* thumbnails = member(result, "thumbnails");
  std::string thumbnail_path;
  int best_width = 0;
  if (cJSON_IsArray(thumbnails)) {
    const int count = cJSON_GetArraySize(thumbnails);
    for (int index = 0; index < count; ++index) {
      const cJSON* thumbnail = cJSON_GetArrayItem(thumbnails, index);
      const int width = static_cast<int>(number_member(thumbnail, "width"));
      const int height = static_cast<int>(number_member(thumbnail, "height"));
      const std::string relative_path = string_member(thumbnail, "relative_path");
      if (!relative_path.empty() && width > best_width && width <= 320 && height > 0 &&
          height <= 320) {
        best_width = width;
        thumbnail_path = relative_path;
      }
    }
  }

  if (!thumbnail_path.empty()) {
    std::string image;
    const std::string image_path =
        "/server/files/gcodes/" + url_encode(thumbnail_path, true);
    if (http_request(profile, image_path.c_str(), HTTP_METHOD_GET, &image,
                     kMaximumThumbnailBytes, nullptr, "image/png") &&
        is_png(image)) {
      cached_preview_ = std::make_shared<std::vector<std::uint8_t>>(image.begin(), image.end());
      ESP_LOGI(kLogTag, "Loaded Moonraker thumbnail (%u px, %u bytes)", best_width,
               static_cast<unsigned>(cached_preview_->size()));
    }
    return;
  }

  // Some Moonraker appliances omit the metadata thumbnail list and embed a PNG
  // in the G-code header. Download only the first 256 KiB, never the whole job.
  std::string header;
  const std::string gcode_path = "/server/files/gcodes/" + url_encode(filename, true);
  if (http_request(profile, gcode_path.c_str(), HTTP_METHOD_GET, &header,
                   kMaximumGcodeHeaderBytes, "bytes=0-262143", "application/octet-stream")) {
    cached_preview_ = extract_embedded_png(header);
    if (cached_preview_) {
      ESP_LOGI(kLogTag, "Loaded embedded Moonraker thumbnail (%u bytes)",
               static_cast<unsigned>(cached_preview_->size()));
    }
  }
}

}  // namespace printdeck::platform
