#include "printdeck/core/preview_policy.hpp"
#include "printdeck/platform/print_preview_service.hpp"
#include "printdeck/platform/image_workspace.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

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
esp_err_t PrintPreviewService::start(PersistenceWorker& worker) {
  if (worker_ == &worker) {
    const std::lock_guard lock(mutex_);
    return stopped_ ? ESP_ERR_INVALID_STATE : ESP_OK;
  }
  if (worker_ || !worker.bind(PersistenceWorker::Slot::preview,
                              work_entry, this, cleanup_entry)) return ESP_ERR_INVALID_STATE;
  worker_ = &worker;
  return ESP_OK;
}
void PrintPreviewService::stop() {
  const std::lock_guard lock(mutex_);
  stopped_ = true;
  request_ = {};
  ++revision_;
  if (worker_) worker_->request(PersistenceWorker::Slot::preview);
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
  if (stopped_) return;
  if (request_.key == next.key && request_.printer == next.printer &&
      request_.phase == next.phase && request_.online == next.online &&
      request_.visible == next.visible && request_.image == next.image) return;
  request_ = std::move(next);
  ++revision_;
  if (worker_) worker_->request(PersistenceWorker::Slot::preview);
}
PrintPreviewService::Snapshot PrintPreviewService::snapshot() const {
  const std::lock_guard lock(mutex_);
  return snapshot_;
}
std::uint32_t PrintPreviewService::work_entry(void* context) {
  return static_cast<PrintPreviewService*>(context)->process();
}
void PrintPreviewService::cleanup_entry(void* context) {
  static_cast<PrintPreviewService*>(context)->cleanup();
}
void PrintPreviewService::cleanup() {
  transfer_.cancel();
  transferring_ = false;
  transfer_image_.reset();
  resident_.reset();
  const std::lock_guard lock(mutex_);
  stopped_ = true;
  snapshot_ = {};
  request_ = {};
}
std::uint32_t PrintPreviewService::process() {
  Request request;
  std::uint32_t revision;
  bool stopped;
  { const std::lock_guard lock(mutex_);
    request = request_; revision = revision_; stopped = stopped_;
    if (snapshot_.key != request.key || !request.visible || !request.online) {
      // Do not keep an obsolete published image beside a new transfer and
      // the newest pending observation. There is no usable old-key fallback.
      snapshot_ = {request.key, nullptr, false};
    }
  }
  if (stopped) { cleanup(); return PersistenceWorker::kNoRetry; }
  if (transferring_ && transfer_revision_ != revision) {
    transfer_.cancel();
    transferring_ = false;
    transfer_image_.reset();
  }
  if (!request.online) {
    resident_.reset();
    const std::lock_guard lock(mutex_);
    if (revision != revision_) return 0;
    snapshot_ = {request.key, nullptr, false};
    return PersistenceWorker::kNoRetry;
  }
  if (request.key != current_key_) {
    if (request.printer == current_printer_) cache_.remove(current_key_);
    current_key_ = request.key;
    current_printer_ = request.printer;
    resident_.reset();
    cleared_key_.clear();
  }
  Snapshot result;
  result.key = request.key;
  if (request.phase != core::JobPhase::unknown && !print_preview_job_active(request.phase)) {
    if (cleared_key_ != request.key) {
      cache_.remove(request.key);
      cleared_key_ = request.key;
    }
    resident_.reset();
  } else if (request.visible && print_preview_job_active(request.phase) && !request.key.empty()) {
    cleared_key_.clear();
    if (!resident_) {
      // Settings never wait behind a camera decode or capture. A pending
      // transfer owns its bytes, but releases this lock after each small step.
      ImageWorkspaceLock workspace(0);
      if (!workspace) return 500;
      if (transferring_) {
        const auto status = transfer_.step();
        if (status == PrintPreviewCache::Transfer::Result::pending) return 0;
        transferring_ = false;
        if (writing_ || status == PrintPreviewCache::Transfer::Result::complete) {
          resident_ = std::move(transfer_image_);
          if (status == PrintPreviewCache::Transfer::Result::complete)
            ESP_LOGI("preview_cache", "Print thumbnail %s flash", writing_ ? "stored in" : "loaded from");
          else ESP_LOGW("preview_cache", "Print thumbnail flash cache unavailable");
        } else {
          transfer_image_.reset();
          cache_.remove(request.key);
          result.fetch_needed = true;
        }
      } else if (request.image && !request.image->empty()) {
        transfer_image_ = request.image;
        transfer_.begin_write(cache_, request.key, *transfer_image_);
        writing_ = transferring_ = true;
        transfer_revision_ = revision;
        return 0;
      } else {
        const auto size = cache_.size(request.key);
        if (size) {
          if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) <
              size + 128 * 1024) return 500;
          transfer_image_ = std::make_shared<std::vector<std::uint8_t>>(size);
          transfer_.begin_read(cache_, request.key, *transfer_image_);
          writing_ = false;
          transferring_ = true;
          transfer_revision_ = revision;
          return 0;
        }
        result.fetch_needed = true;
      }
    }
    result.image = resident_;
  } else resident_.reset();
  { const std::lock_guard lock(mutex_);
    if (revision != revision_) { resident_.reset(); return 0; }
    snapshot_ = std::move(result);
  }
  return PersistenceWorker::kNoRetry;
}
}  // namespace printdeck::platform
