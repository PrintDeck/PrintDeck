#include "printdeck/platform/cpu_load.hpp"
#include <mutex>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"

namespace printdeck::platform {
core::CpuLoadSnapshot cpu_load_snapshot() {
  static std::mutex mutex;
  static core::CpuLoadMeter meter;
  const std::lock_guard lock(mutex);
#if configGENERATE_RUN_TIME_STATS
  return meter.sample(static_cast<std::uint32_t>(esp_timer_get_time()), {
      static_cast<std::uint32_t>(ulTaskGetIdleRunTimeCounterForCore(0)),
      static_cast<std::uint32_t>(ulTaskGetIdleRunTimeCounterForCore(1))});
#else
  return {};
#endif
}
}  // namespace printdeck::platform
