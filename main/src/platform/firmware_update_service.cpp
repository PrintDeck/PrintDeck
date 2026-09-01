#include "printdeck/platform/firmware_update_service.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <string_view>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "printdeck/core/firmware_channel.hpp"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {
namespace {
constexpr char kTag[] = "firmware_update";
constexpr char kStableChannelBase[] =
    "https://printdeck.xyz/ota/";
constexpr char kOfficialReleasePrefix[] =
    "https://github.com/PrintDeck/PrintDeck/releases/download/";
constexpr std::size_t kMaximumManifestBytes = 4 * 1024;
constexpr std::uint64_t kCheckIntervalSeconds = 24ULL * 60ULL * 60ULL;
constexpr std::uint64_t kCheckIntervalMs = kCheckIntervalSeconds * 1000ULL;
constexpr std::uint64_t kManualCheckCooldownMs = 30'000;
// Onboarding and adding the first printer normally happen in the first few
// minutes after boot.  The daily check is invisible background work, so defer
// its first opportunity until that interactive setup window has passed.
constexpr std::uint64_t kAutomaticCheckStartupDelayMs = 5ULL * 60ULL * 1000ULL;
constexpr std::uint32_t kSchedulerStackBytes = 4096;
// HTTPS checks use about 4 KB of stack on the AMOLED target. Reserving the old
// 10 KB stack left no contiguous internal block for the hardware AES driver,
// even though most of the task stack stayed unused. Keep OTA on the same
// flash-safe internal stack size: after camera/WebRTC use, the round target can
// still reserve 6 KB reliably but may no longer have one contiguous 8 KB block.
constexpr std::uint32_t kCheckWorkerStackBytes = 6U * 1024U;
constexpr std::uint32_t kInstallWorkerStackBytes = 6U * 1024U;
constexpr std::time_t kValidEpochThreshold = 1'700'000'000;
constexpr char kNvsNamespace[] = "printdeck";
constexpr char kLastCheckKey[] = "update_check";
struct Response { std::string body; bool too_large = false; };
esp_err_t receive(esp_http_client_event_t* event) {
  auto* response = static_cast<Response*>(event->user_data);
  if (response == nullptr || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
  if (response->body.size() + static_cast<std::size_t>(event->data_len) > kMaximumManifestBytes) {
    response->too_large = true;
    return ESP_FAIL;
  }
  response->body.append(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}
std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time() / 1000ULL);
}
std::time_t valid_epoch_now() {
  const std::time_t now = std::time(nullptr);
  return now >= kValidEpochThreshold ? now : 0;
}
bool matches_prefix(const std::string& value, std::size_t offset,
                    std::string_view prefix) {
  return !prefix.empty() && value.compare(offset, prefix.size(), prefix) == 0;
}
bool map_full_asset_to_ota(std::string& url, std::size_t offset,
                           std::string_view full_prefix,
                           std::string_view ota_prefix) {
  if (!matches_prefix(url, offset, full_prefix)) return false;
  url.replace(offset, full_prefix.size(), ota_prefix);
  return true;
}
bool partition_sha256_matches(
    const esp_partition_t* partition, std::size_t image_size,
    const std::array<std::uint8_t, 32>& expected) {
  if (partition == nullptr || image_size == 0 || image_size > partition->size) {
    return false;
  }
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool valid = mbedtls_sha256_starts(&context, 0) == 0;
  std::array<std::uint8_t, 1024> buffer{};
  for (std::size_t offset = 0; valid && offset < image_size;) {
    const std::size_t bytes = std::min(buffer.size(), image_size - offset);
    valid = esp_partition_read(partition, offset, buffer.data(), bytes) == ESP_OK &&
            mbedtls_sha256_update(&context, buffer.data(), bytes) == 0;
    offset += bytes;
  }
  std::array<std::uint8_t, 32> actual{};
  valid = valid && mbedtls_sha256_finish(&context, actual.data()) == 0;
  mbedtls_sha256_free(&context);
  return valid && actual == expected;
}
}

