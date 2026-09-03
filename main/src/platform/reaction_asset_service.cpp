#include "printdeck/platform/reaction_asset_service.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "lvgl.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {
namespace {

constexpr char kTag[] = "reaction_assets";
constexpr char kMountPath[] = "/assets";
constexpr char kPartitionLabel[] = "assets";
constexpr char kRootPath[] = "/assets/reactions";
constexpr char kCurrentPath[] = "/assets/reactions/current";
constexpr char kPreviousPath[] = "/assets/reactions/previous";
constexpr char kStagingPath[] = "/assets/reactions/staging";
constexpr char kCustomPath[] = "/assets/reactions/custom";
constexpr char kNvsNamespace[] = "pd_reactions";
constexpr char kDisabledMaskKey[] = "disabled";
constexpr char kResetMaskKey[] = "reset";
constexpr std::size_t kMaximumManifestBytes = 8 * 1024;
constexpr std::size_t kMaximumGifBytes = 1536 * 1024;
constexpr std::size_t kMaximumActiveReactionBytes = 1536 * 1024;
constexpr std::uint16_t kMaximumCustomGifFrames = 10;
constexpr std::size_t kStorageSafetyBytes = 128 * 1024;
constexpr std::uint32_t kReaperStackBytes = 3072;
constexpr std::uint64_t kProfileMigrationInitialDelayMs = 15'000;
constexpr std::uint64_t kProfileMigrationRetryDelayMs = 5 * 60 * 1000;
constexpr char kSetBaseUrl[] =
    "https://raw.githubusercontent.com/PrintDeck/PrintDeck/main/reaction-sets/";
constexpr std::string_view kSetAssetFamily =
    kDisplayWidth == 466 ? "466x466"
                         : kDisplayIsRound ? "240x240-round" : "240x240";
constexpr std::string_view kSetAssetProfile = kBoardVariant;
constexpr bool kNeedsRound240ProfileMigration =
    kDisplayIsRound && kDisplayWidth == 240 && kDisplayHeight == 240;

constexpr std::array<ReactionSetDefinition, 9> kSets = {{
    {"alloy_iris_green", "Green", "4.0.0"},
    {"alloy_iris_blue", "Blue", "4.0.0"},
    {"alloy_iris_brown", "Brown", "4.0.0"},
    {"alloy_iris_amber", "Amber", "4.0.0"},
    {"alloy_iris_gray", "Gray", "4.0.0"},
    {"alloy_iris_hazel", "Hazel", "4.0.0"},
    {"alloy_iris_red", "Red", "4.0.0"},
    {"alloy_iris_violet", "Violet", "4.0.0"},
    {"alloy_iris_cyan", "Cyan", "4.0.0"},
}};

std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

bool known_set(std::string_view id) {
  return std::any_of(kSets.begin(), kSets.end(), [id](const auto& set) {
    return set.id == id;
  });
}

std::string set_asset_base_url() {
  return std::string(kSetBaseUrl) + std::string(kSetAssetFamily) + "/sets/";
}

bool regular_file(const std::string& path) {
  struct stat status {};
  return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

bool directory_exists(const char* path) {
  struct stat status {};
  return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

std::size_t file_size(const std::string& path) {
  struct stat status {};
  return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode)
             ? static_cast<std::size_t>(status.st_size)
             : 0;
}

void ensure_directory(const char* path) {
  if (mkdir(path, 0755) != 0 && errno != EEXIST) {
    ESP_LOGW(kTag, "Could not create %s: errno %d", path, errno);
  }
}

bool remove_tree(const char* path) {
  DIR* directory = opendir(path);
  if (directory == nullptr) {
    if (errno == ENOENT) return true;
    return unlink(path) == 0 || errno == ENOENT;
  }
  bool removed = true;
  while (dirent* entry = readdir(directory)) {
    if (std::strcmp(entry->d_name, ".") == 0 ||
        std::strcmp(entry->d_name, "..") == 0) continue;
    const std::string child = std::string(path) + "/" + entry->d_name;
    struct stat status {};
    if (stat(child.c_str(), &status) == 0 && S_ISDIR(status.st_mode)) {
      removed = remove_tree(child.c_str()) && removed;
    } else if (unlink(child.c_str()) != 0 && errno != ENOENT) {
      removed = false;
    }
  }
  if (closedir(directory) != 0) removed = false;
  if (rmdir(path) != 0 && errno != ENOENT) removed = false;
  return removed;
}

const char* path_kind(const char* path) {
  struct stat status {};
  if (stat(path, &status) != 0) return errno == ENOENT ? "missing" : "error";
  if (S_ISDIR(status.st_mode)) return "directory";
  if (S_ISREG(status.st_mode)) return "file";
  return "other";
}

void log_rename_failure(const char* operation, int error) {
  std::size_t total = 0;
  std::size_t used = 0;
  esp_littlefs_info(kPartitionLabel, &total, &used);
  ESP_LOGW(kTag,
           "%s failed: errno=%d (%s), current=%s, previous=%s, staging=%s, "
           "storage=%u/%u",
           operation, error, std::strerror(error), path_kind(kCurrentPath),
           path_kind(kPreviousPath), path_kind(kStagingPath),
           static_cast<unsigned>(used), static_cast<unsigned>(total));
}

bool rename_with_busy_retry(const char* source, const char* destination,
                            const char* operation) {
  constexpr int kMaximumAttempts = 20;
  for (int attempt = 1; attempt <= kMaximumAttempts; ++attempt) {
    errno = 0;
    if (rename(source, destination) == 0) {
      if (attempt > 1) {
        ESP_LOGI(kTag, "%s succeeded after %d attempts", operation, attempt);
      }
      return true;
    }
    const int error = errno;
    if (error != EBUSY || attempt == kMaximumAttempts) {
      log_rename_failure(operation, error);
      return false;
    }
    if (attempt == 1) log_rename_failure(operation, error);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

std::size_t directory_size(const char* path) {
  std::size_t total = 0;
  DIR* directory = opendir(path);
  if (directory == nullptr) return 0;
  while (dirent* entry = readdir(directory)) {
    if (entry->d_name[0] == '.') continue;
    total += file_size(std::string(path) + "/" + entry->d_name);
  }
  closedir(directory);
  return total;
}

bool storage_has_room(std::size_t requested_bytes) {
  std::size_t total = 0;
  std::size_t used = 0;
  if (esp_littlefs_info(kPartitionLabel, &total, &used) != ESP_OK || used > total) {
    return false;
  }
  const std::size_t available = total - used;
  return requested_bytes <= available &&
         kStorageSafetyBytes <= available - requested_bytes;
}

bool write_bytes(const char* path, std::span<const std::uint8_t> bytes) {
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) return false;
  const bool ok = bytes.empty() ||
                  std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  const bool closed = std::fclose(file) == 0;
  return ok && closed;
}

bool write_text(const char* path, std::string_view text) {
  return write_bytes(path, std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(text.data()),
                               text.size()));
}

bool read_file(const std::string& path, std::size_t maximum,
               std::vector<std::uint8_t>& output) {
  output.clear();
  const std::size_t bytes = file_size(path);
  if (bytes == 0 || bytes > maximum) return false;
  output.resize(bytes);
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  const bool ok = std::fread(output.data(), 1, output.size(), file) == output.size();
  std::fclose(file);
  if (!ok) output.clear();
  return ok;
}

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool parse_sha256(std::string_view text, std::array<std::uint8_t, 32>& output) {
  if (text.size() != output.size() * 2) return false;
  for (std::size_t index = 0; index < output.size(); ++index) {
    const int high = hex_value(text[index * 2]);
    const int low = hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

struct TextResponse {
  std::string body;
  bool too_large = false;
};

esp_err_t receive_text(esp_http_client_event_t* event) {
  auto* response = static_cast<TextResponse*>(event->user_data);
  if (response == nullptr || event->event_id != HTTP_EVENT_ON_DATA ||
      event->data_len <= 0) return ESP_OK;
  if (response->body.size() + static_cast<std::size_t>(event->data_len) >
      kMaximumManifestBytes) {
    response->too_large = true;
    return ESP_FAIL;
  }
  response->body.append(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}

void* lv_reaction_open(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  if (path == nullptr || mode != LV_FS_MODE_RD || std::strstr(path, "..") != nullptr) {
    return nullptr;
  }
  std::array<char, 256> full{};
  const int written = std::snprintf(full.data(), full.size(), "%s/%s", kMountPath,
                                    path[0] == '/' ? path + 1 : path);
  if (written <= 0 || static_cast<std::size_t>(written) >= full.size()) return nullptr;
  return std::fopen(full.data(), "rb");
}

lv_fs_res_t lv_reaction_close(lv_fs_drv_t*, void* file) {
  return file != nullptr && std::fclose(static_cast<FILE*>(file)) == 0
             ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

lv_fs_res_t lv_reaction_read(lv_fs_drv_t*, void* file, void* buffer,
                             std::uint32_t requested, std::uint32_t* read) {
  if (file == nullptr || buffer == nullptr || read == nullptr) return LV_FS_RES_INV_PARAM;
  *read = static_cast<std::uint32_t>(
      std::fread(buffer, 1, requested, static_cast<FILE*>(file)));
  return std::ferror(static_cast<FILE*>(file)) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

lv_fs_res_t lv_reaction_seek(lv_fs_drv_t*, void* file, std::uint32_t position,
                             lv_fs_whence_t whence) {
  if (file == nullptr) return LV_FS_RES_INV_PARAM;
  const int origin = whence == LV_FS_SEEK_CUR ? SEEK_CUR
                   : whence == LV_FS_SEEK_END ? SEEK_END : SEEK_SET;
  return std::fseek(static_cast<FILE*>(file), static_cast<long>(position), origin) == 0
             ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

lv_fs_res_t lv_reaction_tell(lv_fs_drv_t*, void* file, std::uint32_t* position) {
  if (file == nullptr || position == nullptr) return LV_FS_RES_INV_PARAM;
  const long value = std::ftell(static_cast<FILE*>(file));
  if (value < 0) return LV_FS_RES_FS_ERR;
  *position = static_cast<std::uint32_t>(value);
  return LV_FS_RES_OK;
}

}  // namespace

const std::array<ReactionSetDefinition, 9>& ReactionAssetService::sets() {
  return kSets;
}

esp_err_t ReactionAssetService::start(const NetworkService& network) {
  if (network_ != nullptr || reaper_task_ != nullptr) return ESP_ERR_INVALID_STATE;
  network_ = &network;
  const esp_vfs_littlefs_conf_t config{
      .base_path = kMountPath,
      .partition_label = kPartitionLabel,
      .partition = nullptr,
      .format_if_mount_failed = true,
      .read_only = false,
      .dont_mount = false,
      .grow_on_mount = true,
  };
  const esp_err_t mounted = esp_vfs_littlefs_register(&config);
  if (mounted != ESP_OK) {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.detail = "Reaction storage is unavailable.";
    return mounted;
  }
  // Workers that write LittleFS need internal-RAM stacks, but a task created
  // with explicit heap capabilities cannot safely free its own stack when
  // internal memory is tight. A small PSRAM-backed reaper performs that
  // deletion from the outside after each one-shot worker has suspended.
  if (xTaskCreatePinnedToCoreWithCaps(
          reaper_task_entry, "reaction_reap", kReaperStackBytes, this, 1,
          &reaper_task_, kServiceCore,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    reaper_task_ = nullptr;
    esp_vfs_littlefs_unregister(kPartitionLabel);
    network_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  ensure_directory(kRootPath);
  ensure_directory(kCustomPath);
  // Recover the unambiguous half of an interrupted directory swap. If both
  // current and previous exist, retain the rollback copy until current has
  // passed the complete manifest, checksum and GIF validation below.
  if (!directory_exists(kCurrentPath) && directory_exists(kPreviousPath)) {
    rename_with_busy_retry(kPreviousPath, kCurrentPath,
                           "Reaction startup rollback");
  }
  if (!remove_tree(kStagingPath)) {
    ESP_LOGW(kTag, "Incomplete reaction staging cleanup will be retried later");
  }
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_u32(handle, kDisabledMaskKey, &disabled_mask_);
    nvs_get_u32(handle, kResetMaskKey, &reset_mask_);
    nvs_close(handle);
  }
  static lv_fs_drv_t driver;
  lv_fs_drv_init(&driver);
  driver.letter = 'R';
  driver.open_cb = lv_reaction_open;
  driver.close_cb = lv_reaction_close;
  driver.read_cb = lv_reaction_read;
  driver.seek_cb = lv_reaction_seek;
  driver.tell_cb = lv_reaction_tell;
  lv_fs_drv_register(&driver);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.available = true;
    snapshot_.detail = "Reaction storage is ready.";
    refresh_storage_locked();
  }
  bool active_valid = load_active_manifest();
  if (!active_valid && directory_exists(kPreviousPath)) {
    const bool removed_invalid = remove_tree(kCurrentPath);
    const bool restored = removed_invalid &&
        rename_with_busy_retry(kPreviousPath, kCurrentPath,
                               "Reaction startup recovery");
    if (restored) active_valid = load_active_manifest();
  }
  if (active_valid && !remove_tree(kPreviousPath)) {
    ESP_LOGW(kTag, "Old reaction set cleanup will be retried later");
  }
  std::array<bool, core::kReactionEventCount> custom_present{};
  std::array<std::size_t, core::kReactionEventCount> custom_sizes{};
  for (std::size_t index = 0; index < core::kReactionEventCount; ++index) {
    const std::string path = std::string(kCustomPath) + "/" +
                             std::string(core::reaction_events()[index].id) + ".gif";
    std::vector<std::uint8_t> bytes;
    core::GifMetadata metadata;
    custom_present[index] = (reset_mask_ & (1UL << index)) == 0 &&
                            read_file(path, snapshot_.maximum_file_bytes, bytes) &&
                            core::inspect_gif(bytes, metadata, kDisplayWidth,
                                              kMaximumCustomGifFrames);
    if (custom_present[index]) custom_sizes[index] = bytes.size();
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    custom_present_ = custom_present;
    custom_sizes_ = custom_sizes;
    refresh_active_bytes_locked();
    refresh_storage_locked();
    if (reset_mask_ != 0) schedule_cleanup_locked();
  }
  return ESP_OK;
}

ReactionAssetSnapshot ReactionAssetService::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void ReactionAssetService::refresh_active_bytes_locked() {
  snapshot_.active_bytes = 0;
  for (std::size_t index = 0; index < core::kReactionEventCount; ++index) {
    if (custom_present_[index]) {
      snapshot_.effective_bytes[index] = custom_sizes_[index];
    } else if (current_present_[index]) {
      snapshot_.effective_bytes[index] = current_sizes_[index];
    } else {
      snapshot_.effective_bytes[index] = 0;
    }
    snapshot_.active_bytes += snapshot_.effective_bytes[index];
  }
}

std::uint32_t ReactionAssetService::generation() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_.generation;
}

void ReactionAssetService::refresh_storage_locked() {
  std::size_t total = 0;
  std::size_t used = 0;
  if (esp_littlefs_info(kPartitionLabel, &total, &used) != ESP_OK || total == 0) return;
  snapshot_.storage_total = total;
  snapshot_.storage_used = used;
  const std::size_t usable = total > kStorageSafetyBytes ? total - kStorageSafetyBytes : 0;
  snapshot_.maximum_file_bytes = std::min(kMaximumGifBytes, usable);
  snapshot_.maximum_set_bytes = std::min(kMaximumActiveReactionBytes, usable);
  snapshot_.maximum_custom_bytes = std::min(kMaximumActiveReactionBytes, usable);
  snapshot_.storage_available_for_upload =
      usable > used ? usable - used : 0;
}

bool ReactionAssetService::request_set(std::string_view id) {
  return begin_set_request(id, false);
}

bool ReactionAssetService::begin_set_request(std::string_view id,
                                             bool profile_migration) {
  const auto requested = std::find_if(kSets.begin(), kSets.end(), [id](const auto& set) {
    return set.id == id;
  });
  if (requested == kSets.end()) return false;
  const std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.busy || task_ != nullptr || reaper_task_ == nullptr) return false;
  requested_set_.assign(id);
  snapshot_.installing_set_id.assign(requested->id);
  snapshot_.installing_set_name.assign(requested->name);
  snapshot_.busy = true;
  snapshot_.cancellable = true;
  snapshot_.progress_percent = 0;
  snapshot_.detail = "Preparing the reaction set…";
  cancel_requested_.store(false, std::memory_order_release);
  profile_migration_attempt_active_ = profile_migration;
  request_pending_ = true;
  // LittleFS writes may briefly disable the flash cache, so this worker needs
  // an internal-RAM stack. Allocate it only for an explicit set change instead
  // of permanently withholding that memory from Wi-Fi and printer TLS.
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "reaction_sets", 6144, this, 2,
                                     &task_, kServiceCore,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
    task_ = nullptr;
    request_pending_ = false;
    cancel_requested_.store(false, std::memory_order_release);
    if (profile_migration_attempt_active_) {
      schedule_profile_migration_retry_locked();
    }
    profile_migration_attempt_active_ = false;
    snapshot_.busy = false;
    snapshot_.cancellable = false;
    snapshot_.installing_set_id.clear();
    snapshot_.installing_set_name.clear();
    snapshot_.detail = "PrintDeck does not have enough working memory to change sets.";
    ESP_LOGW(kTag,
             "Reaction worker allocation failed; internal heap=%u, largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    return false;
  }
  return true;
}

void ReactionAssetService::schedule_profile_migration_retry_locked() {
  if (!profile_migration_pending_) return;
  profile_migration_not_before_ms_ =
      monotonic_ms() + kProfileMigrationRetryDelayMs;
}

void ReactionAssetService::maybe_start_profile_migration() {
  if constexpr (!kNeedsRound240ProfileMigration) return;
  if (network_ == nullptr || !network_->status().station_connected) return;

  std::string set_id;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!profile_migration_pending_ || profile_migration_attempt_active_ ||
        snapshot_.busy || task_ != nullptr ||
        monotonic_ms() < profile_migration_not_before_ms_) {
      return;
    }
    set_id = profile_migration_set_;
  }
  if (!set_id.empty()) begin_set_request(set_id, true);
}

bool ReactionAssetService::cancel_set() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.busy || !snapshot_.cancellable || task_ == nullptr) return false;
  cancel_requested_.store(true, std::memory_order_release);
  snapshot_.cancellable = false;
  snapshot_.detail = "Cancelling reaction set installation…";
  return true;
}

bool ReactionAssetService::event_enabled(std::string_view id) const {
  const auto* event = core::reaction_event(id);
  if (event == nullptr) return false;
  const std::size_t index = static_cast<std::size_t>(event - core::reaction_events().data());
  const std::lock_guard<std::mutex> lock(mutex_);
  return (disabled_mask_ & (1UL << index)) == 0;
}

bool ReactionAssetService::custom_override(std::string_view id) const {
  const auto* event = core::reaction_event(id);
  if (event == nullptr) return false;
  const std::size_t index = static_cast<std::size_t>(event - core::reaction_events().data());
  const std::lock_guard<std::mutex> lock(mutex_);
  return custom_present_[index];
}

esp_err_t ReactionAssetService::persist_disabled_mask_locked() {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (result == ESP_OK) result = nvs_set_u32(handle, kDisabledMaskKey, disabled_mask_);
  if (result == ESP_OK) result = nvs_commit(handle);
  if (handle != 0) nvs_close(handle);
  return result;
}

esp_err_t ReactionAssetService::persist_reset_mask_locked() {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (result == ESP_OK) result = nvs_set_u32(handle, kResetMaskKey, reset_mask_);
  if (result == ESP_OK) result = nvs_commit(handle);
  if (handle != 0) nvs_close(handle);
  return result;
}

void ReactionAssetService::schedule_cleanup_locked() {
  if (reaper_task_ == nullptr) return;
  if (cleanup_task_ != nullptr) {
    // A reset can arrive after the active cleanup has made its final mask
    // snapshot but before the reaper clears its handle. Remember that new
    // request so it receives one fresh bounded cleanup pass.
    cleanup_followup_requested_ = true;
    return;
  }
  if (xTaskCreatePinnedToCoreWithCaps(cleanup_task_entry, "reaction_cleanup", 3072,
                                     this, 1, &cleanup_task_, kServiceCore,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
    cleanup_task_ = nullptr;
    ESP_LOGW(kTag, "Custom GIF cleanup deferred until the next restart");
  }
}

esp_err_t ReactionAssetService::set_event_enabled(std::string_view id, bool enabled) {
  const auto* event = core::reaction_event(id);
  if (event == nullptr) return ESP_ERR_INVALID_ARG;
  const std::size_t index = static_cast<std::size_t>(event - core::reaction_events().data());
  const std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.busy) return ESP_ERR_INVALID_STATE;
  const std::uint32_t previous = disabled_mask_;
  if (enabled) disabled_mask_ &= ~(1UL << index);
  else disabled_mask_ |= 1UL << index;
  const esp_err_t result = persist_disabled_mask_locked();
  if (result != ESP_OK) disabled_mask_ = previous;
  else ++snapshot_.generation;
  return result;
}

esp_err_t ReactionAssetService::install_custom(
    std::string_view id, std::span<const std::uint8_t> bytes) {
  const auto* event = core::reaction_event(id);
  if (event == nullptr) return ESP_ERR_INVALID_ARG;
  const std::size_t index = static_cast<std::size_t>(event - core::reaction_events().data());
  core::GifMetadata metadata;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.busy) return ESP_ERR_INVALID_STATE;
    if (!snapshot_.available || bytes.empty() ||
        bytes.size() > snapshot_.maximum_file_bytes) return ESP_ERR_INVALID_SIZE;
  }
  if (!core::inspect_gif(bytes, metadata, kDisplayWidth,
                         kMaximumCustomGifFrames)) {
    return ESP_ERR_INVALID_ARG;
  }
  const std::lock_guard<std::mutex> mutation_lock(filesystem_mutation_mutex_);
  const std::string current = std::string(kCustomPath) + "/" + std::string(id) + ".gif";
  const std::string temporary = current + ".tmp";
  const std::string backup = current + ".bak";
  std::unique_lock<std::mutex> lock(mutex_);
  if (snapshot_.busy) return ESP_ERR_INVALID_STATE;
  const std::uint32_t previous_reset_mask = reset_mask_;
  reset_mask_ &= ~(1UL << index);
  if (reset_mask_ != previous_reset_mask && persist_reset_mask_locked() != ESP_OK) {
    reset_mask_ = previous_reset_mask;
    return ESP_FAIL;
  }
  const std::size_t current_size = file_size(current);
  unlink(temporary.c_str());
  const std::size_t custom_total = directory_size(kCustomPath);
  if (custom_total - current_size + bytes.size() > snapshot_.maximum_custom_bytes) {
    return ESP_ERR_NO_MEM;
  }
  std::size_t effective_total = 0;
  for (std::size_t event_index = 0; event_index < core::kReactionEventCount;
       ++event_index) {
    const std::string event_id(core::reaction_events()[event_index].id);
    if (event_index == index) {
      effective_total += bytes.size();
    } else if (custom_present_[event_index]) {
      effective_total += file_size(std::string(kCustomPath) + "/" + event_id + ".gif");
    } else if (current_present_[event_index]) {
      effective_total += file_size(std::string(kCurrentPath) + "/" + event_id + ".gif");
    }
  }
  if (effective_total > snapshot_.maximum_custom_bytes) return ESP_ERR_NO_MEM;
  if (!storage_has_room(bytes.size())) return ESP_ERR_NO_MEM;
  if (!write_bytes(temporary.c_str(), bytes)) return ESP_FAIL;
  unlink(backup.c_str());
  const bool had_current_file = regular_file(current);
  const bool current_was_active = custom_present_[index];
  bool current_moved = !had_current_file;
  if (had_current_file) {
    current_moved = rename(current.c_str(), backup.c_str()) == 0;
    if (!current_moved && errno == EBUSY) {
      // LittleFS refuses to rename a file while LVGL is decoding it. Publish
      // the set default first so the display observes a generation change and
      // closes the old custom source, then retry the atomic file swap.
      if (current_was_active) {
        custom_present_[index] = false;
        ++snapshot_.generation;
        refresh_active_bytes_locked();
      }
      lock.unlock();
      for (int attempt = 0; attempt < 50 && !current_moved; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(100));
        current_moved = rename(current.c_str(), backup.c_str()) == 0;
        if (!current_moved && errno != EBUSY) break;
      }
      lock.lock();
    }
    if (!current_moved) {
      if (current_was_active && !custom_present_[index]) {
        custom_present_[index] = true;
        ++snapshot_.generation;
        refresh_active_bytes_locked();
      }
      unlink(temporary.c_str());
      return ESP_FAIL;
    }
  }
  if (rename(temporary.c_str(), current.c_str()) != 0) {
    if (had_current_file) rename(backup.c_str(), current.c_str());
    if (current_was_active && !custom_present_[index]) {
      custom_present_[index] = true;
      ++snapshot_.generation;
      refresh_active_bytes_locked();
    }
    unlink(temporary.c_str());
    return ESP_FAIL;
  }
  unlink(backup.c_str());
  custom_present_[index] = true;
  custom_sizes_[index] = bytes.size();
  ++snapshot_.generation;
  refresh_active_bytes_locked();
  refresh_storage_locked();
  return ESP_OK;
}

