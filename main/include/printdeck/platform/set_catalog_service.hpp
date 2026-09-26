#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "printdeck/core/set_catalog.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {
// Catalog refresh and set installation share one bounded transfer budget.
class AssetTransferLease {
 public:
  AssetTransferLease();
  ~AssetTransferLease();
  explicit operator bool() const {return acquired_;}
  AssetTransferLease(const AssetTransferLease&) = delete;
  AssetTransferLease& operator=(const AssetTransferLease&) = delete;
 private:
  bool acquired_;
};
inline constexpr const char* kAudioSetOrigin = "https://raw.githubusercontent.com/PrintDeck/PrintDeck/main/audio-sets/";
inline constexpr const char* kReactionSetOrigin = "https://raw.githubusercontent.com/PrintDeck/PrintDeck/main/reaction-sets/";

// Starts no permanent task. Catalogs are loaded once, refreshed in a transient
// core-0 worker and atomically replaced only after complete validation.
class SetCatalogService {
 public:
  static SetCatalogService& instance();
  void start(const NetworkService& network);
  void poll();
  void request_refresh();
  void cancel();
  std::vector<core::DownloadableSet> sets(bool audio) const;
  bool online() const {return network_ && network_->status().station_connected;}
  bool busy() const {return running_.load();}
  std::string error() const;
 private:
  static void task_entry(void* context);
  void refresh();
  void load(bool audio);
  const NetworkService* network_ = nullptr;
  mutable std::mutex mutex_;
  std::vector<core::DownloadableSet> reactions_, audio_;
  std::string error_;
  std::atomic<bool> requested_{false}, cancelled_{false}, running_{false};
  std::uint64_t next_refresh_ms_ = 0;
  TaskHandle_t task_=nullptr;
  std::atomic<bool> finished_{false};

};
}  // namespace printdeck::platform
