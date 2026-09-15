#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include <map>
#include <string>
#include "mqtt_client.h"
#include "printdeck/core/settings.hpp"
#include "printdeck/platform/persistence_worker.hpp"

namespace printdeck::platform {
class WebConfig;
// One owner serializes MQTT calls; callbacks only update atomics. NVS writes
// use the existing internal-stack persistence worker.
class MqttExportService {
 public:
  void initialize(WebConfig& source, PersistenceWorker& persistence);
  std::unique_lock<std::mutex> configuration_lock() { return std::unique_lock(configuration_mutex_); }
  void tick(bool network_ready, bool enabled);
  bool connected() const { return connected_.load(); }
  bool has_registrations() const { return registration_count_.load() != 0 || persist_revision_.load()!=saved_revision_.load(); }
  bool cleanup_pending() const { return cleanup_pending_.load(); }
  const char* error() const;
  bool prepare_reset() {
    reset_requested_=true;
    if(persistence_) persistence_->cancel(PersistenceWorker::Slot::mqtt);
    return !running_ && !persist_active_;
  }
  void resume_after_reset() {
    reset_requested_=false;
    if(persistence_ && persist_revision_!=saved_revision_)
      persistence_->request(PersistenceWorker::Slot::mqtt);
  }
 private:
  static void task_entry(void*);
  static void event_entry(void*, esp_event_base_t, int32_t, void*);
  static std::uint32_t persist_entry(void*);
  void run();
  bool publish(const std::string& topic, const std::string& payload, bool retained, bool confirmed);
  bool register_id(std::uint32_t id);
  bool cleanup(std::uint32_t id);
  bool save_registry();
  bool announce(bool refresh_discovery);
  bool publish_cycle();
  void destroy_client();
  std::array<std::uint64_t, core::kMaximumProfiles + 1> registry_copy();
  WebConfig* source_ = nullptr;
  PersistenceWorker* persistence_ = nullptr;
  std::atomic<bool> initialized_{false}, reset_requested_{false}, persist_active_{false};
  std::atomic<bool> running_{false}, network_ready_{false}, enabled_{false};
  std::atomic<bool> connected_{false}, announce_requested_{false}, cleanup_pending_{false};
  std::atomic<int> error_{0}, ack_{-1};
  std::atomic<unsigned> registration_count_{0}, persist_revision_{0}, saved_revision_{0};
  std::mutex configuration_mutex_, registry_mutex_;
  std::uint32_t catalog_hash_ = 0, session_nonce_ = 0, generation_counter_ = 0;
  std::string generation_;
  std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> event_cursors_;
  std::array<std::uint64_t, core::kMaximumProfiles + 1> registry_{};
  esp_mqtt_client_handle_t client_ = nullptr;
  core::MqttSettings active_;
  std::string device_id_, root_;
};
}  // namespace printdeck::platform