esp_err_t FirmwareUpdateService::start(const NetworkService& network) {
  if (network_ != nullptr) return ESP_ERR_INVALID_STATE;
  network_ = &network;
  next_automatic_check_ms_ = monotonic_ms() + kAutomaticCheckStartupDelayMs;
  std::uint64_t last_check_epoch = 0;
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_u64(handle, kLastCheckKey, &last_check_epoch);
    nvs_close(handle);
  }
  last_check_epoch_.store(last_check_epoch, std::memory_order_release);
  // Scheduling only observes state and starts or reclaims the flash-safe
  // worker. Keep this persistent stack in PSRAM; the 6 KB internal stack is
  // reserved only while an HTTPS check or OTA operation is actually running.
  if (xTaskCreatePinnedToCoreWithCaps(
          scheduler_entry, "fw_schedule", kSchedulerStackBytes, this, 2,
          &scheduler_task_, kServiceCore,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    scheduler_task_ = nullptr;
    network_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void FirmwareUpdateService::set_background_activity_probe(
    BackgroundActivityProbe probe, void* context) {
  background_activity_probe_ = probe;
  background_activity_context_ = context;
}

void FirmwareUpdateService::poll() {
  if (network_ == nullptr) return;

  // The worker suspends itself after its flash-sensitive operation. Delete it
  // here, from the PSRAM-backed scheduler, so its internal stack is fully
  // released before another request is allowed to reserve the same memory.
  if (task_ != nullptr && worker_finished_.load(std::memory_order_acquire) &&
      eTaskGetState(task_) == eSuspended) {
    worker_finished_.store(false, std::memory_order_release);
    vTaskDeleteWithCaps(task_);
    task_ = nullptr;
  }

  const bool connected = network_->status().station_connected;
  const std::uint64_t now_ms = monotonic_ms();
  const bool interactive_activity =
      background_activity_probe_ != nullptr &&
      background_activity_probe_(background_activity_context_);
  const std::uint64_t last_check_ms =
      last_check_started_ms_.load(std::memory_order_acquire);
  if (last_check_ms != 0 && now_ms >= last_check_ms &&
      now_ms - last_check_ms >= kManualCheckCooldownMs) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.state == FirmwareUpdateState::current) {
      snapshot_.state = FirmwareUpdateState::idle;
      snapshot_.detail = "Ready to check for updates.";
    }
  }

  // Avoid competing with printer, Web Config and time synchronization during
  // startup. Subsequent daily checks are still evaluated on every bounded
  // monitor pass.
  if (connected && now_ms >= next_automatic_check_ms_) {
    if (interactive_activity) {
      // User-driven network work always wins. Re-evaluate promptly after it
      // ends without marking the update UI busy in the meantime.
      next_automatic_check_ms_ = now_ms + 1000ULL;
    } else {
      const std::time_t now = valid_epoch_now();
      const std::uint64_t last_epoch =
          last_check_epoch_.load(std::memory_order_acquire);
      if (now > 0 && last_epoch != 0 &&
          static_cast<std::uint64_t>(now) <
              last_epoch + kCheckIntervalSeconds) {
        next_automatic_check_ms_ =
            now_ms + (last_epoch + kCheckIntervalSeconds -
                      static_cast<std::uint64_t>(now)) * 1000ULL;
      } else if (request_check(false)) {
        // A failed automatic request is retried on the next daily cadence, not
        // every 30-second manual cooldown. This keeps transient HTTPS failures
        // from repeatedly reserving and fragmenting the 6 KB internal stack.
        next_automatic_check_ms_ = now_ms + kCheckIntervalMs;
      } else {
        // A manual request may already be in flight. Re-evaluate after its
        // bounded cooldown rather than losing the scheduled daily check.
        next_automatic_check_ms_ = now_ms + kManualCheckCooldownMs;
      }
    }
  }

  if (!connected || task_ != nullptr) return;

  if (check_requested_.load(std::memory_order_acquire) &&
      automatic_check_requested_.load(std::memory_order_acquire) &&
      interactive_activity) {
    return;
  }

  WorkerOperation operation = WorkerOperation::none;
  if (check_requested_.exchange(false, std::memory_order_acq_rel)) {
    operation = WorkerOperation::check;
    automatic_check_requested_.store(false, std::memory_order_release);
  } else if (install_requested_.exchange(false, std::memory_order_acq_rel)) {
    operation = WorkerOperation::install;
  }
  if (operation == WorkerOperation::none) return;

  worker_operation_.store(operation, std::memory_order_release);
  worker_finished_.store(false, std::memory_order_release);
  ESP_LOGI(kTag,
           "Starting %s worker with %u-byte stack; internal=%u, "
           "largest-internal=%u, largest-dma=%u",
           operation == WorkerOperation::check ? "check" : "install",
           static_cast<unsigned>(operation == WorkerOperation::check
                                     ? kCheckWorkerStackBytes
                                     : kInstallWorkerStackBytes),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)));
  const std::uint32_t worker_stack_bytes = operation == WorkerOperation::check
                                               ? kCheckWorkerStackBytes
                                               : kInstallWorkerStackBytes;
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "fw_update", worker_stack_bytes,
                                     this, 2,
                                     &task_, kServiceCore,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
    task_ = nullptr;
    worker_operation_.store(WorkerOperation::none, std::memory_order_release);
    if (operation == WorkerOperation::check) {
      fail("PrintDeck could not create the update request.");
    } else {
      install_running_.store(false, std::memory_order_release);
      fail("The firmware update could not start.");
    }
  }
}
bool FirmwareUpdateService::request_check() {
  return request_check(true);
}
bool FirmwareUpdateService::request_check(bool manual_request) {
  if (scheduler_task_ == nullptr) return false;
  const std::uint64_t now = monotonic_ms();
  const std::uint64_t last = last_check_started_ms_.load(std::memory_order_acquire);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    // A visible failure explicitly offers "Tap to retry", so that retry must
    // work immediately. Automatic checks still honor the cooldown after a
    // failure so a temporary HTTP problem cannot create a tight retry loop.
    if ((!manual_request || snapshot_.state != FirmwareUpdateState::failed) &&
        last != 0 && now < last + kManualCheckCooldownMs) {
      return false;
    }
  }
  bool expected = false;
  if (!check_requested_.compare_exchange_strong(expected, true)) return false;
  automatic_check_requested_.store(!manual_request, std::memory_order_release);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.busy) {
      check_requested_ = false;
      automatic_check_requested_.store(false, std::memory_order_release);
      return false;
    }
    snapshot_.state = FirmwareUpdateState::checking;
    snapshot_.busy = true;
    snapshot_.detail = "Checking for a newer PrintDeck release...";
  }
  last_check_started_ms_.store(now, std::memory_order_release);
  if (scheduler_task_ != nullptr) xTaskNotifyGive(scheduler_task_);
  return true;
}
bool FirmwareUpdateService::begin_manual_install() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.busy) return false;
  snapshot_.state = FirmwareUpdateState::downloading;
  snapshot_.busy = true;
  snapshot_.progress_percent = 0;
  snapshot_.detail = "Receiving and validating the firmware image...";
  return true;
}
void FirmwareUpdateService::update_manual_progress(int percent) {
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.progress_percent = std::clamp(percent, 0, 99);
}
void FirmwareUpdateService::fail_manual_install(std::string detail) {
  fail(std::move(detail));
}
void FirmwareUpdateService::finish_manual_install() {
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.state = FirmwareUpdateState::rebooting;
  snapshot_.busy = true;
  snapshot_.progress_percent = 100;
  snapshot_.detail = "Firmware installed. Restarting PrintDeck...";
}
bool FirmwareUpdateService::request_install() {
  if (scheduler_task_ == nullptr) return false;
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.update_available || firmware_url_.empty() || snapshot_.busy) return false;
  snapshot_.state = FirmwareUpdateState::downloading; snapshot_.busy = true;
  snapshot_.progress_percent = 0; snapshot_.detail = "Preparing the firmware update...";
  install_requested_ = true;
  if (scheduler_task_ != nullptr) xTaskNotifyGive(scheduler_task_);
  return true;
}
bool FirmwareUpdateService::request_url_install(std::string url) {
  if (scheduler_task_ == nullptr) return false;
  if (url.rfind("https://", 0) != 0 || url.size() > 512 ||
      url.find_first_of("\r\n\t ") != std::string::npos) return false;
  // A merged `full` image is intended for a factory/USB flash and cannot be
  // handed directly to esp_https_ota.  For our official release assets, map it
  // to the matching application-only image so both GitHub download links do
  // what a user reasonably expects in Web Config.
  if (url.rfind(kOfficialReleasePrefix, 0) == 0) {
    const std::size_t filename = url.find_last_of('/');
    if (filename == std::string::npos) return false;
    const std::size_t name_offset = filename + 1;
    map_full_asset_to_ota(url, name_offset, kFirmwareFullAssetPrefix,
                          kFirmwareOtaAssetPrefix);
    map_full_asset_to_ota(url, name_offset, kLegacyFirmwareFullAssetPrefix,
                          kLegacyFirmwareOtaAssetPrefix);
    // Official release assets are hardware-specific. Rejecting another
    // PrintDeck target here prevents an accidental cross-flash from the
    // release picker or a pasted official download link.
    if (!matches_prefix(url, name_offset, kFirmwareOtaAssetPrefix) &&
        !matches_prefix(url, name_offset, kLegacyFirmwareOtaAssetPrefix)) {
      return false;
    }
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.busy) return false;
  firmware_url_ = std::move(url);
  firmware_sha256_.reset();
  snapshot_.state = FirmwareUpdateState::downloading;
  snapshot_.busy = true;
  snapshot_.progress_percent = 0;
  snapshot_.detail = "Preparing the firmware from the supplied HTTPS address...";
  install_requested_ = true;
  if (scheduler_task_ != nullptr) xTaskNotifyGive(scheduler_task_);
  return true;
}
FirmwareUpdateSnapshot FirmwareUpdateService::snapshot() const { const std::lock_guard<std::mutex> lock(mutex_); return snapshot_; }
void FirmwareUpdateService::scheduler_entry(void* context) {
  static_cast<FirmwareUpdateService*>(context)->scheduler_loop();
  vTaskDeleteWithCaps(nullptr);
}
void FirmwareUpdateService::scheduler_loop() {
  while (true) {
    poll();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
}
void FirmwareUpdateService::task_entry(void* context) {
  auto* service = static_cast<FirmwareUpdateService*>(context);
  service->task_loop();
  ESP_LOGI(kTag,
           "Update worker complete; stack high-water=%u, internal=%u, "
           "largest-internal=%u, largest-dma=%u",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)));
  service->worker_finished_.store(true, std::memory_order_release);
  if (service->scheduler_task_ != nullptr) {
    xTaskNotifyGive(service->scheduler_task_);
  }
  vTaskSuspend(nullptr);
  while (true) vTaskDelay(portMAX_DELAY);
}
void FirmwareUpdateService::task_loop() {
  const WorkerOperation operation =
      worker_operation_.load(std::memory_order_acquire);
  if (operation == WorkerOperation::check) {
    check_release();
  } else if (operation == WorkerOperation::install) {
    bool expected = false;
    if (install_running_.compare_exchange_strong(expected, true)) {
      install_release();
      install_running_.store(false, std::memory_order_release);
    }
  }
  worker_operation_.store(WorkerOperation::none, std::memory_order_release);
}
void FirmwareUpdateService::check_release() {
  Response response;
  const std::string manifest_url = std::string(kStableChannelBase) +
                                   kFirmwareStableChannel + "/stable.json";
  esp_http_client_config_t config{}; config.url = manifest_url.c_str(); config.event_handler = receive;
  config.user_data = &response; config.crt_bundle_attach = esp_crt_bundle_attach;
  const std::string user_agent = std::string("PrintDeck/") + PRINTDECK_VERSION +
                                 " (" + kBoardVariant + ")";
  config.timeout_ms = 15000; config.user_agent = user_agent.c_str();
  config.disable_auto_redirect = false;
  config.max_redirection_count = 3;
  auto* client = esp_http_client_init(&config);
  if (client == nullptr) { fail("PrintDeck could not create the update request."); return; }
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "Cache-Control", "no-cache");
  const esp_err_t result = esp_http_client_perform(client);
  const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status != 200 || response.too_large) { fail(response.too_large ? "The update response was too large." : "PrintDeck could not reach the update service."); return; }
  const auto channel = core::parse_firmware_channel(response.body, kBoardVariant);
  const auto current_version = core::parse_firmware_version(PRINTDECK_VERSION);
  if (!channel || !current_version) {
    fail("The release does not contain a valid PrintDeck version.");
    return;
  }
  const auto latest_version = core::parse_firmware_version(channel->version);
  const bool newer = latest_version && *latest_version > *current_version;
  record_successful_check();
  if (!newer) {
    const std::lock_guard<std::mutex> lock(mutex_);
    firmware_url_.clear(); firmware_sha256_.reset();
    snapshot_.latest_version = channel->version;
    snapshot_.update_available = false; snapshot_.busy = false; snapshot_.progress_percent = 0;
    snapshot_.state = FirmwareUpdateState::current;
    snapshot_.detail = "PrintDeck is up to date.";
    return;
  }
  if (channel->url.empty()) {
    fail("PrintDeck could not reach the update service.");
    return;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  firmware_url_ = channel->url;
  firmware_sha256_ = channel->sha256;
  snapshot_.latest_version = channel->version;
  snapshot_.update_available = true; snapshot_.busy = false; snapshot_.progress_percent = 0;
  snapshot_.state = FirmwareUpdateState::available;
  snapshot_.detail = "A newer PrintDeck release is ready.";
}
void FirmwareUpdateService::install_release() {
  std::string url;
  std::optional<std::array<std::uint8_t, 32>> expected_sha256;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    url = firmware_url_;
    expected_sha256 = firmware_sha256_;
    snapshot_.detail = "Downloading and validating the update...";
  }
  const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
  esp_http_client_config_t http{}; http.url = url.c_str(); http.crt_bundle_attach = esp_crt_bundle_attach;
  const std::string user_agent = std::string("PrintDeck/") + PRINTDECK_VERSION +
                                 " (" + kBoardVariant + ")";
  http.timeout_ms = 30000; http.buffer_size_tx = 2048; http.user_agent = user_agent.c_str();
  http.disable_auto_redirect = false; http.max_redirection_count = 5;
  esp_https_ota_config_t ota{}; ota.http_config = &http; esp_https_ota_handle_t handle = nullptr;
  esp_err_t result = esp_https_ota_begin(&ota, &handle);
  if (result != ESP_OK) {
    ESP_LOGW(kTag, "esp_https_ota_begin failed: %s", esp_err_to_name(result));
    fail("The firmware update could not start.");
    return;
  }
  while ((result = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) { const int read = esp_https_ota_get_image_len_read(handle), total = esp_https_ota_get_image_size(handle); if (total > 0) { const std::lock_guard<std::mutex> lock(mutex_); snapshot_.progress_percent = std::clamp(read * 100 / total, 0, 99); } }
  if (result != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) { esp_https_ota_abort(handle); fail("The firmware download was incomplete."); return; }
  const int image_size = esp_https_ota_get_image_len_read(handle);
  if (expected_sha256 &&
      (image_size <= 0 ||
       !partition_sha256_matches(update_partition,
                                 static_cast<std::size_t>(image_size),
                                 *expected_sha256))) {
    esp_https_ota_abort(handle);
    fail("The downloaded firmware did not pass validation.");
    return;
  }
  if (esp_https_ota_finish(handle) != ESP_OK) { fail("The downloaded firmware did not pass validation."); return; }
  ESP_LOGI(kTag, "OTA image validated; worker stack high-water=%u",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  { const std::lock_guard<std::mutex> lock(mutex_); snapshot_.state = FirmwareUpdateState::rebooting; snapshot_.progress_percent = 100; snapshot_.detail = "Update installed. Restarting PrintDeck..."; }
  vTaskDelay(pdMS_TO_TICKS(1200)); esp_restart();
}
void FirmwareUpdateService::record_successful_check() {
  const std::time_t checked_at = valid_epoch_now();
  if (checked_at <= 0) return;
  const std::uint64_t last_check_epoch = static_cast<std::uint64_t>(checked_at);
  last_check_epoch_.store(last_check_epoch, std::memory_order_release);
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_u64(handle, kLastCheckKey, last_check_epoch);
    nvs_commit(handle);
    nvs_close(handle);
  }
}
void FirmwareUpdateService::fail(std::string detail) { const std::lock_guard<std::mutex> lock(mutex_); firmware_url_.clear(); firmware_sha256_.reset(); snapshot_.state = FirmwareUpdateState::failed; snapshot_.update_available = false; snapshot_.busy = false; snapshot_.progress_percent = 0; snapshot_.detail = std::move(detail); ESP_LOGW(kTag, "%s", snapshot_.detail.c_str()); }
}  // namespace printdeck::platform
