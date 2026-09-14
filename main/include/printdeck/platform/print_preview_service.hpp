#pragma once
#include <atomic>
#include <mutex>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/device_state.hpp"
#include "printdeck/platform/print_preview_cache.hpp"
#include "printdeck/platform/persistence_worker.hpp"

namespace printdeck::platform {
class PrintPreviewService {
 public:
  PrintPreviewService() = default;
  explicit PrintPreviewService(PrintPreviewCache cache) : cache_(std::move(cache)) {}
  struct Snapshot {
    std::string key;
    std::shared_ptr<std::vector<std::uint8_t>> image;
    bool fetch_needed = false;
  };
  static std::string key(const core::PrinterProfile&, const core::JobState&);
  esp_err_t start(PersistenceWorker& worker);
  // Asynchronous: cleanup runs on the internal-stack worker. Objects must
  // outlive the worker; Runtime owns both for the entire application lifetime.
  void stop();
  void update(const core::PrinterProfile*, const core::PrinterSnapshot&, bool visible);
  Snapshot snapshot() const;
 private:
  struct Request {
    std::string key, printer;
    core::JobPhase phase = core::JobPhase::unknown;
    bool online = false, visible = false;
    std::shared_ptr<std::vector<std::uint8_t>> image;
  };
  static std::uint32_t work_entry(void*);
  static void cleanup_entry(void*);
  std::uint32_t process();
  void cleanup();
  mutable std::mutex mutex_;
  Request request_;
  Snapshot snapshot_;
  std::uint32_t revision_ = 0;
  PersistenceWorker* worker_ = nullptr;
  bool stopped_ = false;
  PrintPreviewCache cache_;
  // Only the persistence worker touches transfer state or the resident image.
  PrintPreviewCache::Transfer transfer_;
  bool transferring_ = false, writing_ = false;
  std::uint32_t transfer_revision_ = 0;
  std::string current_key_, current_printer_, cleared_key_;
  std::shared_ptr<std::vector<std::uint8_t>> resident_, transfer_image_;
};
}  // namespace printdeck::platform
