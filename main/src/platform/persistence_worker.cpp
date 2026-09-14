#include "printdeck/platform/persistence_worker.hpp"

#include <algorithm>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {

bool PersistenceWorker::bind(Slot slot, Handler handler, void* context, Cleanup cleanup) {
  if (running()) return false;
  auto& mailbox = mailboxes_[static_cast<unsigned>(slot)];
  mailbox.handler = handler;
  mailbox.context = context;
  mailbox.cleanup = cleanup;
  return true;
}

esp_err_t PersistenceWorker::start() {
  if (stopping_.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
  if (running_.exchange(true, std::memory_order_acq_rel)) return ESP_OK;
  TaskHandle_t task = nullptr;
  // NVS and LittleFS can disable the flash/PSRAM cache. Keep the shared stack
  // internal, including when SD-backed reactions are active elsewhere.
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "persistence", 6144, this, 4,
      &task, kServiceCore, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
    running_.store(false, std::memory_order_release);
    return ESP_ERR_NO_MEM;
  }
  // The entry point publishes its own handle. It may already have stopped
  // before task creation returns; the creator must never restore that handle.
  return ESP_OK;
}

bool PersistenceWorker::request(Slot slot) {
  if (stopping_.load(std::memory_order_acquire)) return false;
  mailboxes_[static_cast<unsigned>(slot)].state.fetch_or(1, std::memory_order_release);
  if (stopping_.load(std::memory_order_acquire)) { cancel(slot); return false; }
  notify();
  return true;
}

void PersistenceWorker::notify() {
  constexpr std::uint32_t closed = 0x80000000U;
  auto users = notification_users_.load(std::memory_order_acquire);
  while (!(users & closed)) {
    if (notification_users_.compare_exchange_weak(users, users + 1U,
                                                 std::memory_order_acq_rel)) {
      if (auto task = task_.load(std::memory_order_acquire)) xTaskNotifyGive(task);
      notification_users_.fetch_sub(1, std::memory_order_release);
      return;
    }
  }
}

void PersistenceWorker::cancel(Slot slot) {
  auto& state = mailboxes_[static_cast<unsigned>(slot)].state;
  auto previous = state.load(std::memory_order_acquire);
  while (!state.compare_exchange_weak(previous, (previous + 2U) & ~1U,
                                      std::memory_order_acq_rel)) {}
  notify();
}

void PersistenceWorker::request_stop() {
  stopping_.store(true, std::memory_order_release);
  notify();
}

void PersistenceWorker::task_entry(void* context) {
  auto* worker = static_cast<PersistenceWorker*>(context);
  worker->task_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
  worker->run();
  constexpr std::uint32_t closed = 0x80000000U;
  worker->notification_users_.fetch_or(closed, std::memory_order_acq_rel);
  while (worker->notification_users_.load(std::memory_order_acquire) != closed) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  worker->task_.store(nullptr, std::memory_order_release);
  worker->running_.store(false, std::memory_order_release);
  // running=false means callbacks/resources are quiescent, not that IDLE has
  // already reclaimed a self-deleted task's stack and TCB.
  vTaskDeleteWithCaps(nullptr);
}

void PersistenceWorker::run() {
  while (!stopping_.load(std::memory_order_acquire)) {
    const auto now = esp_timer_get_time() / 1000;
    std::uint32_t wait_ms = 1000;
    bool dispatched = false;
    // Settings win at step boundaries, with one preview step after four
    // consecutive settings passes to avoid starvation during an input storm.
    const bool preview_first = settings_streak_ >= 4;
    for (unsigned position = 0; position < mailboxes_.size(); ++position) {
      const unsigned index = preview_first ? 1U - position : position;
      auto& mailbox = mailboxes_[index];
      auto state = mailbox.state.load(std::memory_order_acquire);
      const auto generation = state & ~1U;
      bool ready = false;
      if (state & 1U) {
        ready = mailbox.state.compare_exchange_strong(state, generation,
                                                      std::memory_order_acq_rel);
        if (!ready) { wait_ms = 0; continue; }
      } else if (mailbox.due_ms >= 0 && mailbox.due_generation == generation) {
        ready = now >= mailbox.due_ms;
        if (!ready) wait_ms = std::min(wait_ms,
            static_cast<std::uint32_t>(mailbox.due_ms - now));
      }
      if (!ready) continue;
      mailbox.due_ms = -1;
      if (!mailbox.handler || stopping_.load(std::memory_order_acquire) ||
          (mailbox.state.load(std::memory_order_acquire) & ~1U) != generation) continue;
      vTaskPrioritySet(nullptr, index == 0 ? 4 : 2);
      const auto retry = mailbox.handler(mailbox.context);
      vTaskPrioritySet(nullptr, 4);
      if (retry != kNoRetry &&
          (mailbox.state.load(std::memory_order_acquire) & ~1U) == generation) {
        mailbox.due_generation = generation;
        mailbox.due_ms = esp_timer_get_time() / 1000 + retry;
      }
      dispatched = true;
      settings_streak_ = index == 0 ? std::min(settings_streak_ + 1U, 4U) : 0;
      // A preview transfer can require hundreds of steps. Preserve an actual
      // scheduling window between them, even if notifications keep arriving.
      if (index == 1) vTaskDelay(pdMS_TO_TICKS(1));
      break;
    }
    if (!dispatched) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
  }
  for (auto& mailbox : mailboxes_) {
    mailbox.state.fetch_and(~1U, std::memory_order_acq_rel);
    mailbox.due_ms = -1;
    if (mailbox.cleanup) mailbox.cleanup(mailbox.context);
  }
}

}  // namespace printdeck::platform
