#include "printdeck/platform/bambu_compatibility_probe.hpp"
#include "printdeck/platform/task_affinity.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/platform/bambu_model.hpp"
#include "printdeck/platform/bambu_trust.hpp"

#ifndef PRINTDECK_VERSION
#define PRINTDECK_VERSION "unknown"
#endif

namespace printdeck::platform {

namespace {

constexpr char kTag[] = "printdeck.bambu_compat";
constexpr size_t kMaximumReportPayloadBytes = 96U * 1024U;
constexpr size_t kMaximumSchemaFields = 700U;
constexpr size_t kMaximumModules = 24U;
constexpr size_t kMaximumNumericFields = 240U;
constexpr size_t kMaximumArraySamples = 8U;
constexpr uint64_t kConnectDeadlineMs = 16000U;
constexpr uint64_t kCollectionAfterFirstReportMs = 9000U;
constexpr char kGetVersion[] =
    "{\"info\":{\"sequence_id\":\"1\",\"command\":\"get_version\"}}";
constexpr char kStartPush[] =
    "{\"pushing\":{\"sequence_id\":\"2\",\"command\":\"start\"}}";
constexpr char kPushAll[] =
    "{\"pushing\":{\"sequence_id\":\"3\",\"command\":\"pushall\"}}";

uint64_t monotonic_ms() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8U);
  for (unsigned char ch : value) {
    switch (ch) {
      case '\"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (ch < 0x20U) {
          char buffer[7] = {};
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          escaped += buffer;
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
    }
  }
  return escaped;
}

