#include "printdeck/platform/bambu_lan_adapter.hpp"
#include "printdeck/platform/bambu_report_parser.hpp"
#include "printdeck/platform/task_affinity.hpp"

#include "sdkconfig.h"

#if !CONFIG_MQTT_TASK_STACK_ON_EXTERNAL_MEMORY
#error "PrintDeck requires CONFIG_MQTT_TASK_STACK_ON_EXTERNAL_MEMORY=y; use a clean build-local sdkconfig"
#endif

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "printdeck/platform/bambu_trust.hpp"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "bambu_lan";
constexpr std::size_t kMaximumReportBytes = 96 * 1024;
constexpr std::uint64_t kChamberLightTimeoutMs = 10000ULL;
constexpr std::uint64_t kInitialStatusTimeoutMs = 30000ULL;
constexpr std::uint64_t kStatusSilenceBeforeRecoveryMs = 75000ULL;
constexpr std::uint64_t kRecoveryResponseTimeoutMs = 25000ULL;

std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

}  // namespace

esp_err_t BambuLanAdapter::start(const core::PrinterProfile* selected_profile,
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
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "bambu_lan", 8192, this, 5, &task_,
                                      kServiceCore,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    task_ = nullptr;
    running_.store(false, std::memory_order_release);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void BambuLanAdapter::stop() {
  stop_requested_.store(true, std::memory_order_release);
  TaskHandle_t task = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_;
  }
  if (task != nullptr) xTaskNotifyGive(task);
}

void BambuLanAdapter::configure(const core::PrinterProfile* selected_profile) {
  {
    const std::lock_guard<std::mutex> lock(profile_mutex_);
    pending_profile_ = selected_profile != nullptr &&
                               selected_profile->protocol == core::PrinterProtocol::bambu_lan
                           ? *selected_profile
                           : core::PrinterProfile{};
  }
  reconfigure_requested_.store(true);
  TaskHandle_t task = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_;
  }
  if (task != nullptr) xTaskNotifyGive(task);
}

core::PrinterSnapshot BambuLanAdapter::snapshot() const { return snapshots_.read(); }

BambuModelCapabilities BambuLanAdapter::capabilities() const {
  return bambu_capabilities_for(model_.load());
}

bool BambuLanAdapter::request_chamber_light(bool enabled) {
  TaskHandle_t task = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_;
  }
  if (!connected_.load() || task == nullptr ||
      stop_requested_.load(std::memory_order_acquire)) return false;
  core::PrinterSnapshot next = snapshots_.read();
  if (next.job.chamber_light_pending) return false;
  next.job.chamber_light_pending = true;
  next.job.chamber_light_target_on = enabled;
  snapshots_.replace(std::move(next));
  pending_chamber_light_.store(enabled ? 1 : 0);
  chamber_light_deadline_ms_.store(
      static_cast<std::uint64_t>(esp_timer_get_time() / 1000) + kChamberLightTimeoutMs);
  xTaskNotifyGive(task);
  return true;
}

void BambuLanAdapter::task_entry(void* context) {
  auto* adapter = static_cast<BambuLanAdapter*>(context);
  adapter->task_loop();
  {
    const std::lock_guard<std::mutex> lock(adapter->task_mutex_);
    adapter->task_ = nullptr;
  }
  adapter->running_.store(false, std::memory_order_release);
  vTaskDeleteWithCaps(nullptr);
}

void BambuLanAdapter::mqtt_entry(void* context, esp_event_base_t, std::int32_t,
                                 void* event_data) {
  static_cast<BambuLanAdapter*>(context)->handle_mqtt(
      static_cast<esp_mqtt_event_handle_t>(event_data));
}

