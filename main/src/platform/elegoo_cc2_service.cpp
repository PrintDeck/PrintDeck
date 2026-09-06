#include "printdeck/platform/elegoo_cc2_service.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "mdns.h"
#include "mqtt_client.h"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {
namespace {
constexpr std::uint64_t kIdentityTimeoutMs = 2500;
constexpr std::uint64_t kFullRefreshMs = 5000;
constexpr std::uint64_t kRequestTimeoutMs = 4000;
std::uint64_t now_ms() { return static_cast<std::uint64_t>(esp_timer_get_time()) / 1000; }
bool cancelled_now(const std::function<bool()>& cancelled) { return cancelled && cancelled(); }
bool expired(std::uint64_t deadline, const std::function<bool()>& cancelled) {
  return now_ms() >= deadline || cancelled_now(cancelled);
}
void delay() { vTaskDelay(pdMS_TO_TICKS(20)); }

struct ResolverSlot {
  std::array<char, 129> name{};
  ip_addr_t address{};
  bool used = false, waiting = false, complete = false, found = false;
};
std::array<ResolverSlot, 2> resolver_slots;
std::mutex resolver_mutex;
void dns_result(const char*, const ip_addr_t* value, void* argument) {
  const std::lock_guard<std::mutex> lock(resolver_mutex);
  auto& slot = *static_cast<ResolverSlot*>(argument);
  slot.found = value && IP_IS_V4(value);
  if (slot.found) slot.address = *value;
  slot.complete = true;
  if (!slot.waiting) slot.used = false;
}
void dns_start(void* argument) {
  auto& slot = *static_cast<ResolverSlot*>(argument);
  ip_addr_t address{};
  const err_t result = dns_gethostbyname_addrtype(slot.name.data(), &address, dns_result,
                                                 argument, LWIP_DNS_ADDRTYPE_IPV4);
  if (result == ERR_OK) dns_result(nullptr, &address, argument);
  else if (result != ERR_INPROGRESS) dns_result(nullptr, nullptr, argument);
}
std::string resolve(const std::string& host, std::uint64_t deadline,
                    const std::function<bool()>& cancelled) {
  ip_addr_t address{};
  if (!ipaddr_aton(host.c_str(), &address)) {
    if (host.ends_with(".local")) {
      esp_ip4_addr_t native{};
      if (expired(deadline, cancelled)) return {};
      const auto budget = static_cast<std::uint32_t>(std::min<std::uint64_t>(1000, deadline - now_ms()));
      const auto name = host.substr(0, host.size() - 6);
      if (mdns_query_a(name.c_str(), std::max<std::uint32_t>(1, budget), &native) != ESP_OK) return {};
      ip_addr_set_ip4_u32(&address, native.addr);
    } else {
      ResolverSlot* slot = nullptr;
      {
        const std::lock_guard<std::mutex> lock(resolver_mutex);
        for (auto& candidate : resolver_slots) if (!candidate.used) { slot = &candidate; break; }
        if (!slot) return {};
        *slot = {};
        std::memcpy(slot->name.data(), host.data(), host.size());
        slot->used = slot->waiting = true;
      }
      if (tcpip_try_callback(dns_start, slot) != ERR_OK) {
        const std::lock_guard<std::mutex> lock(resolver_mutex);
        slot->used = false;
        return {};
      }
      bool found = false;
      while (!expired(deadline, cancelled)) {
        {
          const std::lock_guard<std::mutex> lock(resolver_mutex);
          if (slot->complete) { found = slot->found; address = slot->address; break; }
        }
        delay();
      }
      {
        const std::lock_guard<std::mutex> lock(resolver_mutex);
        slot->waiting = false;
        if (slot->complete) slot->used = false;
      }
      if (!found) return {};
    }
  }
  std::array<char, 16> formatted{};
  if (!IP_IS_V4(&address) || !ipaddr_ntoa_r(&address, formatted.data(), formatted.size()) ||
      expired(deadline, cancelled) || !elegoo_cc2_endpoint_valid(formatted.data())) return {};
  return formatted.data();
}

// Two bounded clients at most (selected plus one short probe). A second client
// for the same numeric printer is refused, including discovery/manual checks.
std::mutex ownership_mutex;
std::array<std::string, 2> owned_hosts;
class HostLease {
 public:
  explicit HostLease(const std::string& host) {
    const std::lock_guard<std::mutex> lock(ownership_mutex);
    for (const auto& active : owned_hosts) if (active == host) return;
    for (std::size_t i = 0; i < owned_hosts.size(); ++i) if (owned_hosts[i].empty()) {
      owned_hosts[i] = host; index_ = static_cast<int>(i); return;
    }
  }
  ~HostLease() {
    const std::lock_guard<std::mutex> lock(ownership_mutex);
    if (index_ >= 0) owned_hosts[index_].clear();
  }
  bool held() const { return index_ >= 0; }
 private:
  int index_ = -1;
};

class Session {
 public:
  Session(const core::PrinterProfile& profile, const ElegooIdentity& identity, const std::string& host)
      : profile_(profile), host_(host) {
    reducer_.configure(profile.id, identity);
    char random[32]{};
    std::snprintf(random, sizeof(random), "pd_%08lx%08lx", static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    client_id_ = random;
    registration_id_ = client_id_ + "_req";
    const std::string prefix = "elegoo/" + identity.serial;
    response_topic_ = prefix + "/" + client_id_ + "/api_response";
    status_topic_ = prefix + "/api_status";
    registration_topic_ = prefix + "/" + registration_id_ + "/register_response";
    register_to_ = prefix + "/api_register";
    request_topic_ = prefix + "/" + client_id_ + "/api_request";
  }
  ~Session() {
    closing_ = true;
    if (client_) { esp_mqtt_client_stop(client_); esp_mqtt_client_destroy(client_); }
    if (!profile_.access_code.empty()) {
      volatile char* value = profile_.access_code.data();
      for (std::size_t i = 0; i < profile_.access_code.size(); ++i) value[i] = 0;
    }
  }
  ElegooPollResult run(std::uint64_t deadline, const std::function<bool()>& cancelled,
      ElegooIdentity* identity, const std::function<void(const core::PrinterSnapshot&)>& publish) {
    esp_mqtt_client_config_t config{};
    config.broker.address.hostname = host_.c_str();
    config.broker.address.port = 1883;
    config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    config.credentials.client_id = client_id_.c_str();
    config.credentials.username = "elegoo";
    config.credentials.authentication.password = profile_.access_code.c_str();
    config.session.keepalive = 20;
    config.session.disable_clean_session = false;
    config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    config.network.disable_auto_reconnect = true;
    config.network.timeout_ms = 2000;
    config.buffer.size = 2048;
    config.buffer.out_size = 512;
    config.task.stack_size = 12288;
    client_ = esp_mqtt_client_init(&config);
    if (!client_) return {.error = ElegooError::capacity};
    if (esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, event, this) != ESP_OK ||
        esp_mqtt_client_start(client_) != ESP_OK) return {.error = ElegooError::unavailable};
    const std::uint64_t setup_deadline = std::min(deadline, now_ms() + 12000);
    bool subscribed = false, registration_sent = false, queried = false;
    std::uint64_t registration_deadline = 0, query_deadline = 0, next_query = 0;
    std::uint64_t next_ping = 0, next_publish = 0;
    std::uint32_t next_id = 1;
    for (;;) {
      if (cancelled_now(cancelled)) return {.error = ElegooError::cancelled};
      const auto now = now_ms();
      if (now >= deadline) return {.error = ElegooError::timeout};
      bool subscribe_now = false, register_now = false, ping_now = false;
      std::uint32_t attribute_request = 0, status_request = 0;
      bool publish_now = false;
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (error_ != ElegooError::none) return {.error = error_};
        if (!reducer_.baseline_ready() && now >= setup_deadline) return {.error = ElegooError::timeout};
        if (connected_ && !subscribed) {
          subscribed = true;
          subscribe_now = true;
        }
        for (unsigned i = 0; i < 3; ++i) for (const int ack : subscription_acks_)
          if (ack >= 0 && ack == subscription_ids_[i]) subscription_mask_ |= 1U << i;
        if (subscription_mask_ == 7 && !registration_sent) {
          register_now = true;
          registration_sent = true;
          registration_deadline = now + 3000;
        }
        if (registration_sent && !registered_ && now >= registration_deadline) return {.error = ElegooError::timeout};
        if (registered_) {
          if (!queried) {
            attribute_id_ = next_id++;
            attribute_request = attribute_id_;
            queried = true;
            next_ping = now + 10000;
            last_pong_ = now;
          }
          if (!status_id_ && (now >= next_query || (reducer_.needs_baseline() && now + 4000 >= next_query))) {
            status_id_ = next_id++;
            status_request = status_id_;
            query_deadline = now + kRequestTimeoutMs;
            next_query = now + kFullRefreshMs;
          }
          if (status_id_ && now >= query_deadline) return {.error = ElegooError::timeout};
          if (now >= next_ping) {
            ping_now = true;
            next_ping = now + 10000;
          }
          if (now - last_pong_ > 65000) return {.error = ElegooError::timeout};
          if (reducer_.baseline_ready()) {
            reducer_.snapshot_into(published_snapshot_, now);
            if (published_snapshot_.link != core::LinkState::online) return {.error = ElegooError::timeout};
            if (identity) *identity = reducer_.identity();
            if (!publish) return {.snapshot = std::move(published_snapshot_)};
            if (now >= next_publish) {
              publish_now = true;
              next_publish = now + 200;
            }
          }
        }
      }
      // ESP-MQTT dispatches events under its own API lock. Never acquire that
      // lock while holding the reducer mutex: callback lock order is reversed.
      if (subscribe_now) {
        const std::array<int, 3> ids{
          esp_mqtt_client_subscribe(client_, response_topic_.c_str(), 0),
          esp_mqtt_client_subscribe(client_, status_topic_.c_str(), 0),
          esp_mqtt_client_subscribe(client_, registration_topic_.c_str(), 0)};
        for (const int id : ids) if (id < 0) return {.error = ElegooError::unavailable};
        const std::lock_guard<std::mutex> lock(mutex_);
        subscription_ids_ = ids;
      }
      if (register_now && !send(register_to_, "{\"client_id\":\"" + client_id_ + "\",\"request_id\":\"" + registration_id_ + "\"}"))
        return {.error = ElegooError::unavailable};
      if (attribute_request && !request(1001, attribute_request)) return {.error = ElegooError::unavailable};
      if (status_request && !request(1002, status_request)) return {.error = ElegooError::unavailable};
      if (ping_now && !send(request_topic_, "{\"type\":\"PING\"}")) return {.error = ElegooError::unavailable};
      if (publish_now && !cancelled_now(cancelled)) publish(published_snapshot_);
      delay();
    }
  }
 private:
  bool send(const std::string& topic, const std::string& body) {
    return !closing_ && esp_mqtt_client_publish(client_, topic.c_str(), body.c_str(), body.size(), 0, 0) >= 0;
  }
  bool request(unsigned method, std::uint32_t id) {
    if (method != 1001 && method != 1002) return false;
    return send(request_topic_, "{\"id\":" + std::to_string(id) + ",\"method\":" + std::to_string(method) + ",\"params\":{}}");
  }
  static void event(void* context, esp_event_base_t, std::int32_t id, void* data) {
    auto* self = static_cast<Session*>(context);
    if (self->closing_) return;
    auto* value = static_cast<esp_mqtt_event_handle_t>(data);
    if (!value) return;
    const std::lock_guard<std::mutex> lock(self->mutex_);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
      case MQTT_EVENT_CONNECTED: self->connected_ = true; break;
      case MQTT_EVENT_SUBSCRIBED:
        for (int& ack : self->subscription_acks_) if (ack < 0) { ack = value->msg_id; break; }
        break;
      case MQTT_EVENT_DISCONNECTED:
        if (self->error_ == ElegooError::none) self->error_ = ElegooError::unavailable;
        break;
      case MQTT_EVENT_ERROR:
        if (self->error_ == ElegooError::none) {
          const auto* error = value->error_handle;
          const bool authorization = error && error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
              (error->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
               error->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);
          self->error_ = authorization ? ElegooError::authorization : ElegooError::unavailable;
        }
        break;
      case MQTT_EVENT_DATA: self->data(value); break;
      default: break;
    }
  }
  void data(const esp_mqtt_event_t* event) {
    if (event->retain || event->total_data_len <= 0 ||
        event->total_data_len > static_cast<int>(kElegooCc2MaximumMessageBytes) ||
        event->data_len < 0 || (event->data_len && !event->data) || event->current_data_offset < 0) {
      incoming_.reset(); incoming_size_ = 0; return;
    }
    if (!event->current_data_offset) {
      incoming_.reset(); incoming_size_ = 0;
      if (!event->topic || event->topic_len <= 0 || event->topic_len > 160) return;
      incoming_topic_.assign(event->topic, event->topic_len);
      if (incoming_topic_ != status_topic_ && incoming_topic_ != response_topic_ && incoming_topic_ != registration_topic_) return;
      incoming_total_ = static_cast<std::size_t>(event->total_data_len);
      incoming_.reset(static_cast<char*>(heap_caps_malloc(incoming_total_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
      if (!incoming_) { error_ = ElegooError::capacity; return; }
    }
    if (!incoming_ || event->total_data_len != static_cast<int>(incoming_total_) ||
        static_cast<std::size_t>(event->current_data_offset) != incoming_size_ ||
        static_cast<std::size_t>(event->data_len) > incoming_total_ - incoming_size_) {
      incoming_.reset(); incoming_size_ = 0; return;
    }
    if (event->data_len > 0)
      std::memcpy(incoming_.get() + incoming_size_, event->data, event->data_len);
    incoming_size_ += event->data_len;
    if (incoming_size_ != incoming_total_) return;
    const std::string_view body(incoming_.get(), incoming_size_);
    if (incoming_topic_ == registration_topic_) {
      bool capacity = false;
      registered_ = elegoo_cc2_registration_accepted(body, client_id_, capacity);
      if (!registered_) error_ = capacity ? ElegooError::capacity : ElegooError::unsupported_response;
    } else if (registered_) {
      if (incoming_topic_ == response_topic_ && elegoo_cc2_pong(body)) last_pong_ = now_ms();
      else {
        ElegooCc2Update result = ElegooCc2Update::ignored;
        if (incoming_topic_ == status_topic_) result = reducer_.apply(body, now_ms());
        else {
          if (status_id_) result = reducer_.apply(body, now_ms(), status_id_);
          if ((result == ElegooCc2Update::ignored || !status_id_) && attribute_id_) {
            result = reducer_.apply(body, now_ms(), attribute_id_);
            if (result == ElegooCc2Update::attributes) attribute_id_ = 0;
          } else if (result == ElegooCc2Update::status) status_id_ = 0;
        }
        if (result == ElegooCc2Update::identity_mismatch) error_ = ElegooError::identity_mismatch;
        else if (result == ElegooCc2Update::invalid) error_ = ElegooError::unsupported_response;
      }
    }
    incoming_.reset(); incoming_size_ = 0;
  }
  core::PrinterProfile profile_;
  std::string host_, client_id_, registration_id_;
  std::string response_topic_, status_topic_, registration_topic_, register_to_, request_topic_;
  std::string incoming_topic_;
  std::unique_ptr<char, decltype(&heap_caps_free)> incoming_{nullptr, heap_caps_free};
  std::size_t incoming_total_ = 0, incoming_size_ = 0;
  ElegooCc2Reducer reducer_;
  core::PrinterSnapshot published_snapshot_;
  std::mutex mutex_;
  esp_mqtt_client_handle_t client_ = nullptr;
  std::array<int, 3> subscription_ids_{-1, -1, -1};
  std::array<int, 3> subscription_acks_{-1, -1, -1};
  std::atomic<bool> closing_{false};
  std::uint32_t attribute_id_ = 0, status_id_ = 0;
  std::uint64_t last_pong_ = 0;
  unsigned subscription_mask_ = 0;
  bool connected_ = false, registered_ = false;
  ElegooError error_ = ElegooError::none;
};
struct SessionDelete {
  void operator()(Session* value) const { if (value) { value->~Session(); heap_caps_free(value); } }
};
ElegooPollResult communicate(const core::PrinterProfile& profile, std::uint64_t deadline,
    const std::function<bool()>& cancelled, ElegooIdentity* identity,
    const std::function<void(const core::PrinterSnapshot&)>& publish = {}) {
  if (profile.protocol != core::PrinterProtocol::elegoo_cc2 || profile.access_code.empty() ||
      profile.access_code.size() > 32 ||
      std::any_of(profile.access_code.begin(), profile.access_code.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }) ||
      (!profile.serial.empty() && !elegoo_cc2_serial_valid(profile.serial))) return {.error = ElegooError::invalid_configuration};
  ElegooCc2Discovery discovery;
  std::string host;
  const auto identified = elegoo_cc2_identify(profile.endpoint, profile.serial,
      std::min(deadline, now_ms() + kIdentityTimeoutMs), cancelled, discovery, host);
  if (identified != ElegooError::none) return {.error = identified};
  HostLease lease(host);
  if (!lease.held()) return {.error = ElegooError::capacity};
  void* memory = heap_caps_malloc(sizeof(Session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) return {.error = ElegooError::capacity};
  std::unique_ptr<Session, SessionDelete> session(new (memory) Session(profile, discovery.identity, host));
  return session->run(deadline, cancelled, identity, publish);
}
}  // namespace

ElegooError elegoo_cc2_identify(std::string_view endpoint, std::string_view serial,
    std::uint64_t deadline, const std::function<bool()>& cancelled,
    ElegooCc2Discovery& discovery, std::string& numeric_host) {
  discovery = {}; numeric_host.clear();
  const auto host = elegoo_cc2_host(endpoint);
  if (!host || (!serial.empty() && !elegoo_cc2_serial_valid(serial))) return ElegooError::invalid_configuration;
  const auto address = resolve(*host, deadline, cancelled);
  if (address.empty()) return cancelled_now(cancelled) ? ElegooError::cancelled : ElegooError::unavailable;
  sockaddr_in destination{};
  destination.sin_family = AF_INET; destination.sin_port = htons(52700);
  if (inet_pton(AF_INET, address.c_str(), &destination.sin_addr) != 1) return ElegooError::invalid_configuration;
  const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) return ElegooError::capacity;
  const timeval send_timeout{0, 100000};
  if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) != 0) {
    close(socket_fd); return ElegooError::unavailable;
  }
  constexpr char request[] = "{\"id\":0,\"method\":7000}";
  std::array<char, 4097> buffer{};
  ElegooError result = ElegooError::timeout;
  std::uint64_t next_send = 0;
  unsigned attempts = 0;
  while (!expired(deadline, cancelled)) {
    const auto now = now_ms();
    if (attempts < 2 && now >= next_send) {
      if (sendto(socket_fd, request, sizeof(request) - 1, 0,
          reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) < 0) break;
      ++attempts; next_send = now + 750;
    }
    fd_set readable; FD_ZERO(&readable); FD_SET(socket_fd, &readable);
    timeval wait{0, 20000};
    if (select(socket_fd + 1, &readable, nullptr, nullptr, &wait) <= 0) continue;
    sockaddr_in sender{}; socklen_t size = sizeof(sender);
    const auto count = recvfrom(socket_fd, buffer.data(), buffer.size(), 0,
        reinterpret_cast<sockaddr*>(&sender), &size);
    if (count <= 0 || count > 4096 || sender.sin_family != AF_INET ||
        sender.sin_addr.s_addr != destination.sin_addr.s_addr || sender.sin_port != destination.sin_port) continue;
    const auto decoded = parse_elegoo_cc2_discovery(std::string_view(buffer.data(), count));
    if (!decoded) continue;
    if (!serial.empty() && decoded->identity.serial != serial) { result = ElegooError::identity_mismatch; break; }
    if (!decoded->lan_mode) { result = ElegooError::service_not_ready; break; }
    discovery = *decoded; numeric_host = address; result = ElegooError::none; break;
  }
  close(socket_fd);
  return cancelled_now(cancelled) ? ElegooError::cancelled : result;
}

