#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/platform/elegoo_cc2_parser.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {

// Targeted, credential-free UDP identification. The result comes from the
// selected numeric IPv4; never adopts an address advertised in JSON.
ElegooError elegoo_cc2_identify(std::string_view endpoint,
    std::string_view expected_serial, std::uint64_t deadline_ms,
    const std::function<bool()>& cancelled, ElegooCc2Discovery& discovery,
    std::string& numeric_host);

ElegooPollResult elegoo_cc2_probe(const core::PrinterProfile& profile,
    std::uint64_t deadline_ms, const std::function<bool()>& cancelled,
    ElegooIdentity* identity = nullptr);

class ElegooCc2Adapter {
 public:
  esp_err_t start(const core::PrinterProfile* profile, const NetworkService& network);
  void configure(const core::PrinterProfile* profile);
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  core::PrinterSnapshot snapshot() const { return snapshots_.read(); }
  void snapshot_into(core::PrinterSnapshot& destination) const { snapshots_.read_into(destination); }
 private:
  static void task_entry(void* context);
  void run();
  mutable std::mutex mutex_;
  core::PrinterProfile profile_;
  core::SnapshotStore snapshots_;
  const NetworkService* network_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<std::uint32_t> generation_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
};

}  // namespace printdeck::platform
