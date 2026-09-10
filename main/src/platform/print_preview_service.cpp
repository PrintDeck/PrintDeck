#include "printdeck/core/preview_policy.hpp"
#include "printdeck/platform/print_preview_service.hpp"
#include "printdeck/platform/image_workspace.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

namespace printdeck::platform {
namespace {
using core::print_preview_job_active;
std::string printer_key(const core::PrinterProfile& profile) {
  return std::to_string(profile.id) + "\n" +
      std::to_string(static_cast<int>(profile.protocol)) + "\n" + profile.endpoint;
}
}
std::string PrintPreviewService::key(const core::PrinterProfile& profile,
                                    const core::JobState& job) {
  if (job.preview_hint.empty() && job.gcode_file.empty() && job.name.empty()) return {};
  // Retire stock GK3 black-history thumbnails cached by earlier firmware.
  const auto format = profile.protocol == core::PrinterProtocol::uniformation_sdcp ? "ctb-model-v1\n" : "";
  return printer_key(profile) + "\n" + format + job.preview_hint + "\n" + job.gcode_file +
      "\n" + job.name + "\n" + job.preview_plate_hint;
}
esp_err_t PrintPreviewService::start() {
  if (task_) return ESP_OK;
  // LittleFS writes may disable the flash/PSRAM cache. Only this internal-stack
  // core-0 worker touches the files; UI and protocol workers exchange references.
  return xTaskCreatePinnedToCoreWithCaps(task_entry, "preview_cache", 6144, this, 2,
      &task_, kServiceCore, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS
      ? ESP_OK : ESP_ERR_NO_MEM;
}
void PrintPreviewService::update(const core::PrinterProfile* profile,
                                 const core::PrinterSnapshot& state, bool visible) {
  Request next;
  if (profile && state.profile_id == profile->id) {
    next.key = key(*profile, state.job);
    next.printer = printer_key(*profile);
    next.phase = state.job.phase;
    next.online = state.link == core::LinkState::online;
    next.visible = visible;
    if (visible && next.online && print_preview_job_active(next.phase)) next.image = state.job.preview;
  }
  const std::lock_guard lock(mutex_);
  if (request_.key == next.key && request_.printer == next.printer &&
      request_.phase == next.phase && request_.online == next.online &&
      request_.visible == next.visible && request_.image == next.image) return;
  request_ = std::move(next);
  ++revision_;
  if (task_) xTaskNotifyGive(task_);
}
PrintPreviewService::Snapshot PrintPreviewService::snapshot() const {
  const std::lock_guard lock(mutex_);
  return snapshot_;
}
void PrintPreviewService::task_entry(void* context) {
  static_cast<PrintPreviewService*>(context)->run();
}
void PrintPreviewService::run() {
  std::string current_key, current_printer, cleared_key;
  std::shared_ptr<std::vector<std::uint8_t>> resident;
  bool retry = false;
  while (true) {
    ulTaskNotifyTake(pdTRUE, retry ? pdMS_TO_TICKS(500) : portMAX_DELAY);
    Request request;
    std::uint32_t revision;
    { const std::lock_guard lock(mutex_); request = request_; revision = revision_; }
    retry = false;
    if (!request.online) {
      resident.reset();
      const std::lock_guard lock(mutex_);
      if (revision == revision_) snapshot_ = {request.key, nullptr, false};
      else retry = true;
      continue;
    }
    if (request.key != current_key) {
      if (request.online && request.printer == current_printer) cache_.remove(current_key);
      current_key = request.key;
      current_printer = request.printer;
      resident.reset();
      cleared_key.clear();
    }
    Snapshot result;
    result.key = request.key;
    if (request.online && request.phase != core::JobPhase::unknown && !print_preview_job_active(request.phase)) {
      if (cleared_key != request.key) {
        cache_.remove(request.key);
        cleared_key = request.key;
      }
      resident.reset();
    } else if (request.visible && request.online && print_preview_job_active(request.phase) && !request.key.empty()) {
      cleared_key.clear();
      if (!resident) {
        ImageWorkspaceLock workspace(50);
        if (!workspace) retry = true;
        else if (request.image && !request.image->empty()) {
          if (cache_.write(request.key, *request.image))
            ESP_LOGI("preview_cache", "Print thumbnail stored in flash (%u bytes)",
                     static_cast<unsigned>(request.image->size()));
          else ESP_LOGW("preview_cache", "Print thumbnail flash cache unavailable");
          resident = request.image;
        } else {
          const auto size = cache_.size(request.key);
          if (size) {
            if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) <
                size + 128 * 1024) retry = true;
            else {
              auto image = std::make_shared<std::vector<std::uint8_t>>(size);
              if (cache_.read(request.key, *image)) {
                resident = std::move(image);
                ESP_LOGI("preview_cache", "Print thumbnail loaded from flash");
              } else { cache_.remove(request.key); result.fetch_needed = true; }
            }
          } else result.fetch_needed = true;
        }
      }
      result.image = resident;
    } else resident.reset();
    { const std::lock_guard lock(mutex_);
      if (revision == revision_) snapshot_ = std::move(result);
      else { resident.reset(); retry = true; } }
  }
}
}  // namespace printdeck::platform
