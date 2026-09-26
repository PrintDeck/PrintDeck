#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "printdeck/core/set_catalog.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {
struct AudioSetSnapshot {
  bool busy=false, available=false;
  int progress=0;
  std::uint32_t generation=0;
  std::string id, style, language, version, requested_id, request_id, error;
};
class AudioSetService {
 public:
  static AudioSetService& instance();
  void start(const NetworkService& network);
  void poll();
  bool request(std::string_view id, std::string_view request_id={});
  bool cancel(std::string_view request_id={});
  AudioSetSnapshot snapshot() const;
  bool read_sample(std::string_view style, std::string_view language, std::string_view event,
                   std::vector<std::uint8_t>& gzip, std::size_t& decoded) const;
  static std::size_t storage_bytes();
  static std::size_t storage_budget();
  static std::size_t storage_reserve();
 private:
  static void task_entry(void* context);
  void install();
  bool load_current();
  mutable std::mutex mutex_, file_mutex_;
  const NetworkService* network_=nullptr;
  AudioSetSnapshot state_;
  core::DownloadableSet requested_;
  std::atomic<bool> pending_{false}, running_{false}, cancelled_{false};
  TaskHandle_t task_=nullptr;
  std::atomic<bool> finished_{false};

};
}  // namespace printdeck::platform