const cJSON* child(const cJSON* object, const char* key) {
  return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

std::string safe_identity_value(const cJSON* object, const char* key) {
  const cJSON* value = child(object, key);
  if (!cJSON_IsString(value) || value->valuestring == nullptr) return {};
  std::string result;
  result.reserve(64U);
  for (const unsigned char ch : std::string(value->valuestring)) {
    if (result.size() >= 64U) break;
    if (std::isalnum(ch) || ch == ' ' || ch == '_' || ch == '-' || ch == '.' ||
        ch == '/' || ch == '(' || ch == ')') {
      result.push_back(static_cast<char>(ch));
    }
  }
  return result;
}

const char* json_type(const cJSON* item) {
  if (cJSON_IsObject(item)) return "object";
  if (cJSON_IsArray(item)) return "array";
  if (cJSON_IsString(item)) return "string";
  if (cJSON_IsNumber(item)) return "number";
  if (cJSON_IsBool(item)) return "boolean";
  if (cJSON_IsNull(item)) return "null";
  return "unknown";
}

void collect_schema(const cJSON* item, const std::string& path,
                    std::set<std::string>* fields, int depth = 0) {
  if (item == nullptr || fields == nullptr || depth > 12 ||
      fields->size() >= kMaximumSchemaFields) {
    return;
  }
  if (!path.empty()) fields->insert(path + ":" + json_type(item));
  if (cJSON_IsObject(item)) {
    const cJSON* member = nullptr;
    cJSON_ArrayForEach(member, item) {
      if (member->string == nullptr || fields->size() >= kMaximumSchemaFields) break;
      const std::string nested = path.empty() ? member->string : path + "." + member->string;
      collect_schema(member, nested, fields, depth + 1);
    }
  } else if (cJSON_IsArray(item)) {
    const int count = std::min<int>(cJSON_GetArraySize(item),
                                    static_cast<int>(kMaximumArraySamples));
    for (int index = 0; index < count; ++index) {
      collect_schema(cJSON_GetArrayItem(item, index), path + "[]", fields, depth + 1);
    }
  }
}

bool sensitive_numeric_path(const std::string& path) {
  static constexpr std::array<const char*, 18> kSensitiveTokens = {
      "access", "account", "address", "code", "device_id", "host",
      "ip", "mac", "name", "password", "profile_id", "sequence_id",
      "serial", "sn", "subtask_id", "task_id", "token", "user_id"};
  std::string component;
  const auto sensitive_component = [&component]() {
    for (const char* token : kSensitiveTokens) {
      if (component == token) return true;
    }
    return component.size() > 3U &&
           component.compare(component.size() - 3U, 3U, "_id") == 0;
  };
  for (unsigned char ch : path) {
    if (std::isalnum(ch) != 0 || ch == '_') {
      component.push_back(static_cast<char>(std::tolower(ch)));
      continue;
    }
    if (!component.empty() && sensitive_component()) return true;
    component.clear();
  }
  return !component.empty() && sensitive_component();
}

void collect_numeric_fields(const cJSON* item, const std::string& path,
                            std::map<std::string, BambuNumericFieldObservation>* fields,
                            int depth = 0) {
  if (item == nullptr || fields == nullptr || depth > 12) return;
  if (cJSON_IsNumber(item)) {
    if (path.empty() || sensitive_numeric_path(path) ||
        !std::isfinite(item->valuedouble)) return;
    auto existing = fields->find(path);
    if (existing == fields->end()) {
      if (fields->size() >= kMaximumNumericFields) return;
      existing = fields->emplace(path, BambuNumericFieldObservation{
          .minimum = item->valuedouble,
          .maximum = item->valuedouble,
          .samples = 0U}).first;
    }
    existing->second.minimum = std::min(existing->second.minimum, item->valuedouble);
    existing->second.maximum = std::max(existing->second.maximum, item->valuedouble);
    ++existing->second.samples;
    return;
  }
  if (cJSON_IsObject(item)) {
    const cJSON* member = nullptr;
    cJSON_ArrayForEach(member, item) {
      if (member->string == nullptr) continue;
      const std::string nested = path.empty() ? member->string : path + "." + member->string;
      collect_numeric_fields(member, nested, fields, depth + 1);
    }
  } else if (cJSON_IsArray(item)) {
    const int count = std::min<int>(cJSON_GetArraySize(item),
                                    static_cast<int>(kMaximumArraySamples));
    for (int index = 0; index < count; ++index) {
      collect_numeric_fields(cJSON_GetArrayItem(item, index), path + "[]", fields, depth + 1);
    }
  }
}

bool schema_has(const std::set<std::string>& fields, const std::string& prefix);

bool schema_has_any(const std::set<std::string>& fields,
                    std::initializer_list<const char*> prefixes) {
  for (const char* prefix : prefixes) {
    if (schema_has(fields, prefix)) return true;
  }
  return false;
}

std::string json_number(double value) {
  char buffer[40]{};
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return buffer;
}

bool schema_has(const std::set<std::string>& fields, const std::string& prefix) {
  const auto iterator = fields.lower_bound(prefix);
  return iterator != fields.end() && iterator->compare(0, prefix.size(), prefix) == 0;
}

bool unsigned_json_value(const cJSON* value, std::uint64_t* result,
                         bool prefer_hex_for_long_text = false) {
  if (result == nullptr) return false;
  if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
      value->valuedouble >= 0.0 && std::floor(value->valuedouble) == value->valuedouble) {
    *result = static_cast<std::uint64_t>(value->valuedouble);
    return true;
  }
  if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
  const char* text = value->valuestring;
  const bool explicit_hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  bool hex_like = true;
  bool contains_hex_alpha = false;
  std::size_t digits = 0;
  for (const char* cursor = text + (explicit_hex ? 2 : 0); *cursor != '\0'; ++cursor) {
    if (std::isxdigit(static_cast<unsigned char>(*cursor)) == 0) {
      hex_like = false;
      break;
    }
    contains_hex_alpha = contains_hex_alpha ||
                         std::isalpha(static_cast<unsigned char>(*cursor)) != 0;
    ++digits;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(
      text, &end,
      explicit_hex || contains_hex_alpha ||
              (prefer_hex_for_long_text && hex_like && digits >= 8)
          ? 16
          : 10);
  if (end == text || end == nullptr || *end != '\0' || errno == ERANGE) return false;
  *result = static_cast<std::uint64_t>(parsed);
  return true;
}

std::string infer_model(const std::vector<BambuCompatibilityProbe::ModuleIdentity>& modules) {
  for (const auto& module : modules) {
    const BambuPrinterModel model = bambu_model_from_identity(
        module.product_name, module.project_name);
    if (model != BambuPrinterModel::unknown) return bambu_model_name(model);
  }
  return {};
}

std::string make_client_id() {
  char buffer[40] = {};
  std::snprintf(buffer, sizeof(buffer), "printdeck-probe-%08lx",
                static_cast<unsigned long>(esp_random()));
  return buffer;
}

std::string make_report_id() {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "%08lx%08lx",
                static_cast<unsigned long>(esp_random()),
                static_cast<unsigned long>(esp_random()));
  return buffer;
}