void BambuLanAdapter::task_loop() {
  publish_state(core::LinkState::stopped, "No Bambu LAN printer selected");
  while (!stop_requested_.load(std::memory_order_acquire)) {
    if (reconfigure_requested_.exchange(false)) {
      connected_.store(false);
      if (client_ != nullptr) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
      }
      {
        const std::lock_guard<std::mutex> lock(profile_mutex_);
        profile_ = pending_profile_;
      }
      report_topic_.clear();
      request_topic_.clear();
      client_id_.clear();
      model_.store(bambu_model_from_identity({}, profile_.model));
      restricted_commands_.store(false);
      sequence_id_.store(1);
      reset_session_health();
      pending_chamber_light_.store(-1);
      chamber_light_deadline_ms_.store(0);
      {
        const std::lock_guard<std::mutex> lock(incoming_mutex_);
        incoming_topic_.clear();
        incoming_payload_.clear();
      }
      core::PrinterSnapshot fresh;
      fresh.profile_id = profile_.id;
      fresh.link = profile_.id == 0 ? core::LinkState::stopped : core::LinkState::connecting;
      fresh.link_detail = profile_.id == 0 ? "No Bambu LAN printer selected"
                                           : "Connecting through local MQTT/TLS";
      snapshots_.replace(std::move(fresh));
    }
    const std::uint64_t light_deadline = chamber_light_deadline_ms_.load();
    if (light_deadline != 0 &&
        static_cast<std::uint64_t>(esp_timer_get_time() / 1000) >= light_deadline) {
      pending_chamber_light_.store(-1);
      chamber_light_deadline_ms_.store(0);
      core::PrinterSnapshot timed_out = snapshots_.read();
      timed_out.job.chamber_light_pending = false;
      snapshots_.replace(std::move(timed_out));
    }
    if (profile_.id == 0) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
      continue;
    }
    if (network_ == nullptr || !network_->status().station_connected) {
      publish_state(core::LinkState::waiting_for_network, "Waiting for Wi-Fi");
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1500));
      continue;
    }
    if (client_ == nullptr) {
      const esp_err_t result = begin_client();
      if (result != ESP_OK) {
        publish_state(core::LinkState::failed, "Bambu LAN connection could not start");
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        continue;
      }
    }
    const int light_request = pending_chamber_light_.exchange(-1);
    if (light_request >= 0) {
      if (!publish_chamber_light(light_request != 0)) {
        pending_chamber_light_.store(light_request);
      }
    }
    maintain_session(monotonic_ms());
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
  connected_.store(false);
  if (client_ != nullptr) {
    esp_mqtt_client_stop(client_);
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
  }
  report_topic_.clear();
  request_topic_.clear();
  client_id_.clear();
  model_.store(BambuPrinterModel::unknown);
  restricted_commands_.store(false);
  reset_session_health();
  pending_chamber_light_.store(-1);
  chamber_light_deadline_ms_.store(0);
  {
    const std::lock_guard<std::mutex> lock(incoming_mutex_);
    incoming_topic_.clear();
    incoming_payload_.clear();
  }
  {
    const std::lock_guard<std::mutex> lock(profile_mutex_);
    profile_ = {};
    pending_profile_ = {};
  }
  core::PrinterSnapshot stopped;
  stopped.link = core::LinkState::stopped;
  stopped.link_detail = "Bambu LAN connection unloaded";
  snapshots_.replace(std::move(stopped));
}

esp_err_t BambuLanAdapter::begin_client() {
  ESP_LOGI(kLogTag, "Starting local MQTT/TLS; internal heap=%u, largest=%u, PSRAM=%u",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  report_topic_ = "device/" + profile_.serial + "/report";
  request_topic_ = "device/" + profile_.serial + "/request";
  char client_id[32]{};
  std::snprintf(client_id, sizeof(client_id), "printdeck-%08lx",
                static_cast<unsigned long>(esp_random()));
  client_id_ = client_id;

  esp_mqtt_client_config_t config{};
  config.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
  config.broker.address.hostname = profile_.endpoint.c_str();
  config.broker.address.port = 8883;
  config.broker.verification.certificate = bambu_trust_anchors();
  config.broker.verification.skip_cert_common_name_check = true;
  config.credentials.client_id = client_id_.c_str();
  config.credentials.username = "bblp";
  config.credentials.authentication.password = profile_.access_code.c_str();
  config.session.keepalive = 20;
  config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  config.buffer.size = 16384;
  config.buffer.out_size = 2048;
  config.task.stack_size = 10240;
  config.network.timeout_ms = 15000;
  config.network.reconnect_timeout_ms = 5000;
  client_ = esp_mqtt_client_init(&config);
  if (client_ == nullptr) {
    ESP_LOGE(kLogTag, "Local MQTT/TLS client allocation failed; internal heap=%u, largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    return ESP_ERR_NO_MEM;
  }
  esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, mqtt_entry, this);
  publish_state(core::LinkState::connecting, "Connecting through local MQTT/TLS");
  const esp_err_t result = esp_mqtt_client_start(client_);
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "Local MQTT/TLS task could not start: %s; internal heap=%u, largest=%u",
             esp_err_to_name(result),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
  }
  return result;
}