esp_err_t ReactionAssetService::reset_custom(std::string_view id) {
  const auto* event = core::reaction_event(id);
  if (event == nullptr) return ESP_ERR_INVALID_ARG;
  const std::size_t index = static_cast<std::size_t>(event - core::reaction_events().data());
  const std::lock_guard<std::mutex> mutation_lock(filesystem_mutation_mutex_);
  const std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.busy) return ESP_ERR_INVALID_STATE;
  // LittleFS deliberately rejects unlinking an open file. The display keeps
  // the active GIF open while decoding, so first persist the reset intent in NVS and
  // publish the set-default source. A small core-0 worker removes the old file
  // after LVGL observes the generation change and closes its decoder.
  const std::uint32_t previous_reset_mask = reset_mask_;
  reset_mask_ |= 1UL << index;
  if (persist_reset_mask_locked() != ESP_OK) {
    reset_mask_ = previous_reset_mask;
    return ESP_FAIL;
  }
  custom_present_[index] = false;
  custom_sizes_[index] = 0;
  ++snapshot_.generation;
  refresh_active_bytes_locked();
  refresh_storage_locked();
  schedule_cleanup_locked();
  return ESP_OK;
}

std::string ReactionAssetService::set_vfs_path(std::string_view id) const {
  const auto* event = core::reaction_event(id);
  if (event == nullptr) return {};
  const std::size_t index = static_cast<std::size_t>(event - core::reaction_events().data());
  const std::lock_guard<std::mutex> lock(mutex_);
  return current_present_[index]
             ? std::string(kCurrentPath) + "/" + std::string(id) + ".gif"
             : std::string{};
}

