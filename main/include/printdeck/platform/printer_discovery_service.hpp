#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/settings.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {

enum class PrinterDiscoveryState { idle, scanning, complete, failed };

struct DiscoveredPrinter {
  core::PrinterProtocol protocol = core::PrinterProtocol::moonraker;
  std::string name;
  std::string model;
  std::string host;
  std::string serial;
  std::uint16_t port = 0;
  std::uint64_t last_seen_ms = 0;
  std::uint64_t retain_until_ms = 0;
  bool seen_in_current_scan = false;
};

struct PrinterDiscoverySnapshot {
  PrinterDiscoveryState state = PrinterDiscoveryState::idle;
  std::uint32_t scan_id = 0;
  int progress_percent = 0;
  std::string network_name;
  std::string detail = "Ready to search";
  std::vector<DiscoveredPrinter> printers;
};

class PrinterDiscoveryService {
 public:
  esp_err_t start(NetworkStatus network, const core::DeviceSettings& settings);
  bool cancel(std::uint32_t scan_id);
  bool running() const { return running_.load(std::memory_order_acquire); }
  PrinterDiscoverySnapshot snapshot() const;

 private:
  static void task_entry(void* context);
  void run();
  void publish_progress(std::size_t completed, std::size_t total);
  void add_result(DiscoveredPrinter result);

  mutable std::mutex mutex_;
  NetworkStatus network_;
  std::vector<std::string> saved_ipv4_hosts_;
  std::string cache_network_key_;
  PrinterDiscoverySnapshot snapshot_;
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> running_{false};
  std::uint32_t next_scan_id_ = 0;
  TaskHandle_t task_ = nullptr;
};

}  // namespace printdeck::platform
