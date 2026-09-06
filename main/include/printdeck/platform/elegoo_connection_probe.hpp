#pragma once

#include <atomic>
#include <mutex>
#include <string_view>
#include "esp_err.h"
#include "printdeck/platform/elegoo_types.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {

struct ElegooCheckSnapshot {
  std::string id;
  bool running = false;
  bool ready = false;
  ElegooError error = ElegooError::none;
  ElegooIdentity identity;
};

class ElegooConnectionProbe {
 public:
  esp_err_t start(core::PrinterProfile profile, const NetworkService& network);
  bool accept_live(core::PrinterProfile profile, ElegooIdentity identity);
  void cancel(std::string_view id);
  ElegooCheckSnapshot snapshot() const;
  bool verified(const core::PrinterProfile& profile, std::string_view id) const;
 private:
  static void task_entry(void* context);
  void run();
  mutable std::mutex mutex_;
  core::PrinterProfile profile_;
  ElegooCheckSnapshot snapshot_;
  std::atomic<bool> cancelled_{false};
  const NetworkService* network_ = nullptr;
  std::uint64_t verified_until_ = 0;
};

}  // namespace printdeck::platform