std::string ReactionAssetService::effective_vfs_path(std::string_view id) const {
  const auto* event = core::reaction_event(id);
  if (event == nullptr) return {};
  const std::size_t index = static_cast<std::size_t>(event - core::reaction_events().data());
  const std::lock_guard<std::mutex> lock(mutex_);
  if ((disabled_mask_ & (1UL << index)) != 0) return {};
  if (custom_present_[index]) {
    return std::string(kCustomPath) + "/" + std::string(id) + ".gif";
  }
  return current_present_[index]
             ? std::string(kCurrentPath) + "/" + std::string(id) + ".gif"
             : std::string{};
}

std::string ReactionAssetService::preview_vfs_path(std::string_view id) const {
  const auto* event = core::reaction_event(id);
  if (event == nullptr) return {};
  const std::size_t index = static_cast<std::size_t>(event - core::reaction_events().data());
  const std::lock_guard<std::mutex> lock(mutex_);
  if (custom_present_[index]) {
    return std::string(kCustomPath) + "/" + std::string(id) + ".gif";
  }
  return current_present_[index]
             ? std::string(kCurrentPath) + "/" + std::string(id) + ".gif"
             : std::string{};
}

std::string ReactionAssetService::effective_lvgl_path(core::PrinterActivity activity) const {
  const std::string vfs = effective_vfs_path(core::reaction_event(activity).id);
  if (vfs.rfind(kMountPath, 0) != 0) return {};
  return std::string("R:") + vfs.substr(std::strlen(kMountPath));
}

