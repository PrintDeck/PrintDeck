#include "printdeck/platform/task_affinity.hpp"

#include "lvgl.h"
#include "src/osal/lv_os_private.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

namespace printdeck::platform {
namespace {
void run_lvgl_thread(void* argument) {
  auto* thread = static_cast<lv_thread_t*>(argument);
  thread->pvStartRoutine(thread->pTaskArg);
#if defined(LV_FREERTOS_TASK_STACK_ALLOC_CAPS)
  vTaskDeleteWithCaps(nullptr);
#else
  vTaskDelete(nullptr);
#endif
}
}  // namespace

// LVGL's FreeRTOS port creates unpinned software-draw workers. Keep them on
// the UI core as well as the main LVGL task, so a camera decode on core 0
// cannot delay a worker while LVGL holds the display lock on core 1.
extern "C" lv_result_t __wrap_lv_thread_init(
    lv_thread_t* thread, const char* name, lv_thread_prio_t priority,
    void (*entry)(void*), size_t stack_size, void* argument) {
  thread->pTaskArg = argument;
  thread->pvStartRoutine = entry;
  // Preserve the stack allocation used by the configured LVGL FreeRTOS port.
  const auto stack_depth = static_cast<configSTACK_DEPTH_TYPE>(stack_size / sizeof(StackType_t));
#if defined(LV_FREERTOS_TASK_STACK_ALLOC_CAPS)
  const auto result = xTaskCreatePinnedToCoreWithCaps(
      run_lvgl_thread, name, stack_depth, thread, tskIDLE_PRIORITY + priority,
      &thread->xTaskHandle, kLvglCore, LV_FREERTOS_TASK_STACK_ALLOC_CAPS);
#else
  const auto result = xTaskCreatePinnedToCore(
      run_lvgl_thread, name, stack_depth, thread, tskIDLE_PRIORITY + priority,
      &thread->xTaskHandle, kLvglCore);
#endif
  if (result != pdPASS) return LV_RESULT_INVALID;
  ESP_LOGI("display", "LVGL worker %s pinned to core %d", name, kLvglCore);
  return LV_RESULT_OK;
}
}  // namespace printdeck::platform
