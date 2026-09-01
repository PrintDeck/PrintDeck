#include "printdeck/platform/moonraker_connection_probe.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <new>
#include <utility>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {
namespace {

constexpr std::size_t kMaximumResponseBytes = 64 * 1024;
constexpr int kRequestTimeoutMs = 4500;

struct JsonDeleter {
  void operator()(cJSON* value) const { cJSON_Delete(value); }
};
using JsonDocument = std::unique_ptr<cJSON, JsonDeleter>;

struct ResponseBuffer {
  std::string body;
  bool overflow = false;
};

esp_err_t response_event(esp_http_client_event_t* event) {
  if (event == nullptr || event->event_id != HTTP_EVENT_ON_DATA ||
      event->user_data == nullptr || event->data == nullptr || event->data_len <= 0) {
    return ESP_OK;
  }
  auto* response = static_cast<ResponseBuffer*>(event->user_data);
  const std::size_t bytes = static_cast<std::size_t>(event->data_len);
  if (response->body.size() + bytes > kMaximumResponseBytes) {
    response->overflow = true;
    return ESP_FAIL;
  }
  response->body.append(static_cast<const char*>(event->data), bytes);
  return ESP_OK;
}

std::string request_url(std::string endpoint, std::string_view path) {
  while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
  if (endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) {
    endpoint.insert(0, "http://");
  }
  return endpoint + std::string(path);
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string trim(std::string value) {
  const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  }).base();
  return begin < end ? std::string(begin, end) : std::string{};
}