bool ReactionAssetService::download_manifest(std::string_view id, std::string& body) const {
  const std::string url = set_asset_base_url() + std::string(id) + "/manifest.json";
  for (int attempt = 1; attempt <= 2; ++attempt) {
    if (cancellation_requested()) return false;
    TextResponse response;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.event_handler = receive_text;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    // Keep cancellation bounded even while DNS, TLS or the remote endpoint is
    // unresponsive. A second attempt still covers a transient network stall.
    config.timeout_ms = 5000;
    config.user_agent = "PrintDeck reactions";
    config.disable_auto_redirect = false;
    config.max_redirection_count = 3;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
      ESP_LOGW(kTag,
               "Reaction manifest request allocation failed (attempt %d); "
               "internal heap=%u, largest=%u",
               attempt,
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    } else {
      esp_http_client_set_header(client, "Accept", "application/json");
      const esp_err_t result = esp_http_client_perform(client);
      const int status =
          result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
      esp_http_client_cleanup(client);
      if (cancellation_requested()) return false;
      if (result == ESP_OK && status == 200 && !response.too_large &&
          !response.body.empty()) {
        body = std::move(response.body);
        return true;
      }
      ESP_LOGW(kTag,
               "Reaction manifest download failed (attempt %d): result=%s, "
               "status=%d, bytes=%u, too_large=%d, internal heap=%u, largest=%u",
               attempt, esp_err_to_name(result), status,
               static_cast<unsigned>(response.body.size()), response.too_large,
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    }
    if (attempt == 1) vTaskDelay(pdMS_TO_TICKS(400));
  }
  return false;
}

bool ReactionAssetService::validate_manifest(
    std::string_view body, std::string_view expected_id,
    std::array<std::size_t, core::kReactionEventCount>& sizes,
    std::array<std::string, core::kReactionEventCount>& hashes,
    std::string& name, std::string& version, std::size_t& total,
    bool allow_legacy_profile) const {
  cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
  if (root == nullptr) return false;
  const cJSON* set_id = cJSON_GetObjectItemCaseSensitive(root, "set");
  if (cJSON_IsString(set_id)) {
    const auto definition = std::find_if(
        kSets.begin(), kSets.end(), [expected_id](const auto& set) {
          return set.id == expected_id;
        });
    const cJSON* dimensions = cJSON_GetObjectItemCaseSensitive(root, "size");
    const cJSON* asset_profile =
        cJSON_GetObjectItemCaseSensitive(root, "asset_profile");
    const cJSON* width = cJSON_IsArray(dimensions)
                             ? cJSON_GetArrayItem(dimensions, 0)
                             : nullptr;
    const cJSON* height = cJSON_IsArray(dimensions)
                              ? cJSON_GetArrayItem(dimensions, 1)
                              : nullptr;
    const cJSON* files = cJSON_GetObjectItemCaseSensitive(root, "files");
    const cJSON* declared_total =
        cJSON_GetObjectItemCaseSensitive(root, "total_bytes");
    const cJSON* passed = cJSON_GetObjectItemCaseSensitive(root, "pass");
    const bool migratable_lcd_profile =
        allow_legacy_profile && kNeedsRound240ProfileMigration &&
        cJSON_IsString(asset_profile) &&
        std::string_view(asset_profile->valuestring) == "lcd_1_54";
    const bool profile_valid =
        (cJSON_IsString(asset_profile) &&
         kSetAssetProfile == asset_profile->valuestring) ||
        (allow_legacy_profile && asset_profile == nullptr) ||
        migratable_lcd_profile;
    bool valid = definition != kSets.end() && expected_id == set_id->valuestring &&
                 profile_valid &&
                 cJSON_IsArray(dimensions) && cJSON_GetArraySize(dimensions) == 2 &&
                 cJSON_IsNumber(width) && width->valueint == kDisplayWidth &&
                 cJSON_IsNumber(height) && height->valueint == kDisplayHeight &&
                 cJSON_IsObject(files) &&
                 cJSON_GetArraySize(files) ==
                     static_cast<int>(core::kReactionEventCount) &&
                 cJSON_IsNumber(declared_total) && declared_total->valuedouble > 0 &&
                 cJSON_IsTrue(passed);
    total = 0;
    for (std::size_t index = 0;
         valid && index < core::kReactionEventCount; ++index) {
      const auto& event = core::reaction_events()[index];
      const cJSON* entry =
          cJSON_GetObjectItemCaseSensitive(files, event.id.data());
      const cJSON* file =
          entry ? cJSON_GetObjectItemCaseSensitive(entry, "file") : nullptr;
      const cJSON* bytes =
          entry ? cJSON_GetObjectItemCaseSensitive(entry, "bytes") : nullptr;
      const cJSON* sha =
          entry ? cJSON_GetObjectItemCaseSensitive(entry, "sha256") : nullptr;
      const cJSON* gif_width =
          entry ? cJSON_GetObjectItemCaseSensitive(entry, "width") : nullptr;
      const cJSON* gif_height =
          entry ? cJSON_GetObjectItemCaseSensitive(entry, "height") : nullptr;
      const cJSON* frames =
          entry ? cJSON_GetObjectItemCaseSensitive(entry, "frames") : nullptr;
      const std::string expected_file = std::string(event.id) + ".gif";
      std::array<std::uint8_t, 32> digest{};
      valid = cJSON_IsObject(entry) && cJSON_IsString(file) &&
              expected_file == file->valuestring && cJSON_IsNumber(bytes) &&
              bytes->valuedouble > 0 &&
              bytes->valuedouble <= snapshot_.maximum_file_bytes &&
              cJSON_IsString(sha) && parse_sha256(sha->valuestring, digest) &&
              cJSON_IsNumber(gif_width) && gif_width->valueint == kDisplayWidth &&
              cJSON_IsNumber(gif_height) && gif_height->valueint == kDisplayHeight &&
              cJSON_IsNumber(frames) && frames->valueint > 0 &&
              frames->valueint <= 120;
      if (valid) {
        sizes[index] = static_cast<std::size_t>(bytes->valuedouble);
        hashes[index] = sha->valuestring;
        total += sizes[index];
      }
    }
    valid = valid && total <= snapshot_.maximum_set_bytes &&
            declared_total->valuedouble == static_cast<double>(total);
    if (valid) {
      name = std::string(definition->name);
      version = std::string(definition->version);
    }
    cJSON_Delete(root);
    return valid;
  }

  // Keep the original schema readable so the active set already stored on a
  // device survives the first boot after upgrading to resolution-specific sets.
  const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
  const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
  const cJSON* set_name = cJSON_GetObjectItemCaseSensitive(root, "name");
  const cJSON* set_version = cJSON_GetObjectItemCaseSensitive(root, "version");
  const cJSON* events = cJSON_GetObjectItemCaseSensitive(root, "events");
  bool valid = allow_legacy_profile &&
               cJSON_IsNumber(schema) && schema->valueint == 1 && cJSON_IsString(id) &&
               (expected_id.empty() || expected_id == id->valuestring) &&
               cJSON_IsString(set_name) && std::strlen(set_name->valuestring) <= 48 &&
               cJSON_IsString(set_version) && std::strlen(set_version->valuestring) <= 16 &&
               cJSON_IsObject(events) &&
               cJSON_GetArraySize(events) == static_cast<int>(core::kReactionEventCount);
  total = 0;
  for (std::size_t index = 0; valid && index < core::kReactionEventCount; ++index) {
    const auto& definition = core::reaction_events()[index];
    const cJSON* entry = cJSON_GetObjectItemCaseSensitive(events, definition.id.data());
    const cJSON* file = entry ? cJSON_GetObjectItemCaseSensitive(entry, "file") : nullptr;
    const cJSON* bytes = entry ? cJSON_GetObjectItemCaseSensitive(entry, "bytes") : nullptr;
    const cJSON* sha = entry ? cJSON_GetObjectItemCaseSensitive(entry, "sha256") : nullptr;
    const std::string expected_file = std::string(definition.id) + ".gif";
    std::array<std::uint8_t, 32> digest{};
    valid = cJSON_IsObject(entry) && cJSON_IsString(file) &&
            expected_file == file->valuestring && cJSON_IsNumber(bytes) &&
            bytes->valuedouble > 0 && bytes->valuedouble <= snapshot_.maximum_file_bytes &&
            cJSON_IsString(sha) && parse_sha256(sha->valuestring, digest);
    if (valid) {
      sizes[index] = static_cast<std::size_t>(bytes->valuedouble);
      hashes[index] = sha->valuestring;
      total += sizes[index];
    }
  }
  valid = valid && total <= snapshot_.maximum_set_bytes;
  if (valid) {
    name = set_name->valuestring;
    version = set_version->valuestring;
  }
  cJSON_Delete(root);
  return valid;
}

bool ReactionAssetService::download_file(std::string_view url_view,
                                         const char* output_path,
                                         std::size_t expected_size,
                                         std::string_view expected_sha256) const {
  const std::string url(url_view);
  std::array<std::uint8_t, 32> expected{};
  if (!parse_sha256(expected_sha256, expected)) return false;
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.crt_bundle_attach = esp_crt_bundle_attach;
  // esp_http_client_read reports ESP_ERR_HTTP_EAGAIN on this timeout. Short
  // reads let the worker observe a cancellation without closing a client from
  // another task, which esp_http_client does not document as thread-safe.
  config.timeout_ms = 5000;
  config.buffer_size = 4096;
  config.user_agent = "PrintDeck reactions";
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr || esp_http_client_open(client, 0) != ESP_OK) {
    if (client != nullptr) esp_http_client_cleanup(client);
    return false;
  }
  const int64_t content_length = esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status != 200 || (content_length >= 0 &&
      static_cast<std::size_t>(content_length) != expected_size)) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  esp_http_client_set_timeout_ms(client, 2000);
  FILE* file = std::fopen(output_path, "wb");
  if (file == nullptr) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  bool valid = mbedtls_sha256_starts(&sha, 0) == 0;
  struct HeapCapsDeleter {
    void operator()(std::uint8_t* pointer) const { heap_caps_free(pointer); }
  };
  std::unique_ptr<std::uint8_t, HeapCapsDeleter> buffer(static_cast<std::uint8_t*>(
      heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
  if (!buffer) valid = false;
  std::size_t received = 0;
  int consecutive_timeouts = 0;
  while (valid && received < expected_size) {
    if (cancellation_requested()) {
      valid = false;
      break;
    }
    const int bytes = esp_http_client_read(
        client, reinterpret_cast<char*>(buffer.get()),
        std::min<std::size_t>(4096, expected_size - received));
    if (bytes == -ESP_ERR_HTTP_EAGAIN) {
      if (++consecutive_timeouts <= 5) continue;
      valid = false;
      break;
    }
    if (bytes <= 0) {
      valid = false;
      break;
    }
    consecutive_timeouts = 0;
    valid = std::fwrite(buffer.get(), 1, static_cast<std::size_t>(bytes), file) ==
                static_cast<std::size_t>(bytes) &&
            mbedtls_sha256_update(&sha, buffer.get(), static_cast<std::size_t>(bytes)) == 0;
    received += static_cast<std::size_t>(bytes);
  }
  std::array<std::uint8_t, 32> actual{};
  valid = valid && received == expected_size &&
          mbedtls_sha256_finish(&sha, actual.data()) == 0 && actual == expected;
  mbedtls_sha256_free(&sha);
  // Always close the staging file, including cancellation and checksum error
  // paths. Short-circuiting fclose when `valid` is already false leaves an
  // open LittleFS descriptor and prevents the staging tree from being removed.
  const bool file_closed = std::fclose(file) == 0;
  valid = valid && file_closed;
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (!valid) unlink(output_path);
  return valid;
}

bool ReactionAssetService::load_active_manifest() {
  std::vector<std::uint8_t> bytes;
  if (!read_file(std::string(kCurrentPath) + "/manifest.json",
                 kMaximumManifestBytes, bytes)) return false;
  const std::string_view body(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
  const cJSON* id_item = root ? cJSON_GetObjectItemCaseSensitive(root, "set") : nullptr;
  if (!cJSON_IsString(id_item) && root != nullptr) {
    id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
  }
  const std::string id = cJSON_IsString(id_item) ? id_item->valuestring : "";
  const cJSON* asset_profile =
      root ? cJSON_GetObjectItemCaseSensitive(root, "asset_profile") : nullptr;
  const bool exact_profile = cJSON_IsString(asset_profile) &&
      kSetAssetProfile == asset_profile->valuestring;
  const bool migratable_profile = kNeedsRound240ProfileMigration &&
      (asset_profile == nullptr ||
       (cJSON_IsString(asset_profile) &&
        std::string_view(asset_profile->valuestring) == "lcd_1_54"));
  if (root != nullptr) cJSON_Delete(root);
  std::array<std::size_t, core::kReactionEventCount> sizes{};
  std::array<std::string, core::kReactionEventCount> hashes{};
  std::string name;
  std::string version;
  std::size_t total = 0;
  if (id.empty() ||
      !validate_manifest(body, id, sizes, hashes, name, version, total, true)) {
    return false;
  }
  for (std::size_t index = 0; index < core::kReactionEventCount; ++index) {
    const std::string path = std::string(kCurrentPath) + "/" +
                             std::string(core::reaction_events()[index].id) + ".gif";
    std::vector<std::uint8_t> gif;
    std::array<std::uint8_t, 32> expected{};
    if (file_size(path) != sizes[index] || !parse_sha256(hashes[index], expected) ||
        !read_file(path, sizes[index], gif)) return false;
    std::array<std::uint8_t, 32> actual{};
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    const bool checksum_ok = mbedtls_sha256_starts(&sha, 0) == 0 &&
                             mbedtls_sha256_update(&sha, gif.data(), gif.size()) == 0 &&
                             mbedtls_sha256_finish(&sha, actual.data()) == 0 &&
                             actual == expected;
    mbedtls_sha256_free(&sha);
    core::GifMetadata metadata;
    if (!checksum_ok || !core::inspect_gif(gif, metadata)) return false;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  current_present_.fill(true);
  current_sizes_ = sizes;
  snapshot_.active_set_id = id;
  snapshot_.active_set_name = name;
  snapshot_.active_set_version = version;
  if constexpr (kNeedsRound240ProfileMigration) {
    if (migratable_profile && known_set(id)) {
      profile_migration_pending_ = true;
      profile_migration_set_ = id;
      profile_migration_not_before_ms_ =
          monotonic_ms() + kProfileMigrationInitialDelayMs;
      ESP_LOGI(kTag,
               "Legacy KNOMI2 reaction set %s will migrate after Wi-Fi is ready",
               id.c_str());
    } else if (exact_profile) {
      profile_migration_pending_ = false;
      profile_migration_attempt_active_ = false;
      profile_migration_set_.clear();
      profile_migration_not_before_ms_ = 0;
    } else if (migratable_profile) {
      ESP_LOGW(kTag,
               "Legacy reaction set %s is not in the catalog; automatic migration skipped",
               id.c_str());
    }
  }
  ++snapshot_.generation;
  refresh_active_bytes_locked();
  return true;
}

void ReactionAssetService::reaper_task_entry(void* context) {
  static_cast<ReactionAssetService*>(context)->reaper_loop();
  while (true) vTaskDelay(portMAX_DELAY);
}

void ReactionAssetService::reaper_loop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    TaskHandle_t worker_to_delete = nullptr;
    TaskHandle_t cleanup_to_delete = nullptr;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (task_ != nullptr &&
          worker_finished_.load(std::memory_order_acquire) &&
          eTaskGetState(task_) == eSuspended) {
        worker_to_delete = task_;
      }
      if (cleanup_task_ != nullptr &&
          cleanup_finished_.load(std::memory_order_acquire) &&
          eTaskGetState(cleanup_task_) == eSuspended) {
        cleanup_to_delete = cleanup_task_;
      }
    }

    if (worker_to_delete != nullptr) {
      vTaskDeleteWithCaps(worker_to_delete);
      const std::lock_guard<std::mutex> lock(mutex_);
      if (task_ == worker_to_delete) task_ = nullptr;
      worker_finished_.store(false, std::memory_order_release);
    }
    if (cleanup_to_delete != nullptr) {
      vTaskDeleteWithCaps(cleanup_to_delete);
      const std::lock_guard<std::mutex> lock(mutex_);
      if (cleanup_task_ == cleanup_to_delete) cleanup_task_ = nullptr;
      cleanup_finished_.store(false, std::memory_order_release);
      const bool run_followup = cleanup_followup_requested_ && reset_mask_ != 0;
      cleanup_followup_requested_ = false;
      if (run_followup) schedule_cleanup_locked();
    }
    maybe_start_profile_migration();
  }
}

