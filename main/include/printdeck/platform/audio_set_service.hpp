#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "printdeck/core/set_catalog.hpp"
#include "printdeck/core/audio_pack.hpp"
#include "printdeck/core/reaction_sd_store.hpp"
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
  // Called by the SD worker under the shared storage lock.
  void select_storage(const std::string& directory);
 private:
  static void task_entry(void* context);
  void install();
  bool load_current(const std::string& directory = "/assets/audio/current");
  mutable std::mutex mutex_, file_mutex_;
  const NetworkService* network_=nullptr;
  AudioSetSnapshot state_;
  bool committing_ = false;
  core::DownloadableSet requested_;
  std::string active_directory_ = "/assets/audio/current";
  core::ReactionGif sd_pack_;
  std::unique_ptr<core::AudioPackIndex> sd_pack_index_;
  std::atomic<bool> pending_{false}, running_{false}, cancelled_{false};
  TaskHandle_t task_=nullptr;
  std::atomic<bool> finished_{false};

};
}  // namespace printdeck::platform