void redact_token(std::string* text, const std::string& token) {
  if (text == nullptr || token.empty()) return;
  std::string lowered_token = token;
  std::transform(lowered_token.begin(), lowered_token.end(), lowered_token.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  size_t position = 0;
  while (position < text->size()) {
    std::string lowered_text = *text;
    std::transform(lowered_text.begin(), lowered_text.end(), lowered_text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    position = lowered_text.find(lowered_token, position);
    if (position == std::string::npos) break;
    text->replace(position, token.size(), "<redacted>");
    position += 10U;
  }
}

}  // namespace

const char* to_string(BambuCompatibilityState state) {
  switch (state) {
    case BambuCompatibilityState::kIdle: return "idle";
    case BambuCompatibilityState::kConnecting: return "connecting";
    case BambuCompatibilityState::kCollecting: return "collecting";
    case BambuCompatibilityState::kProbingServices: return "probing_services";
    case BambuCompatibilityState::kComplete: return "complete";
    case BambuCompatibilityState::kFailed: return "failed";
    case BambuCompatibilityState::kCancelled: return "cancelled";
  }
  return "idle";
}

esp_err_t BambuCompatibilityProbe::start(BambuLocalConnection connection,
                                         BambuProbePurpose purpose) {
  if (!connection.is_ready()) return ESP_ERR_INVALID_ARG;
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return ESP_ERR_INVALID_STATE;

  cancel_requested_.store(false);
  mqtt_connected_.store(false);
  mqtt_connected_observed_.store(false);
  first_report_received_.store(false);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_connection_ = std::move(connection);
    pending_purpose_ = purpose;
    snapshot_ = {};
    snapshot_.state = BambuCompatibilityState::kConnecting;
    snapshot_.progress_percent = 3;
    snapshot_.detail = purpose == BambuProbePurpose::kConnectionTest
        ? "Preparing the Bambu Lab connection test"
        : "Preparing the read-only BambuLab probe";
    report_json_.clear();
    schema_fields_.clear();
    numeric_fields_.clear();
    modules_.clear();
    detected_model_.clear();
    maximum_payload_bytes_ = 0;
    maximum_live_extruders_ = 0;
    maximum_ams_units_ = 0;
    maximum_virtual_slots_ = 0;
    restricted_commands_observed_ = false;
    incoming_topic_.clear();
    incoming_payload_.clear();
  }

  if (xTaskCreatePinnedToCoreWithCaps(
          &BambuCompatibilityProbe::task_entry, "bambu_compat", 20480, this, 2,
          nullptr, kServiceCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_connection_.access_code.assign(pending_connection_.access_code.size(), '\0');
    pending_connection_ = {};
    snapshot_.state = BambuCompatibilityState::kFailed;
    snapshot_.detail = "The compatibility worker could not be started";
    running_.store(false);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void BambuCompatibilityProbe::cancel() {
  if (running_.load()) cancel_requested_.store(true);
}

BambuCompatibilitySnapshot BambuCompatibilityProbe::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

std::string BambuCompatibilityProbe::report_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return report_json_;
}

void BambuCompatibilityProbe::task_entry(void* context) {
  auto* probe = static_cast<BambuCompatibilityProbe*>(context);
  if (probe == nullptr) {
    vTaskDeleteWithCaps(nullptr);
    return;
  }
  BambuLocalConnection connection;
  BambuProbePurpose purpose = BambuProbePurpose::kCompatibilityReport;
  {
    std::lock_guard<std::mutex> lock(probe->mutex_);
    connection = std::move(probe->pending_connection_);
    probe->pending_connection_ = {};
    purpose = probe->pending_purpose_;
    probe->pending_purpose_ = BambuProbePurpose::kCompatibilityReport;
  }
  probe->task_loop(std::move(connection), purpose);
  vTaskDeleteWithCaps(nullptr);
}

void BambuCompatibilityProbe::set_status(BambuCompatibilityState state, int progress,
                                         const std::string& detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.state = state;
  snapshot_.progress_percent = std::clamp(progress, 0, 100);
  snapshot_.detail = detail;
}

void BambuCompatibilityProbe::task_loop(BambuLocalConnection connection,
                                        BambuProbePurpose purpose) {
  report_topic_ = "device/" + connection.serial + "/report";
  request_topic_ = "device/" + connection.serial + "/request";
  mqtt_client_id_ = make_client_id();

  esp_mqtt_client_config_t config = {};
  config.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
  config.broker.address.hostname = connection.host.c_str();
  config.broker.address.port = connection.mqtt_port;
  const char* ca_bundle = bambu_trust_anchors();
  config.broker.verification.certificate = ca_bundle;
  config.broker.verification.certificate_len = std::strlen(ca_bundle) + 1U;
  config.broker.verification.skip_cert_common_name_check = true;
  config.credentials.client_id = mqtt_client_id_.c_str();
  config.credentials.username = connection.mqtt_username.c_str();
  config.credentials.authentication.password = connection.access_code.c_str();
  config.session.keepalive = 15;
  config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  config.network.timeout_ms = 10000;
  config.network.reconnect_timeout_ms = 3000;
  config.buffer.size = 16384;
  config.buffer.out_size = 4096;
  config.task.stack_size = 10240;

  mqtt_client_ = esp_mqtt_client_init(&config);
  if (mqtt_client_ != nullptr) {
    esp_mqtt_client_register_event(mqtt_client_, MQTT_EVENT_ANY,
                                   &BambuCompatibilityProbe::mqtt_event_handler, this);
    if (esp_mqtt_client_start(mqtt_client_) != ESP_OK) stop_mqtt();
  }

  const uint64_t started_ms = monotonic_ms();
  uint64_t first_report_ms = 0;
  while (!cancel_requested_.load()) {
    const uint64_t elapsed = monotonic_ms() - started_ms;
    if (first_report_received_.load()) {
      if (purpose == BambuProbePurpose::kConnectionTest) break;
      if (first_report_ms == 0) first_report_ms = monotonic_ms();
      const uint64_t collecting_ms = monotonic_ms() - first_report_ms;
      set_status(BambuCompatibilityState::kCollecting,
                 30 + static_cast<int>(std::min<uint64_t>(40U, collecting_ms * 40U /
                                                                  kCollectionAfterFirstReportMs)),
                 "Collecting MQTT field shapes and safe numeric ranges");
      if (collecting_ms >= kCollectionAfterFirstReportMs) break;
    } else {
      set_status(BambuCompatibilityState::kConnecting,
                 5 + static_cast<int>(std::min<uint64_t>(20U, elapsed * 20U /
                                                                 kConnectDeadlineMs)),
                 mqtt_connected_.load() ? "Connected; waiting for the first printer report"
                                        : "Connecting to the printer over MQTT/TLS");
      if (elapsed >= kConnectDeadlineMs) break;
    }
    vTaskDelay(pdMS_TO_TICKS(150));
  }

  const bool mqtt_available = first_report_received_.load();
  const bool mqtt_transport_available = mqtt_connected_observed_.load();
  stop_mqtt();

  if (purpose == BambuProbePurpose::kConnectionTest) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = mqtt_available ? BambuCompatibilityState::kComplete
                                     : BambuCompatibilityState::kFailed;
    snapshot_.progress_percent = 100;
    snapshot_.detail = mqtt_available
        ? "Bambu Lab connection verified."
        : mqtt_transport_available
              ? "The printer answered, but it did not accept these connection details."
              : "PrintDeck could not reach this Bambu Lab printer. Check the address, LAN access code and Wi-Fi network.";
    snapshot_.connection_verified = mqtt_available;
    snapshot_.report_ready = false;
    connection.access_code.assign(connection.access_code.size(), '\0');
    connection = {};
    report_topic_.clear();
    request_topic_.clear();
    mqtt_client_id_.clear();
    running_.store(false);
    return;
  }

  bool service_6000 = false;
  bool service_322 = false;
  bool service_990 = false;
  if (!cancel_requested_.load()) {
    set_status(BambuCompatibilityState::kProbingServices, 73,
               "Checking certificate-validated local service availability");
    service_6000 = probe_tls_service(connection, 6000);
    set_status(BambuCompatibilityState::kProbingServices, 82,
               "Checking optional local video service");
    if (!cancel_requested_.load()) service_322 = probe_tls_service(connection, 322);
    set_status(BambuCompatibilityState::kProbingServices, 91,
               "Checking optional local file service");
    if (!cancel_requested_.load()) service_990 = probe_tls_service(connection, 990);
  }

  if (cancel_requested_.load()) {
    set_status(BambuCompatibilityState::kCancelled, 0, "Compatibility check cancelled");
  } else {
    const std::string detail = mqtt_available
                                   ? "Compatibility report ready to download"
                                   : "No MQTT report was received; a partial service report is ready";
    finish_report(connection, mqtt_transport_available, service_6000, service_322, service_990,
                  detail);
  }

  connection.access_code.assign(connection.access_code.size(), '\0');
  connection = {};
  report_topic_.clear();
  request_topic_.clear();
  mqtt_client_id_.clear();
  running_.store(false);
}

void BambuCompatibilityProbe::mqtt_event_handler(void* context, esp_event_base_t,
                                                 int32_t, void* event_data) {
  auto* probe = static_cast<BambuCompatibilityProbe*>(context);
  if (probe != nullptr) {
    probe->handle_mqtt_event(static_cast<esp_mqtt_event_handle_t>(event_data));
  }
}

void BambuCompatibilityProbe::handle_mqtt_event(esp_mqtt_event_handle_t event) {
  if (event == nullptr || cancel_requested_.load()) return;
  switch (static_cast<esp_mqtt_event_id_t>(event->event_id)) {
    case MQTT_EVENT_CONNECTED:
      mqtt_connected_.store(true);
      mqtt_connected_observed_.store(true);
      esp_mqtt_client_subscribe(mqtt_client_, report_topic_.c_str(), 0);
      break;
    case MQTT_EVENT_SUBSCRIBED:
      esp_mqtt_client_publish(mqtt_client_, request_topic_.c_str(), kGetVersion, 0, 0, 0);
      esp_mqtt_client_publish(mqtt_client_, request_topic_.c_str(), kStartPush, 0, 0, 0);
      esp_mqtt_client_publish(mqtt_client_, request_topic_.c_str(), kPushAll, 0, 0, 0);
      break;
    case MQTT_EVENT_DISCONNECTED:
    case MQTT_EVENT_ERROR:
      mqtt_connected_.store(false);
      break;
    case MQTT_EVENT_DATA: {
      if (event->total_data_len <= 0 ||
          static_cast<size_t>(event->total_data_len) > kMaximumReportPayloadBytes) {
        return;
      }
      std::string topic;
      std::vector<char> payload;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (event->current_data_offset == 0) {
          incoming_topic_.assign(event->topic, event->topic_len);
          incoming_payload_.assign(static_cast<size_t>(event->total_data_len), '\0');
        }
        const size_t offset = static_cast<size_t>(event->current_data_offset);
        const size_t chunk = static_cast<size_t>(event->data_len);
        if (offset + chunk > incoming_payload_.size()) {
          incoming_topic_.clear();
          incoming_payload_.clear();
          return;
        }
        std::memcpy(incoming_payload_.data() + offset, event->data, chunk);
        if (offset + chunk == incoming_payload_.size()) {
          topic = incoming_topic_;
          payload.swap(incoming_payload_);
          incoming_topic_.clear();
        }
      }
      if (!payload.empty() && topic == report_topic_) {
        consume_report(payload.data(), payload.size());
      }
      break;
    }
    default:
      break;
  }
}

