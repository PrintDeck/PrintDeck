#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace printdeck::platform {

// Coalesced mailboxes, not a queue of settings/image copies. Handlers must
// finish one bounded step and release every lock before returning. In
// particular, preview steps never invoke the settings handler recursively.
class PersistenceWorker {
 public:
  enum class Slot : unsigned { settings, preview, mqtt, cloud };
  static constexpr std::uint32_t kNoRetry = std::numeric_limits<std::uint32_t>::max();
  using Handler = std::uint32_t (*)(void*);  // delay in ms, or kNoRetry
  using Cleanup = void (*)(void*);
  bool bind(Slot slot, Handler handler, void* context, Cleanup cleanup = nullptr);
  // Allocation failure may be retried. Once stopped, this lifetime is closed;
  // Runtime keeps its worker alive permanently rather than restarting it.
  esp_err_t start();
  bool request(Slot slot);
  // Cancels queued/delayed work. A step already executing finishes normally;
  // its returned retry is discarded. The owner still owns its I/O state.
  void cancel(Slot slot);
  void request_stop();
  bool running() const { return running_.load(std::memory_order_acquire); }

 private:
  struct Mailbox {
    Handler handler = nullptr;
    Cleanup cleanup = nullptr;
    void* context = nullptr;
    // Low bit: pending. Other bits: cancellation generation. One atomic word
    // gives concurrent request/cancel a defined order without a UI mutex.
    std::atomic<std::uint32_t> state{0};
    std::int64_t due_ms = -1;       // consumer-owned
    std::uint32_t due_generation = 0;
  };
  static void task_entry(void* context);
  void notify();
  void run();
  std::array<Mailbox, 4> mailboxes_{};
  std::atomic<TaskHandle_t> task_{nullptr};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  // The high bit closes notifications before task deletion; the remaining
  // bits keep an already-started notification from using a freed task handle.
  std::atomic<std::uint32_t> notification_users_{0};
  unsigned settings_streak_ = 0;
  unsigned next_background_ = 1;
};

}  // namespace printdeck::platform
