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
#include "printdeck/core/reaction_sd_store.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {

struct ReactionSetDefinition {
  std::string_view id;
  std::string_view name;
  std::string_view version;
  std::string_view family_id;
  std::string_view family_name;
  std::string_view variant_name;
};

struct ReactionAssetSnapshot {
  bool sd_supported = false;
  bool sd_ready = false;
  bool sd_ejected = false;
  bool sd_busy = false;
  bool sd_migration_available = false;
  bool sd_migration_prompt = false;
  std::uint32_t sd_session = 0;
  std::size_t sd_missing = 0;
  std::uint64_t sd_total = 0;
  std::uint64_t sd_free = 0;
  std::size_t sd_image_bytes = 0;
  bool sd_selected = false;
  bool sd_can_copy_internal = false;
  bool sd_can_enable = true;
  std::uint32_t sd_conflicts = 0;
  std::string sd_detail;
  bool available = false;
  bool busy = false;
  bool cancellable = false;
  bool install_failed = false;
  int progress_percent = 0;
  std::string detail;
  std::string active_set_id;
  std::string active_set_name;
  std::string active_set_version;
  std::string request_id;
  std::string upload_event;
  bool upload_success = false;
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
  std::array<bool, core::kReactionEventCount> effective_custom{};
  // Browser previews change only when their effective image changes. The
  // general generation also wakes the device decoder for non-image changes.
  std::array<std::uint32_t, core::kReactionEventCount> preview_generations{};
  std::uint32_t preview_session = 0;
  std::uint32_t generation = 0;
};

class ReactionAssetService {
 public:
  esp_err_t start(const NetworkService& network);
  // Set during startup, before start(); successful explicit SD or set changes.
  void set_storage_changed_callback(void (*callback)(void*), void* context) {
    storage_changed_ = callback; storage_context_ = context;
  }
  ReactionAssetSnapshot snapshot() const;
  std::uint32_t generation() const;
  static std::span<const ReactionSetDefinition> sets();
  bool request_set(std::string_view id, std::string_view request_id = {});
  bool request_storage(std::string_view action, std::uint32_t session = 0);
  core::ReactionGif cached_sd_gif(std::string_view id) const;
  bool read_custom_gif(std::string_view id, std::span<std::uint8_t> destination) const;
  bool cancel_set(std::string_view expected_set = {}, std::string_view expected_request = {});
  bool event_enabled(std::string_view id) const;
  bool custom_override(std::string_view id) const;
  esp_err_t set_event_enabled(std::string_view id, bool enabled);
  esp_err_t install_custom(std::string_view id, std::span<const std::uint8_t> bytes, std::string_view upload_request = {});
  bool begin_cloud_upload(std::string_view event, std::string_view request, std::string_view set, std::string_view revision, std::uint32_t session);
  bool finish_cloud_upload(std::string_view request, std::span<const std::uint8_t> bytes);
  esp_err_t reset_custom(std::string_view id);
  esp_err_t prepare_factory_reset();
  std::string effective_lvgl_path(core::PrinterActivity activity) const;
  std::string effective_vfs_path(std::string_view id) const;
  std::string preview_vfs_path(std::string_view id) const;
  std::string set_vfs_path(std::string_view id) const;

 private:
  static void reaper_task_entry(void* context);
  static void task_entry(void* context);
  static void cleanup_task_entry(void* context);
  void reaper_loop();
  void poll_storage();
  void storage_task(std::string action);
  void sync_sd_locked();
  bool initialize_sd_index();
  bool persist_sd_index(const core::ReactionSdIndex& index);
  void task_loop();
  void cleanup_reset_custom_files();
  void install_requested_set(std::string id);
  bool begin_set_request(std::string_view id, bool profile_migration, std::string_view request_id = {});
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
  void refresh_set_preview_generations_locked();
  void refresh_storage_locked();
  void fail(std::string detail);
  esp_err_t persist_disabled_mask_locked();
  esp_err_t persist_reset_mask_locked();
  void schedule_cleanup_locked();

  mutable std::mutex mutex_;
  // Serializes filesystem swaps while allowing readers to observe generation
  // changes and release an open LVGL decoder between rename retries.
  mutable std::mutex filesystem_mutation_mutex_;
  const NetworkService* network_ = nullptr;
  ReactionAssetSnapshot snapshot_;
  std::uint32_t disabled_mask_ = 0;
  std::uint32_t reset_mask_ = 0;
  std::array<bool, core::kReactionEventCount> current_present_{};
  std::array<bool, core::kReactionEventCount> custom_present_{};
  std::array<std::size_t, core::kReactionEventCount> current_sizes_{};
  std::array<std::size_t, core::kReactionEventCount> custom_sizes_{};
  void (*storage_changed_)(void*) = nullptr;
  void* storage_context_ = nullptr;
  core::ReactionSdStore sd_store_;
  core::ReactionGifArray sd_cache_{};
  std::uint32_t sd_owned_mask_ = 0;
  bool sd_index_valid_ = false;
  bool sd_initialized_ = false;
  int sd_mode_override_ = -1;
  std::array<bool, core::kReactionEventCount> flash_custom_present_{};
  std::array<std::size_t, core::kReactionEventCount> flash_custom_sizes_{};
  std::string requested_storage_;
  std::uint64_t sd_poll_after_ms_ = 0;
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