void ReactionAssetService::task_entry(void* context) {
  auto* service = static_cast<ReactionAssetService*>(context);
  service->task_loop();
  ESP_LOGI(kTag,
           "Reaction worker complete; stack high-water=%u, internal heap=%u, largest=%u",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  service->worker_finished_.store(true, std::memory_order_release);
  if (service->reaper_task_ != nullptr) {
    xTaskNotifyGive(service->reaper_task_);
  }
  vTaskSuspend(nullptr);
  while (true) vTaskDelay(portMAX_DELAY);
}

void ReactionAssetService::cleanup_task_entry(void* context) {
  auto* service = static_cast<ReactionAssetService*>(context);
  service->cleanup_reset_custom_files();
  service->cleanup_finished_.store(true, std::memory_order_release);
  if (service->reaper_task_ != nullptr) {
    xTaskNotifyGive(service->reaper_task_);
  }
  vTaskSuspend(nullptr);
  while (true) vTaskDelay(portMAX_DELAY);
}

void ReactionAssetService::cleanup_reset_custom_files() {
  // Give the display loop time to observe the generation change, then retry
  // while any browser preview or LVGL decoder still has the old GIF open.
  for (int attempt = 0; attempt < 50; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    bool pending = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const std::uint32_t previous_reset_mask = reset_mask_;
      for (std::size_t index = 0; index < core::kReactionEventCount; ++index) {
        if ((reset_mask_ & (1UL << index)) == 0) continue;
        const auto& event = core::reaction_events()[index];
        const std::string path = std::string(kCustomPath) + "/" +
                                 std::string(event.id) + ".gif";
        if (unlink(path.c_str()) != 0 && errno != ENOENT) {
          pending = true;
          continue;
        }
        reset_mask_ &= ~(1UL << index);
        ESP_LOGI(kTag, "Removed custom GIF override for %.*s",
                 static_cast<int>(event.id.size()), event.id.data());
      }
      if (reset_mask_ != previous_reset_mask && persist_reset_mask_locked() != ESP_OK) {
        reset_mask_ = previous_reset_mask;
        pending = true;
      }
      refresh_storage_locked();
      pending = pending || reset_mask_ != 0;
    }
    if (!pending) return;
  }
  ESP_LOGW(kTag, "Custom GIF cleanup remains pending for the next restart");
}

