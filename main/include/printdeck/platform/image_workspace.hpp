#pragma once

#include <cstdint>
#include "printdeck/platform/memory_admission.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace printdeck::platform {

// A full H264 picture and the composed display capture each need several MiB.
// Serialize their temporary buffers through capture transmission, keeping this
// recursive lock outside the display lock. Capture callers own the encoded PNG
// until their transport has finished; capture_png also protects direct callers.
inline SemaphoreHandle_t image_workspace_mutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutexStatic(&storage);
  return mutex;
}

class ImageWorkspaceLock {
 public:
  explicit ImageWorkspaceLock(std::uint32_t timeout_ms)
      : mutex_(image_workspace_mutex()),
        acquired_(xSemaphoreTakeRecursive(mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) { if (acquired_) memory_image_activity(true); }
  ~ImageWorkspaceLock() { if (acquired_) { memory_image_activity(false); xSemaphoreGiveRecursive(mutex_); } }
  explicit operator bool() const { return acquired_; }
  ImageWorkspaceLock(const ImageWorkspaceLock&) = delete;
  ImageWorkspaceLock& operator=(const ImageWorkspaceLock&) = delete;

 private:
  SemaphoreHandle_t mutex_;
  bool acquired_;
};

}  // namespace printdeck::platform