void BambuCompatibilityProbe::consume_report(const char* payload, size_t length) {
  cJSON* root = cJSON_ParseWithLength(payload, length);
  if (root == nullptr) return;

  std::set<std::string> fields;
  collect_schema(root, "", &fields);
  std::map<std::string, BambuNumericFieldObservation> numeric_fields;
  collect_numeric_fields(root, "", &numeric_fields);
  std::vector<ModuleIdentity> discovered_modules;
  const cJSON* info = child(root, "info");
  const cJSON* module_array = child(info, "module");
  if (!cJSON_IsArray(module_array)) module_array = child(info, "modules");
  if (cJSON_IsArray(module_array)) {
    const cJSON* module = nullptr;
    cJSON_ArrayForEach(module, module_array) {
      if (discovered_modules.size() >= kMaximumModules || !cJSON_IsObject(module)) break;
      ModuleIdentity identity;
      identity.name = safe_identity_value(module, "name");
      identity.product_name = safe_identity_value(module, "product_name");
      identity.project_name = safe_identity_value(module, "project_name");
      identity.hardware_version = safe_identity_value(module, "hw_ver");
      identity.software_version = safe_identity_value(module, "sw_ver");
      if (!identity.name.empty() || !identity.product_name.empty() ||
          !identity.project_name.empty() || !identity.hardware_version.empty() ||
          !identity.software_version.empty()) {
        discovered_modules.push_back(std::move(identity));
      }
    }
  }

  bool print_active = false;
  const cJSON* print = child(root, "print");
  const cJSON* state = child(print, "gcode_state");
  if (cJSON_IsString(state) && state->valuestring != nullptr) {
    const std::string value(state->valuestring);
    print_active = value == "RUNNING" || value == "PAUSE" || value == "PAUSED" ||
                   value == "PREPARE" || value == "SLICING" || value == "INIT";
  }

  size_t live_extruders = 0;
  size_t ams_units = 0;
  size_t virtual_slots = 0;
  bool restricted_commands = false;
  const cJSON* device = child(print, "device");
  const cJSON* extruder = child(device, "extruder");
  std::uint64_t extruder_state = 0;
  if (unsigned_json_value(child(extruder, "state"), &extruder_state)) {
    live_extruders = std::min<size_t>(extruder_state & 0x0FU, 12U);
  }
  const cJSON* extruder_info = child(extruder, "info");
  if (cJSON_IsArray(extruder_info)) {
    const int count = cJSON_GetArraySize(extruder_info);
    for (int index = 0; index < count; ++index) {
      const cJSON* item = cJSON_GetArrayItem(extruder_info, index);
      std::uint64_t id = static_cast<std::uint64_t>(index);
      unsigned_json_value(child(item, "id"), &id);
      if (id < 12U) live_extruders = std::max(live_extruders, static_cast<size_t>(id + 1U));
    }
  }
  const cJSON* units = child(child(print, "ams"), "ams");
  if (cJSON_IsArray(units)) ams_units = static_cast<size_t>(cJSON_GetArraySize(units));
  const cJSON* virtual_array = child(print, "vir_slot");
  if (!cJSON_IsArray(virtual_array)) virtual_array = child(device, "vir_slot");
  if (!cJSON_IsArray(virtual_array)) {
    virtual_array = child(child(device, "ams"), "vir_slot");
  }
  if (cJSON_IsArray(virtual_array)) {
    virtual_slots = static_cast<size_t>(cJSON_GetArraySize(virtual_array));
  }
  std::uint64_t feature_flags = 0;
  if (unsigned_json_value(child(print, "fun"), &feature_flags, true)) {
    restricted_commands = (feature_flags & 0x20000000ULL) != 0;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& field : fields) {
      if (schema_fields_.size() >= kMaximumSchemaFields) break;
      schema_fields_.insert(field);
    }
    for (const auto& [path, observation] : numeric_fields) {
      auto existing = numeric_fields_.find(path);
      if (existing == numeric_fields_.end()) {
        if (numeric_fields_.size() >= kMaximumNumericFields) break;
        numeric_fields_.emplace(path, observation);
        continue;
      }
      existing->second.minimum = std::min(existing->second.minimum, observation.minimum);
      existing->second.maximum = std::max(existing->second.maximum, observation.maximum);
      existing->second.samples += observation.samples;
    }
    for (auto& module : discovered_modules) {
      const auto duplicate = std::find_if(modules_.begin(), modules_.end(),
          [&module](const ModuleIdentity& existing) {
            return existing.name == module.name &&
                   existing.software_version == module.software_version &&
                   existing.project_name == module.project_name;
          });
      if (duplicate == modules_.end() && modules_.size() < kMaximumModules) {
        modules_.push_back(std::move(module));
      }
    }
    detected_model_ = infer_model(modules_);
    maximum_live_extruders_ = std::max(maximum_live_extruders_, live_extruders);
    maximum_ams_units_ = std::max(maximum_ams_units_, ams_units);
    maximum_virtual_slots_ = std::max(maximum_virtual_slots_, virtual_slots);
    restricted_commands_observed_ = restricted_commands_observed_ || restricted_commands;
    snapshot_.model = detected_model_;
    ++snapshot_.mqtt_messages;
    snapshot_.active_print_observed = snapshot_.active_print_observed || print_active;
    maximum_payload_bytes_ = std::max(maximum_payload_bytes_, length);
  }
  first_report_received_.store(true);
  cJSON_Delete(root);
}