ElegooPollResult elegoo_cc2_probe(const core::PrinterProfile& profile,
    std::uint64_t deadline, const std::function<bool()>& cancelled, ElegooIdentity* identity) {
  return communicate(profile, deadline, cancelled, identity);
}

void ElegooCc2Adapter::configure(const core::PrinterProfile* profile) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto next = profile && profile->protocol == core::PrinterProtocol::elegoo_cc2 ? *profile : core::PrinterProfile{};
  if (core::same_printer_connection(profile_, next)) return;
  profile_ = next; ++generation_;
  snapshots_.invalidate(next.id, next.id ? core::LinkState::connecting : core::LinkState::stopped);
  if (task_) xTaskNotifyGive(task_);
}
esp_err_t ElegooCc2Adapter::start(const core::PrinterProfile* profile, const NetworkService& network) {
  configure(profile);
  const std::lock_guard<std::mutex> lock(mutex_);
  if (running_) return stopping_ ? ESP_ERR_INVALID_STATE : ESP_OK;
  if (!profile_.id || profile_.serial.empty()) return ESP_ERR_INVALID_ARG;
  network_ = &network; stopping_ = false; running_ = true;
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "elegoo_cc2", 49152, this, 4,
      &task_, kServiceCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    task_ = nullptr; running_ = false; return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
void ElegooCc2Adapter::stop() {
  const std::lock_guard<std::mutex> lock(mutex_);
  stopping_ = true;
  if (task_) xTaskNotifyGive(task_);
}
void ElegooCc2Adapter::task_entry(void* context) {
  static_cast<ElegooCc2Adapter*>(context)->run();
  vTaskDeleteWithCaps(nullptr);
}
void ElegooCc2Adapter::run() {
  while (!stopping_) {
    core::PrinterProfile profile;
    std::uint32_t generation;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      profile = profile_; generation = generation_;
    }
    if (!profile.id) break;
    const auto cancelled = [&] { return stopping_.load() || generation != generation_ || !network_->status().station_connected; };
    const auto publish = [&](const core::PrinterSnapshot& snapshot) {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!cancelled()) snapshots_.replace(snapshot);
    };
    const auto result = communicate(profile, std::numeric_limits<std::uint64_t>::max(), cancelled, nullptr, publish);
    if (!stopping_ && generation == generation_) {
      const auto link = network_->status().station_connected ? core::LinkState::failed : core::LinkState::waiting_for_network;
      const std::lock_guard<std::mutex> lock(mutex_);
      if (generation == generation_) snapshots_.invalidate(profile.id, link, now_ms());
    }
    const unsigned backoff = result.error == ElegooError::authorization || result.error == ElegooError::identity_mismatch ? 30000 : 4000 + esp_random() % 1000;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff));
  }
  ESP_LOGI("elegoo_cc2", "Stopped worker stack high-water=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  const std::lock_guard<std::mutex> lock(mutex_);
  task_ = nullptr; running_ = false;
}

}  // namespace printdeck::platform