void BambuLanAdapter::publish_state(core::LinkState link, const char* detail) {
  core::PrinterSnapshot next = snapshots_.read();
  next.profile_id = profile_.id;
  next.link = link;
  next.link_detail = detail;
  next.job.reachable = link == core::LinkState::online;
  next.updated_at_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  snapshots_.replace(std::move(next));
}

bool BambuLanAdapter::publish(const char* payload) {
  return client_ != nullptr && connected_.load() && !request_topic_.empty() &&
         esp_mqtt_client_publish(client_, request_topic_.c_str(), payload, 0, 0, 0) >= 0;
}

bool BambuLanAdapter::publish_command(const char* section, const char* command) {
  if (section == nullptr || command == nullptr) return false;
  const std::uint32_t sequence = sequence_id_.fetch_add(1);
  char payload[160]{};
  const int length = std::snprintf(payload, sizeof(payload),
      "{\"%s\":{\"sequence_id\":\"%lu\",\"command\":\"%s\"}}",
      section, static_cast<unsigned long>(sequence), command);
  return length > 0 && static_cast<std::size_t>(length) < sizeof(payload) && publish(payload);
}

bool BambuLanAdapter::publish_chamber_light(bool enabled) {
  const std::uint32_t sequence = sequence_id_.fetch_add(1);
  char payload[320]{};
  const int length = std::snprintf(
      payload, sizeof(payload),
      "{\"system\":{\"sequence_id\":\"%lu\",\"command\":\"ledctrl\","
      "\"led_node\":\"chamber_light\",\"led_mode\":\"%s\","
      "\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":0,"
      "\"interval_time\":0}}",
      static_cast<unsigned long>(sequence), enabled ? "on" : "off");
  return length > 0 && static_cast<std::size_t>(length) < sizeof(payload) && publish(payload);
}

void BambuLanAdapter::reset_session_health() {
  status_ready_.store(false);
  connected_at_ms_.store(0);
  last_report_ms_.store(0);
  last_status_report_ms_.store(0);
  last_full_request_ms_.store(0);
  recovery_request_ms_.store(0);
  oversized_reports_.store(0);
}

void BambuLanAdapter::maintain_session(std::uint64_t now_ms) {
  if (!connected_.load() || client_ == nullptr) return;
  const bool ready = status_ready_.load();
  const std::uint64_t last_status = last_status_report_ms_.load();
  const std::uint64_t connected_at = connected_at_ms_.load();

  if (ready) {
    const std::uint64_t last_full = last_full_request_ms_.load();
    const std::uint64_t interval = capabilities().full_report_refresh_ms;
    if (last_full != 0 && now_ms >= last_full && now_ms - last_full >= interval &&
        publish_command("pushing", "pushall")) {
      last_full_request_ms_.store(now_ms);
    }
  }

  const std::uint64_t reference = ready ? last_status : connected_at;
  const std::uint64_t timeout = ready ? kStatusSilenceBeforeRecoveryMs
                                      : kInitialStatusTimeoutMs;
  if (reference == 0 || now_ms < reference || now_ms - reference < timeout) {
    if (ready) recovery_request_ms_.store(0);
    return;
  }

  const std::uint64_t recovery = recovery_request_ms_.load();
  if (recovery == 0) {
    ESP_LOGW(kLogTag, "Printer status is stale; requesting one bounded refresh");
    publish_command("pushing", "start");
    if (publish_command("pushing", "pushall")) last_full_request_ms_.store(now_ms);
    recovery_request_ms_.store(now_ms);
    return;
  }
  if (now_ms >= recovery && now_ms - recovery >= kRecoveryResponseTimeoutMs) {
    ESP_LOGW(kLogTag, "Printer status did not recover; reconnecting local MQTT/TLS");
    status_ready_.store(false);
    recovery_request_ms_.store(now_ms);
    publish_state(core::LinkState::connecting, "Reconnecting through local MQTT/TLS");
    esp_mqtt_client_reconnect(client_);
  }
}

