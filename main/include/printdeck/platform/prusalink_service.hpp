#pragma once

#include <atomic>
#include <mutex>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/platform/network_service.hpp"
#include "printdeck/platform/prusalink_http_transport.hpp"

namespace printdeck::platform {

PrusaLinkCredentials prusalink_credentials(const core::PrinterProfile& profile);
PrusaLinkPollResult prusalink_probe(const core::PrinterProfile& profile,
    std::uint64_t deadline, const std::function<bool()>& cancelled,
    PrusaLinkIdentity* identity = nullptr);

class PrusaLinkAdapter {
 public:
  esp_err_t start(const core::PrinterProfile* profile, const NetworkService& network);
  void configure(const core::PrinterProfile* profile);
  void stop();
  bool running() const { return running_.load(); }
  core::PrinterSnapshot snapshot() const { return snapshots_.read(); }
  void snapshot_into(core::PrinterSnapshot& value) const { snapshots_.read_into(value); }
 private:
  static void task_entry(void* context);
  void run();
  mutable std::mutex mutex_;
  core::PrinterProfile profile_;
  core::SnapshotStore snapshots_;
  const NetworkService* network_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint32_t> generation_{0};
};

struct PrusaLinkCheckSnapshot {
  std::string id;
  bool running = false;
  bool ready = false;
  PrusaLinkError error = PrusaLinkError::none;
  PrusaLinkIdentity identity;
};

class PrusaLinkConnectionProbe {
 public:
  esp_err_t start(core::PrinterProfile profile, const NetworkService& network);
  void cancel(std::string_view id);
  PrusaLinkCheckSnapshot snapshot() const;
  bool verified(const core::PrinterProfile& profile, std::string_view id) const;
 private:
  static void task_entry(void* context);
  void run();
  mutable std::mutex mutex_;
  core::PrinterProfile profile_;
  PrusaLinkCheckSnapshot snapshot_;
  std::atomic<bool> cancelled_{false};
  const NetworkService* network_ = nullptr;
  std::uint64_t verified_until_ = 0;
};

}  // namespace printdeck::platform
