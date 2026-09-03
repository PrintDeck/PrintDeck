#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/job_state.hpp"
#include "printdeck/core/reactions.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {

struct ReactionSetDefinition {
  std::string_view id;
  std::string_view name;
  std::string_view version;
};

struct ReactionAssetSnapshot {
  bool available = false;
  bool busy = false;
  bool cancellable = false;
  int progress_percent = 0;
  std::string detail;
  std::string active_set_id;
  std::string active_set_name;
  std::string active_set_version;
  std::string installing_set_id;
  std::string installing_set_name;
  std::size_t storage_total = 0;
  std::size_t storage_used = 0;
  std::size_t active_bytes = 0;
  std::size_t maximum_file_bytes = 0;
  std::size_t maximum_set_bytes = 0;
  std::size_t maximum_custom_bytes = 0;
  std::size_t storage_available_for_upload = 0;
  std::array<std::size_t, core::kReactionEventCount> effective_bytes{};
  std::uint32_t generation = 0;
};

class ReactionAssetService {
 public:
  esp_err_t start(const NetworkService& network);
  ReactionAssetSnapshot snapshot() const;
  std::uint32_t generation() const;
  static const std::array<ReactionSetDefinition, 9>& sets();
  bool request_set(std::string_view id);
  bool cancel_set();
  bool event_enabled(std::string_view id) const;
  bool custom_override(std::string_view id) const;
  esp_err_t set_event_enabled(std::string_view id, bool enabled);
  esp_err_t install_custom(std::string_view id, std::span<const std::uint8_t> bytes);
  esp_err_t reset_custom(std::string_view id);
  std::string effective_lvgl_path(core::PrinterActivity activity) const;
  std::string effective_vfs_path(std::string_view id) const;
  std::string preview_vfs_path(std::string_view id) const;
  std::string set_vfs_path(std::string_view id) const;

 private:
  static void reaper_task_entry(void* context);
  static void task_entry(void* context);
  static void cleanup_task_entry(void* context);
  void reaper_loop();
  void task_loop();
  void cleanup_reset_custom_files();
  void install_requested_set(std::string id);
  bool begin_set_request(std::string_view id, bool profile_migration);
  void maybe_start_profile_migration();
  void schedule_profile_migration_retry_locked();
  bool cancellation_requested() const;
  void finish_cancelled_install();
  bool load_active_manifest();
  bool validate_manifest(std::string_view body, std::string_view expected_id,
                         std::array<std::size_t, core::kReactionEventCount>& sizes,
                         std::array<std::string, core::kReactionEventCount>& hashes,
                         std::string& name, std::string& version,
                         std::size_t& total, bool allow_legacy_profile) const;
  bool download_manifest(std::string_view id, std::string& body) const;
  bool download_file(std::string_view url, const char* output_path,
                     std::size_t expected_size, std::string_view expected_sha256) const;
  void refresh_active_bytes_locked();
  void refresh_storage_locked();
  void fail(std::string detail);
  esp_err_t persist_disabled_mask_locked();
  esp_err_t persist_reset_mask_locked();
  void schedule_cleanup_locked();

  mutable std::mutex mutex_;
  // Serializes filesystem swaps while allowing readers to observe generation
  // changes and release an open LVGL decoder between rename retries.
  std::mutex filesystem_mutation_mutex_;
  const NetworkService* network_ = nullptr;
  ReactionAssetSnapshot snapshot_;
  std::uint32_t disabled_mask_ = 0;
  std::uint32_t reset_mask_ = 0;
  std::array<bool, core::kReactionEventCount> current_present_{};
  std::array<bool, core::kReactionEventCount> custom_present_{};
  std::array<std::size_t, core::kReactionEventCount> current_sizes_{};
  std::array<std::size_t, core::kReactionEventCount> custom_sizes_{};
  std::string requested_set_;
  std::string profile_migration_set_;
  std::uint64_t profile_migration_not_before_ms_ = 0;
  bool profile_migration_pending_ = false;
  bool profile_migration_attempt_active_ = false;
  std::atomic<bool> request_pending_{false};
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> worker_finished_{false};
  std::atomic<bool> cleanup_finished_{false};
  bool cleanup_followup_requested_ = false;
  TaskHandle_t reaper_task_ = nullptr;
  TaskHandle_t task_ = nullptr;
  TaskHandle_t cleanup_task_ = nullptr;
};

}  // namespace printdeck::platform
