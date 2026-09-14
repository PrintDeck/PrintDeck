#include "printdeck/platform/tinymaker_service.hpp"

#include <chrono>
#include "esp_log.h"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {
namespace {
bool acquire(std::unique_lock<std::timed_mutex>& lock, std::uint64_t deadline,
             const std::function<bool()>& cancelled) {
  while (prusalink_now_ms() < deadline && !cancelled()) {
    if (lock.try_lock_for(std::chrono::milliseconds(20))) return !cancelled();
  }
  return false;
}
}

PrusaLinkPollResult tinymaker_probe(const core::PrinterProfile& profile,
    std::uint64_t deadline, const std::function<bool()>& cancelled, PrusaLinkIdentity* identity) {
  std::unique_lock<std::timed_mutex> lock(prusalink_transaction_mutex(), std::defer_lock);
  if (!acquire(lock, deadline, cancelled))
    return {.error = cancelled() ? PrusaLinkError::cancelled : PrusaLinkError::timeout};
  PrusaLinkEspTransport transport;
  TinyMakerClient client(transport, prusalink_now_ms);
  if (profile.protocol != core::PrinterProtocol::tinymaker ||
      !client.configure(profile.endpoint, profile.id))
    return {.error = PrusaLinkError::invalid_configuration};
  auto result = client.poll(deadline, cancelled);
  if (identity && result.sample) *identity = client.identity();
  return result;
}

void TinyMakerAdapter::configure(const core::PrinterProfile* profile) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const core::PrinterProfile next = profile && profile->protocol == core::PrinterProtocol::tinymaker
      ? *profile : core::PrinterProfile{};
  if (core::same_printer_connection(profile_, next)) return;
  profile_ = next;
  ++generation_;
  core::PrinterSnapshot empty;
  empty.profile_id = next.id;
  empty.link = next.id ? core::LinkState::connecting : core::LinkState::stopped;
  snapshots_.replace(std::move(empty));
}

esp_err_t TinyMakerAdapter::start(const core::PrinterProfile* profile, const NetworkService& network) {
  configure(profile);
  const std::lock_guard<std::mutex> lock(mutex_);
  if (running_) return stopping_ ? ESP_ERR_INVALID_STATE : ESP_OK;
  if (!profile_.id) return ESP_ERR_INVALID_ARG;
  network_ = &network;
  stopping_ = false;
  running_ = true;
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "tinymaker", 24576, this, 4, &task_,
      kServiceCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    running_ = false;
    task_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void TinyMakerAdapter::stop() {
  const std::lock_guard<std::mutex> lock(mutex_);
  stopping_ = true;
  if (task_) xTaskNotifyGive(task_);
}

void TinyMakerAdapter::task_entry(void* context) {
  static_cast<TinyMakerAdapter*>(context)->run();
  vTaskDeleteWithCaps(nullptr);
}

void TinyMakerAdapter::run() {
  PrusaLinkEspTransport transport;
  TinyMakerClient client(transport, prusalink_now_ms);
  std::uint32_t revision = 0;
  bool configured = false;
  bool reported_stack = false;
  while (!stopping_) {
    core::PrinterProfile profile;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      profile = profile_;
      if (!configured || revision != generation_) {
        revision = generation_;
        configured = client.configure(profile.endpoint, profile.id);
      }
    }
    if (!profile.id) break;
    const auto cancelled = [&] {
      return stopping_.load() || revision != generation_ || !network_->status().station_connected;
    };
    const auto deadline = prusalink_now_ms() + 7000;
    std::unique_lock<std::timed_mutex> transaction(prusalink_transaction_mutex(), std::defer_lock);
    PrusaLinkPollResult result{.error = PrusaLinkError::invalid_configuration};
    if (configured && acquire(transaction, deadline, cancelled)) result = client.poll(deadline, cancelled);
    if (transaction.owns_lock()) transaction.unlock();
    if (result.sample && !reported_stack) {
      ESP_LOGI("tinymaker", "Status worker stack high-water=%u",
               static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
      reported_stack = true;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!cancelled()) {
        if (result.sample) {
          result.sample->snapshot.updated_at_ms = prusalink_now_ms();
          snapshots_.replace(std::move(result.sample->snapshot));
        } else {
          core::PrinterSnapshot failed;
          failed.profile_id = profile.id;
          failed.link = core::LinkState::failed;
          failed.updated_at_ms = prusalink_now_ms();
          snapshots_.replace(std::move(failed));
        }
      }
    }
    // Exposure labels require a fresh report within 2.5 seconds. Keep the
    // selected printer current without starting any additional live connection.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
  ESP_LOGI("tinymaker", "Stopped worker stack high-water=%u",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  const std::lock_guard<std::mutex> lock(mutex_);
  task_ = nullptr;
  running_ = false;
}

esp_err_t TinyMakerConnectionProbe::start(core::PrinterProfile profile, const NetworkService& network) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.running) return ESP_ERR_INVALID_STATE;
  profile_ = std::move(profile);
  network_ = &network;
  cancelled_ = false;
  verified_until_ = 0;
  snapshot_ = {.id = prusalink_random_cnonce(), .running = true};
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "tinymaker_check", 24576, this, 4, nullptr,
      kServiceCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    snapshot_.running = false;
    snapshot_.error = PrusaLinkError::unavailable;
    profile_ = {};
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void TinyMakerConnectionProbe::cancel(std::string_view id) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (id != snapshot_.id) return;
  cancelled_ = true;
  snapshot_.ready = false;
  verified_until_ = 0;
  if (!snapshot_.running) profile_ = {};
}

TinyMakerCheckSnapshot TinyMakerConnectionProbe::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

bool TinyMakerConnectionProbe::verified(const core::PrinterProfile& profile, std::string_view id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return !cancelled_ && snapshot_.ready && id == snapshot_.id &&
      prusalink_now_ms() < verified_until_ && core::same_printer_connection(profile_, profile);
}

void TinyMakerConnectionProbe::task_entry(void* context) {
  static_cast<TinyMakerConnectionProbe*>(context)->run();
  vTaskDeleteWithCaps(nullptr);
}

void TinyMakerConnectionProbe::run() {
  core::PrinterProfile profile;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    profile = profile_;
  }
  PrusaLinkIdentity identity;
  const auto cancelled = [&] { return cancelled_.load() || !network_->status().station_connected; };
  const auto result = tinymaker_probe(profile, prusalink_now_ms() + 12000, cancelled, &identity);
  ESP_LOGI("tinymaker", "Verification worker stack high-water=%u",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.running = false;
  snapshot_.ready = result.sample.has_value() && !cancelled();
  snapshot_.error = cancelled() ? PrusaLinkError::cancelled : result.error;
  snapshot_.identity = std::move(identity);
  verified_until_ = snapshot_.ready ? prusalink_now_ms() + 300000 : 0;
  if (!snapshot_.ready) profile_ = {};
}

}  // namespace printdeck::platform
