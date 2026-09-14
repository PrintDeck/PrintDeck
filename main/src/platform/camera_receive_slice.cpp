#include "printdeck/platform/camera_receive_slice.hpp"

#include <cerrno>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#include "esp_timer.h"
#endif

namespace printdeck::platform {
namespace {
std::atomic<TaskHandle_t> receiving_task{nullptr};
const std::atomic<bool>* stop_requested = nullptr;
std::size_t maximum_packets = 0;
std::size_t received_packets = 0;
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
CameraReceiveDiagnostics diagnostics;
std::int64_t last_receive_us = -1;
#endif
}  // namespace

#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
void reset_camera_receive_diagnostics() {
  diagnostics = {};
  last_receive_us = -1;
}

CameraReceiveDiagnostics take_camera_receive_diagnostics() {
  const CameraReceiveDiagnostics result = diagnostics;
  diagnostics = {};
  return result;
}
#endif

CameraReceiveSlice::CameraReceiveSlice(const std::atomic<bool>& stop,
                                       std::size_t packet_limit) {
  stop_requested = &stop;
  maximum_packets = packet_limit;
  received_packets = 0;
  receiving_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
}

CameraReceiveSlice::~CameraReceiveSlice() {
  receiving_task.store(nullptr, std::memory_order_release);
}

extern "C" ssize_t __real_lwip_recvfrom(int socket, void* buffer, size_t length,
                                       int flags, struct sockaddr* address,
                                       socklen_t* address_length);

extern "C" ssize_t __wrap_lwip_recvfrom(int socket, void* buffer, size_t length,
                                       int flags, struct sockaddr* address,
                                       socklen_t* address_length) {
  const TaskHandle_t owner = receiving_task.load(std::memory_order_acquire);
  if (owner != nullptr && owner == xTaskGetCurrentTaskHandle()) {
    const bool stopped = stop_requested->load(std::memory_order_acquire);
    const bool exhausted = maximum_packets != 0 && received_packets >= maximum_packets;
    if (stopped || exhausted) {
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
      if (!stopped && exhausted) ++diagnostics.packet_budget_blocks;
#endif
      errno = EWOULDBLOCK;
      return -1;
    }
  }
  const auto received = __real_lwip_recvfrom(socket, buffer, length, flags, address, address_length);
  if (received > 0 && owner != nullptr && owner == xTaskGetCurrentTaskHandle()) {
    ++received_packets;
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    // Ignore handshake/unbounded slices and other tasks' UDP. This measures
    // successful receive completions; gaps also include silence from the peer.
    if (maximum_packets != 0) {
      ++diagnostics.packets_received;
      const std::int64_t now = esp_timer_get_time();
      if (last_receive_us >= 0 && now - last_receive_us > diagnostics.maximum_receive_gap_us) {
        diagnostics.maximum_receive_gap_us = now - last_receive_us;
      }
      last_receive_us = now;
    }
#endif
  }
  return received;
}

}  // namespace printdeck::platform
