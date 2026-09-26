#pragma once
#include <mutex>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "printdeck/core/memory_admission.hpp"

namespace printdeck::platform {
inline std::mutex& memory_admission_mutex() { static std::mutex mutex; return mutex; }
inline core::MemoryAdmission& memory_admission() { static core::MemoryAdmission state; return state; }
inline void memory_image_activity(bool begin) {
  const std::lock_guard lock(memory_admission_mutex());
  if (begin) ++memory_admission().images;
  else --memory_admission().images;
}
inline void memory_thumbnail_pending() {
  const std::lock_guard lock(memory_admission_mutex());
  memory_admission().prefer_thumbnail(esp_timer_get_time() / 1000);
}
inline void memory_update_active(bool active) {
  const std::lock_guard lock(memory_admission_mutex());
  memory_admission().updating = active;
}
class MemoryLease {
 public:
  explicit MemoryLease(core::MemoryWork work) : work_(work) {
    const std::lock_guard lock(memory_admission_mutex());
    reason_ = memory_admission().acquire(work,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        esp_timer_get_time() / 1000);
  }
  ~MemoryLease() { if (*this) { const std::lock_guard lock(memory_admission_mutex()); memory_admission().release(work_); } }
  MemoryLease(const MemoryLease&) = delete;
  MemoryLease& operator=(const MemoryLease&) = delete;
  explicit operator bool() const { return reason_ == core::MemoryDeferral::none; }
  core::MemoryDeferral reason() const { return reason_; }
 private:
  core::MemoryWork work_;
  core::MemoryDeferral reason_;
};
} // namespace printdeck::platform