bool BambuCompatibilityProbe::probe_tls_service(const BambuLocalConnection& connection,
                                                uint16_t port) const {
  if (cancel_requested_.load()) return false;
  const char* ca_bundle = bambu_trust_anchors();
  esp_tls_cfg_t config = {};
  config.cacert_buf = reinterpret_cast<const unsigned char*>(ca_bundle);
  config.cacert_bytes = static_cast<unsigned int>(std::strlen(ca_bundle) + 1U);
  config.skip_common_name = true;
  config.timeout_ms = 2500;
  config.addr_family = ESP_TLS_AF_INET;
  config.tls_version = ESP_TLS_VER_TLS_1_2;
  esp_tls_t* tls = esp_tls_init();
  if (tls == nullptr) return false;
  const bool available = esp_tls_conn_new_sync(
      connection.host.c_str(), static_cast<int>(connection.host.size()), port,
      &config, tls) == 1;
  esp_tls_conn_destroy(tls);
  return available;
}

void BambuCompatibilityProbe::finish_report(const BambuLocalConnection& connection,
                                            bool mqtt_available,
                                            bool service_6000, bool service_322,
                                            bool service_990,
                                            const std::string& terminal_detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool has_ams = schema_has(schema_fields_, "print.ams") ||
                       schema_has(schema_fields_, "print.vt_tray") ||
                       schema_has(schema_fields_, "print.vir_slot") ||
                       schema_has(schema_fields_, "print.device.ams") ||
                       schema_has(schema_fields_, "print.device.vir_slot");
  const bool has_chamber = schema_has(schema_fields_, "print.chamber_temper") ||
                           schema_has(schema_fields_, "print.device.chamber") ||
                           schema_has(schema_fields_, "print.device.ctc");
  const bool has_v2_shape = schema_has(schema_fields_, "print.device") ||
                            schema_has(schema_fields_, "print.vir_slot");
  const bool has_multi_tool = maximum_live_extruders_ > 1;
  const bool has_speed_multiplier = schema_has_any(
      schema_fields_, {"print.spd_mag", "print.speed_multiplier", "print.speed_factor"});
  const bool has_fan_speed = schema_has_any(
      schema_fields_, {"print.cooling_fan_speed", "print.fan_speed", "print.fan_gear"});
  const bool has_velocity = schema_has_any(
      schema_fields_, {"print.velocity", "print.toolhead.velocity", "print.motion.velocity"});
  const bool has_position = schema_has_any(
      schema_fields_, {"print.position", "print.toolhead.position", "print.motion.position"});
  const bool has_flow = schema_has_any(
      schema_fields_, {"print.flow", "print.flow_rate", "print.extrusion_multiplier"});
  const bool has_heater_power = schema_has_any(
      schema_fields_, {"print.nozzle_power", "print.bed_power", "print.heater_power",
                       "print.device.heater"});
  const bool has_layer_data = schema_has_any(
      schema_fields_, {"print.layer_num", "print.total_layer_num"});
  const bool has_remaining_time = schema_has(schema_fields_, "print.mc_remaining_time");
  const bool has_light_status = schema_has(schema_fields_, "print.lights_report");
  const BambuPrinterModel model = bambu_model_from_identity(detected_model_);
  const BambuModelCapabilities model_capabilities = bambu_capabilities_for(model);
  const char* expected_camera = model_capabilities.camera == BambuCameraProtocol::jpeg_tls
                                    ? "jpeg_tls_6000"
                                    : model_capabilities.camera == BambuCameraProtocol::rtsps
                                          ? "rtsps_322"
                                          : "unknown";
  const char* observed_camera = service_6000 && !service_322
                                    ? "jpeg_tls_6000"
                                    : service_322 && !service_6000
                                          ? "rtsps_322"
                                          : service_6000 && service_322 ? "both" : "none";
  const std::string report_id = make_report_id();

  std::string body;
  body.reserve(6144U + schema_fields_.size() * 48U + numeric_fields_.size() * 80U);
  body += "{\n  \"report_schema\":\"printdeck.bambu.compatibility.v3\",";
  body += "\n  \"report_id\":\"" + report_id + "\",";
  body += "\n  \"monitor_firmware\":\"" PRINTDECK_VERSION "\",";
  body += "\n  \"privacy\":{\"raw_payloads_included\":false,\"host_included\":false,";
  body += "\"serial_included\":false,\"lan_access_code_included\":false,";
  body += "\"ssid_included\":false,\"job_names_included\":false,";
  body += "\"string_values_in_numeric_summaries\":false,";
  body += "\"sensitive_numeric_paths_included\":false},";
  body += "\n  \"identity\":{\"model_guess\":";
  body += detected_model_.empty() ? "null" : "\"" + json_escape(detected_model_) + "\"";
  body += ",\"modules\":[";
  for (size_t index = 0; index < modules_.size(); ++index) {
    if (index > 0) body += ',';
    const auto& module = modules_[index];
    body += "{\"name\":\"" + json_escape(module.name) + "\",";
    body += "\"product_name\":\"" + json_escape(module.product_name) + "\",";
    body += "\"project_name\":\"" + json_escape(module.project_name) + "\",";
    body += "\"hardware_version\":\"" + json_escape(module.hardware_version) + "\",";
    body += "\"software_version\":\"" + json_escape(module.software_version) + "\"}";
  }
  body += "]},";
  body += "\n  \"transport\":{\"mqtt_tls_8883\":";
  body += mqtt_available ? "true" : "false";
  body += ",\"camera_tls_6000\":" + std::string(service_6000 ? "true" : "false");
  body += ",\"video_tls_322\":" + std::string(service_322 ? "true" : "false");
  body += ",\"ftps_tls_990\":" + std::string(service_990 ? "true" : "false");
  body += ",\"expected_camera_protocol\":\"" + std::string(expected_camera) + "\"";
  body += ",\"observed_camera_protocol\":\"" + std::string(observed_camera) + "\"},";
  body += "\n  \"observations\":{\"mqtt_messages\":" +
          std::to_string(snapshot_.mqtt_messages);
  body += ",\"maximum_payload_bytes\":" + std::to_string(maximum_payload_bytes_);
  body += ",\"maximum_live_extruders\":" + std::to_string(maximum_live_extruders_);
  body += ",\"maximum_ams_units\":" + std::to_string(maximum_ams_units_);
  body += ",\"maximum_virtual_slots\":" + std::to_string(maximum_virtual_slots_);
  body += ",\"active_print_observed\":" +
          std::string(snapshot_.active_print_observed ? "true" : "false");
  body += ",\"signed_critical_commands_required\":" +
          std::string(restricted_commands_observed_ ? "true" : "false") + "},";
  body += "\n  \"capability_hints\":{\"mqtt_v2_shape\":" +
          std::string(has_v2_shape ? "true" : "false");
  body += ",\"ams_status\":" + std::string(has_ams ? "true" : "false");
  body += ",\"chamber_temperature\":" + std::string(has_chamber ? "true" : "false");
  body += ",\"multiple_toolheads\":" + std::string(has_multi_tool ? "true" : "false");
  body += ",\"vortek_storage_metadata\":" +
          std::string(model == BambuPrinterModel::h2c ? "true" : "false");
  body += ",\"speed_multiplier\":" + std::string(has_speed_multiplier ? "true" : "false");
  body += ",\"fan_speed\":" + std::string(has_fan_speed ? "true" : "false");
  body += ",\"motion_velocity\":" + std::string(has_velocity ? "true" : "false");
  body += ",\"axis_position\":" + std::string(has_position ? "true" : "false");
  body += ",\"extrusion_flow\":" + std::string(has_flow ? "true" : "false");
  body += ",\"heater_power\":" + std::string(has_heater_power ? "true" : "false");
  body += ",\"layer_data\":" + std::string(has_layer_data ? "true" : "false");
  body += ",\"remaining_time\":" + std::string(has_remaining_time ? "true" : "false");
  body += ",\"chamber_light_status\":" + std::string(has_light_status ? "true" : "false");
  body += ",\"local_camera_service\":" + std::string(service_6000 ? "true" : "false");
  body += ",\"local_video_service\":" + std::string(service_322 ? "true" : "false");
  body += ",\"local_file_service\":" + std::string(service_990 ? "true" : "false") + "},";
  body += "\n  \"mqtt_schema\":[";
  size_t field_index = 0;
  for (const std::string& field : schema_fields_) {
    if (field_index++ > 0) body += ',';
    body += "\"" + json_escape(field) + "\"";
  }
  body += "],";
  body += "\n  \"numeric_field_summaries\":[";
  size_t numeric_index = 0;
  for (const auto& [path, observation] : numeric_fields_) {
    if (numeric_index++ > 0) body += ',';
    body += "{\"path\":\"" + json_escape(path) + "\",";
    body += "\"samples\":" + std::to_string(observation.samples) + ',';
    body += "\"minimum\":" + json_number(observation.minimum) + ',';
    body += "\"maximum\":" + json_number(observation.maximum) + '}';
  }
  body += "]\n}\n";

  // Defense in depth: field shapes, filtered numeric summaries and allow-listed
  // firmware identity values should never contain credentials, but scrub the
  // exact endpoint tokens before the document becomes downloadable.
  redact_token(&body, connection.host);
  redact_token(&body, connection.serial);
  redact_token(&body, connection.access_code);

  report_json_ = std::move(body);
  snapshot_.state = BambuCompatibilityState::kComplete;
  snapshot_.progress_percent = 100;
  snapshot_.detail = terminal_detail;
  snapshot_.report_ready = true;
  snapshot_.connection_verified = mqtt_available;
}

void BambuCompatibilityProbe::stop_mqtt() {
  mqtt_connected_.store(false);
  if (mqtt_client_ != nullptr) {
    esp_mqtt_client_stop(mqtt_client_);
    esp_mqtt_client_destroy(mqtt_client_);
    mqtt_client_ = nullptr;
  }
}

}  // namespace printdeck::platform