void BambuLanAdapter::handle_mqtt(esp_mqtt_event_handle_t event) {
  if (event == nullptr) return;
  switch (static_cast<esp_mqtt_event_id_t>(event->event_id)) {
    case MQTT_EVENT_CONNECTED:
      connected_.store(true);
      status_ready_.store(false);
      connected_at_ms_.store(monotonic_ms());
      last_report_ms_.store(0);
      last_status_report_ms_.store(0);
      last_full_request_ms_.store(0);
      recovery_request_ms_.store(0);
      esp_mqtt_client_subscribe(client_, report_topic_.c_str(), 0);
      // The transport is ready, but the dashboard does not have a trustworthy
      // printer state until the first complete report is parsed. Keeping this
      // as connecting avoids briefly rendering the unavailable reaction and
      // opens the dashboard only after handle_report() publishes online.
      publish_state(core::LinkState::connecting, "Connected; waiting for printer status");
      break;
    case MQTT_EVENT_SUBSCRIBED:
      publish_command("info", "get_version");
      publish_command("pushing", "start");
      if (publish_command("pushing", "pushall")) last_full_request_ms_.store(monotonic_ms());
      break;
    case MQTT_EVENT_DISCONNECTED:
      connected_.store(false);
      status_ready_.store(false);
      connected_at_ms_.store(0);
      recovery_request_ms_.store(0);
      publish_state(core::LinkState::connecting, "Reconnecting through local MQTT/TLS");
      break;
    case MQTT_EVENT_ERROR:
      connected_.store(false);
      status_ready_.store(false);
      if (event->error_handle != nullptr) {
        ESP_LOGW(kLogTag,
                 "Local MQTT/TLS error type=%d esp=%d tls=%d verify=0x%x socket=%d",
                 static_cast<int>(event->error_handle->error_type),
                 static_cast<int>(event->error_handle->esp_tls_last_esp_err),
                 event->error_handle->esp_tls_stack_err,
                 event->error_handle->esp_tls_cert_verify_flags,
                 event->error_handle->esp_transport_sock_errno);
      }
      publish_state(core::LinkState::failed, "Check address, serial, LAN code and LAN mode");
      break;
    case MQTT_EVENT_DATA: {
      if (event->total_data_len <= 0) return;
      if (static_cast<std::size_t>(event->total_data_len) > kMaximumReportBytes) {
        if (event->current_data_offset == 0) {
          const std::uint32_t count = oversized_reports_.fetch_add(1) + 1;
          ESP_LOGW(kLogTag, "Ignored oversized Bambu report (%d bytes, occurrence %lu)",
                   event->total_data_len, static_cast<unsigned long>(count));
          const std::lock_guard<std::mutex> lock(incoming_mutex_);
          incoming_payload_.clear();
          incoming_topic_.clear();
        }
        return;
      }
      std::string topic;
      std::vector<char> complete;
      {
        const std::lock_guard<std::mutex> lock(incoming_mutex_);
        if (event->current_data_offset == 0) {
          incoming_topic_.assign(event->topic, event->topic_len);
          incoming_payload_.assign(static_cast<std::size_t>(event->total_data_len), '\0');
        }
        const std::size_t offset = static_cast<std::size_t>(event->current_data_offset);
        const std::size_t length = static_cast<std::size_t>(event->data_len);
        if (offset + length > incoming_payload_.size()) {
          incoming_payload_.clear();
          incoming_topic_.clear();
          return;
        }
        std::memcpy(incoming_payload_.data() + offset, event->data, length);
        if (offset + length == incoming_payload_.size()) {
          topic = incoming_topic_;
          complete.swap(incoming_payload_);
          incoming_topic_.clear();
        }
      }
      if (!complete.empty() && topic == report_topic_) {
        handle_report(complete.data(), complete.size());
      }
      break;
    }
    default: break;
  }
}

void BambuLanAdapter::handle_report(const char* payload, std::size_t length) {
  const std::uint64_t received_at = monotonic_ms();
  BambuReportParseResult parsed = parse_bambu_report(
      payload, length, snapshots_.read(), profile_.id,
      received_at);
  if (!parsed.parsed) return;
  last_report_ms_.store(received_at);
  if (parsed.identity_report) {
    const BambuPrinterModel detected = bambu_model_from_identity(parsed.product_name,
                                                                  profile_.model);
    if (detected != BambuPrinterModel::unknown) model_.store(detected);
  }
  if (parsed.restricted_commands) restricted_commands_.store(true);
  if (parsed.chamber_light_confirmed) chamber_light_deadline_ms_.store(0);
  if (parsed.status_report) {
    status_ready_.store(true);
    last_status_report_ms_.store(received_at);
    recovery_request_ms_.store(0);
    snapshots_.replace(std::move(parsed.snapshot));
  }
}

}  // namespace printdeck::platform