std::string config_metadata(const std::string& config, std::string wanted) {
  wanted = lower(std::move(wanted));
  std::size_t start = 0;
  while (start < config.size()) {
    const std::size_t end = config.find('\n', start);
    std::string line = trim(config.substr(start, end == std::string::npos
                                                    ? std::string::npos : end - start));
    if (!line.empty() && line.front() == '#') line = trim(line.substr(1));
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos && lower(trim(line.substr(0, colon))) == wanted) {
      return trim(line.substr(colon + 1));
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}

std::string brand_for(std::string manufacturer) {
  manufacturer = lower(std::move(manufacturer));
  for (const auto& [needle, brand] : {
           std::pair{"creality", "creality"}, std::pair{"snapmaker", "snapmaker"},
           std::pair{"prusa", "prusa"}, std::pair{"anycubic", "anycubic"},
           std::pair{"elegoo", "elegoo"}, std::pair{"qidi", "qidi"},
           std::pair{"sovol", "sovol"}, std::pair{"flashforge", "flashforge"},
           std::pair{"ankermake", "ankermake"}, std::pair{"voron", "voron"},
           std::pair{"rat rig", "ratrig"}, std::pair{"ratrig", "ratrig"},
           std::pair{"klipper", "klipper"}}) {
    if (manufacturer.find(needle) != std::string::npos) return brand;
  }
  return {};
}

bool perform_get(const core::PrinterProfile& profile, std::string_view path,
                 ResponseBuffer& response, int& status, const char* range = nullptr) {
  response = {};
  const std::string url = request_url(profile.endpoint, path);
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = kRequestTimeoutMs;
  config.event_handler = response_event;
  config.user_data = &response;
  config.buffer_size = 2048;
  config.buffer_size_tx = 512;
  if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return false;
  if (!profile.api_key.empty()) esp_http_client_set_header(client, "X-Api-Key", profile.api_key.c_str());
  esp_http_client_set_header(client, "Accept", "application/json");
  if (range != nullptr) esp_http_client_set_header(client, "Range", range);
  const esp_err_t result = esp_http_client_perform(client);
  status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
  esp_http_client_cleanup(client);
  return result == ESP_OK && !response.overflow && status >= 200 && status < 300;
}

const cJSON* member(const cJSON* object, const char* key) {
  return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

std::string string_member(const cJSON* object, const char* key) {
  const cJSON* value = member(object, key);
  return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : "";
}

}  // namespace

esp_err_t MoonrakerConnectionProbe::start(core::PrinterProfile profile) {
  if (profile.protocol != core::PrinterProtocol::moonraker || profile.endpoint.empty()) {
    return ESP_ERR_INVALID_ARG;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.running) return ESP_ERR_INVALID_STATE;
    pending_profile_ = std::move(profile);
    snapshot_ = {
        .state = MoonrakerProbeState::connecting,
        .progress_percent = 15,
        .detail = "Contacting your printer…",
        .version = {},
        .klipper_state = {},
        .manufacturer = {},
        .model = {},
        .brand = {},
        .evidence = {},
        .running = true,
    };
  }
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "moonraker_check", 8192, this, 4,
                                     &task_, kServiceCore,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    finish(MoonrakerProbeState::unavailable,
           "PrintDeck could not start the connection test. Please try again.");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

MoonrakerProbeSnapshot MoonrakerConnectionProbe::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void MoonrakerConnectionProbe::task_entry(void* context) {
  static_cast<MoonrakerConnectionProbe*>(context)->run();
  vTaskDeleteWithCaps(nullptr);
}

void MoonrakerConnectionProbe::run() {
  core::PrinterProfile profile;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    profile = pending_profile_;
    snapshot_.progress_percent = 40;
    snapshot_.detail = "Checking the Moonraker service…";
  }

  ResponseBuffer response;
  int status = 0;
  const bool server_ready = perform_get(profile, "/server/info", response, status);

  if (status == 401 || status == 403) {
    finish(MoonrakerProbeState::authorization_required,
           "Moonraker answered, but it did not accept the access key.");
    return;
  }
  if (!server_ready) {
    finish(MoonrakerProbeState::unavailable,
           "PrintDeck could not reach Moonraker at this address. Check the address and Wi-Fi network.");
    return;
  }

  JsonDocument document(cJSON_ParseWithLength(response.body.data(), response.body.size()));
  const cJSON* info = member(document.get(), "result");
  const std::string version = string_member(info, "moonraker_version");
  const std::string klipper_state = string_member(info, "klippy_state");
  const cJSON* connected = member(info, "klippy_connected");
  if (!document || !cJSON_IsObject(info) ||
      (version.empty() && !cJSON_IsBool(connected))) {
    finish(MoonrakerProbeState::unavailable,
           "A device answered at this address, but it is not a recognized Moonraker service.");
    return;
  }

  std::string manufacturer;
  std::string model;
  std::string brand;
  std::string evidence;
  ResponseBuffer config_response;
  ResponseBuffer objects_response;
  int identity_status = 0;
  const bool has_config = perform_get(profile, "/server/files/config/printer.cfg",
                                      config_response, identity_status, "bytes=0-65535");
  perform_get(profile, "/printer/objects/list", objects_response, identity_status);
  if (has_config) {
    manufacturer = config_metadata(config_response.body, "manufacturer");
    if (manufacturer.empty()) manufacturer = config_metadata(config_response.body, "brand");
    model = config_metadata(config_response.body, "model");
    if (!manufacturer.empty() || !model.empty()) evidence = "printer.cfg metadata";
  }
  const std::string fingerprint = lower(config_response.body + "\n" + objects_response.body);
  if (fingerprint.find("# f008") != std::string::npos ||
      (fingerprint.find("prtouch_v3") != std::string::npos &&
       fingerprint.find("[stepper_z1]") != std::string::npos &&
       fingerprint.find("[z_tilt]") != std::string::npos &&
       fingerprint.find("[box]") != std::string::npos)) {
    manufacturer = "Creality"; model = "K2 Plus"; brand = "creality";
    evidence = "Creality K2 Plus factory configuration";
  } else if (fingerprint.find("snapmaker u1") != std::string::npos ||
             (fingerprint.find("fm175xx_reader") != std::string::npos &&
              fingerprint.find("flow_calibrator") != std::string::npos &&
              fingerprint.find("mcu e3") != std::string::npos)) {
    manufacturer = "Snapmaker"; model = "U1"; brand = "snapmaker";
    evidence = "Snapmaker U1 factory configuration";
  } else if (fingerprint.find("# f021") != std::string::npos) {
    manufacturer = "Creality"; model = "K2 Series"; brand = "creality";
    evidence = "Creality K2 Series factory configuration";
  } else if (fingerprint.find("# qidi plus4") != std::string::npos ||
             fingerprint.find("# qidi plus 4") != std::string::npos) {
    manufacturer = "QIDI Tech"; model = "Plus4"; brand = "qidi";
    evidence = "QIDI Plus4 configuration header";
  } else if (fingerprint.find("/home/sovol/printer_data/") != std::string::npos) {
    manufacturer = "Sovol"; brand = "sovol";
    evidence = "Sovol factory configuration path";
  }
  if (brand.empty()) brand = brand_for(manufacturer);

  std::string detail = manufacturer.empty() && model.empty()
      ? "Moonraker is connected, but the printer identity was not recognized."
      : "Printer detected: " + (manufacturer.empty() ? std::string{} : manufacturer) +
            (manufacturer.empty() || model.empty() ? std::string{} : " ") + model + ".";
  if (cJSON_IsFalse(connected)) {
    detail += " Klipper is not connected yet.";
  }
  finish(MoonrakerProbeState::ready, std::move(detail), version, klipper_state,
         std::move(manufacturer), std::move(model), std::move(brand), std::move(evidence));
}

void MoonrakerConnectionProbe::finish(MoonrakerProbeState state, std::string detail,
                                      std::string version, std::string klipper_state,
                                      std::string manufacturer, std::string model,
                                      std::string brand, std::string evidence) {
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.state = state;
  snapshot_.progress_percent = 100;
  snapshot_.detail = std::move(detail);
  snapshot_.version = std::move(version);
  snapshot_.klipper_state = std::move(klipper_state);
  snapshot_.manufacturer = std::move(manufacturer);
  snapshot_.model = std::move(model);
  snapshot_.brand = std::move(brand);
  snapshot_.evidence = std::move(evidence);
  snapshot_.running = false;
  task_ = nullptr;
}

}  // namespace printdeck::platform