bool ReactionAssetService::cancellation_requested() const {
  return cancel_requested_.load(std::memory_order_acquire);
}

void ReactionAssetService::finish_cancelled_install() {
  remove_tree(kStagingPath);
  const std::lock_guard<std::mutex> lock(mutex_);
  cancel_requested_.store(false, std::memory_order_release);
  snapshot_.busy = false;
  snapshot_.cancellable = false;
  snapshot_.progress_percent = 0;
  snapshot_.detail = "Reaction set installation cancelled.";
  snapshot_.installing_set_id.clear();
  snapshot_.installing_set_name.clear();
  if (profile_migration_pending_) schedule_profile_migration_retry_locked();
  profile_migration_attempt_active_ = false;
  refresh_storage_locked();
}

void ReactionAssetService::task_loop() {
  if (!request_pending_.exchange(false)) return;
  std::string id;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    id = requested_set_;
  }
  install_requested_set(std::move(id));
}

void ReactionAssetService::install_requested_set(std::string id) {
  if (cancellation_requested()) {
    finish_cancelled_install();
    return;
  }
  if (network_ == nullptr || !network_->status().station_connected) {
    fail("Connect PrintDeck to Wi-Fi before changing reaction sets.");
    return;
  }
  std::string manifest;
  if (!download_manifest(id, manifest)) {
    if (cancellation_requested()) {
      finish_cancelled_install();
      return;
    }
    fail("The reaction set could not be downloaded from GitHub.");
    return;
  }
  if (cancellation_requested()) {
    finish_cancelled_install();
    return;
  }
  std::array<std::size_t, core::kReactionEventCount> sizes{};
  std::array<std::string, core::kReactionEventCount> hashes{};
  std::string name;
  std::string version;
  std::size_t total = 0;
  if (!validate_manifest(manifest, id, sizes, hashes, name, version, total, false)) {
    fail("The reaction set manifest did not pass validation.");
    return;
  }
  std::size_t effective_total = 0;
  std::size_t maximum_active_bytes = 0;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    maximum_active_bytes = snapshot_.maximum_custom_bytes;
    for (std::size_t index = 0; index < core::kReactionEventCount; ++index) {
      if (custom_present_[index]) {
        effective_total += custom_sizes_[index];
      } else {
        effective_total += sizes[index];
      }
    }
  }
  if (effective_total > maximum_active_bytes) {
    fail("The reaction set and custom GIFs exceed the 1.5 MB animation limit.");
    return;
  }
  if (cancellation_requested()) {
    finish_cancelled_install();
    return;
  }
  if (!remove_tree(kPreviousPath) || !remove_tree(kStagingPath)) {
    fail("PrintDeck could not prepare reaction storage.");
    return;
  }
  if (!storage_has_room(total + manifest.size())) {
    fail("PrintDeck needs more free reaction storage to change sets safely.");
    return;
  }
  ensure_directory(kStagingPath);
  if (!write_text((std::string(kStagingPath) + "/manifest.json").c_str(), manifest)) {
    remove_tree(kStagingPath);
    fail("PrintDeck could not prepare reaction storage.");
    return;
  }
  std::vector<std::uint8_t> gif;
  for (std::size_t index = 0; index < core::kReactionEventCount; ++index) {
    if (cancellation_requested()) {
      finish_cancelled_install();
      return;
    }
    const std::string event(core::reaction_events()[index].id);
    const std::string filename = event + ".gif";
    const std::string url = set_asset_base_url() + id + "/" + filename;
    const std::string output = std::string(kStagingPath) + "/" + filename;
    if (!download_file(url, output.c_str(), sizes[index], hashes[index]) ||
        !read_file(output, sizes[index], gif)) {
      if (cancellation_requested()) {
        finish_cancelled_install();
        return;
      }
      remove_tree(kStagingPath);
      fail("A reaction GIF was incomplete or did not match its checksum.");
      return;
    }
    core::GifMetadata metadata;
    if (!core::inspect_gif(gif, metadata)) {
      remove_tree(kStagingPath);
      fail("A reaction GIF did not pass device validation.");
      return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.progress_percent = static_cast<int>((index + 1) * 95 /
                                                  core::kReactionEventCount);
    snapshot_.detail = "Downloading and validating reaction GIFs…";
  }
  bool cancelled_before_activation = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    cancelled_before_activation = cancellation_requested();
    if (!cancelled_before_activation) {
      snapshot_.cancellable = false;
      snapshot_.detail = "Activating reaction set…";
    }
  }
  if (cancelled_before_activation) {
    finish_cancelled_install();
    return;
  }
  std::array<bool, core::kReactionEventCount> previous_present{};
  {
    const std::lock_guard<std::mutex> mutation_lock(filesystem_mutation_mutex_);
    const bool had_current = directory_exists(kCurrentPath);
    if (had_current) {
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        previous_present = current_present_;
        current_present_.fill(false);
        ++snapshot_.generation;
        refresh_active_bytes_locked();
      }
      // Let the LVGL task observe the empty source and close any decoder
      // before the bounded directory swap begins.
      vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (had_current &&
        !rename_with_busy_retry(kCurrentPath, kPreviousPath,
                                "Preserving current reaction set")) {
      remove_tree(kStagingPath);
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        current_present_ = previous_present;
        ++snapshot_.generation;
        refresh_active_bytes_locked();
      }
      fail("The current reaction set could not be preserved.");
      return;
    }
    if (!rename_with_busy_retry(kStagingPath, kCurrentPath,
                                "Activating staged reaction set")) {
      bool rollback_ok = true;
      if (had_current) {
        rollback_ok = rename_with_busy_retry(kPreviousPath, kCurrentPath,
                                             "Restoring current reaction set");
      }
      remove_tree(kStagingPath);
      if (rollback_ok) {
        const std::lock_guard<std::mutex> lock(mutex_);
        current_present_ = previous_present;
        ++snapshot_.generation;
        refresh_active_bytes_locked();
      } else {
        const std::lock_guard<std::mutex> lock(mutex_);
        current_present_.fill(false);
        snapshot_.active_set_id.clear();
        snapshot_.active_set_name.clear();
        snapshot_.active_set_version.clear();
        ++snapshot_.generation;
        refresh_active_bytes_locked();
      }
      fail("The new reaction set could not be activated.");
      return;
    }
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.active_set_id = std::move(id);
    snapshot_.active_set_name = std::move(name);
    snapshot_.active_set_version = std::move(version);
    current_present_.fill(true);
    current_sizes_ = sizes;
    profile_migration_pending_ = false;
    profile_migration_attempt_active_ = false;
    profile_migration_set_.clear();
    profile_migration_not_before_ms_ = 0;
    ++snapshot_.generation;
    refresh_active_bytes_locked();
    refresh_storage_locked();
  }
  // Publish the new generation before deleting the rollback directory. This
  // gives LVGL and browser readers time to close descriptors that still refer
  // to files moved under `previous` during the atomic directory swap.
  vTaskDelay(pdMS_TO_TICKS(1000));
  {
    const std::lock_guard<std::mutex> mutation_lock(filesystem_mutation_mutex_);
    if (!remove_tree(kPreviousPath)) {
      ESP_LOGW(kTag, "Old reaction set cleanup deferred until the next restart");
    }
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    cancel_requested_.store(false, std::memory_order_release);
    snapshot_.busy = false;
    snapshot_.cancellable = false;
    snapshot_.progress_percent = 100;
    snapshot_.detail = "Reaction set installed.";
    snapshot_.installing_set_id.clear();
    snapshot_.installing_set_name.clear();
    refresh_storage_locked();
  }
}

void ReactionAssetService::fail(std::string detail) {
  const std::lock_guard<std::mutex> lock(mutex_);
  cancel_requested_.store(false, std::memory_order_release);
  snapshot_.busy = false;
  snapshot_.cancellable = false;
  snapshot_.progress_percent = 0;
  snapshot_.detail = std::move(detail);
  snapshot_.installing_set_id.clear();
  snapshot_.installing_set_name.clear();
  if (profile_migration_pending_) schedule_profile_migration_retry_locked();
  profile_migration_attempt_active_ = false;
  refresh_storage_locked();
  ESP_LOGW(kTag, "%s", snapshot_.detail.c_str());
}

}  // namespace printdeck::platform
