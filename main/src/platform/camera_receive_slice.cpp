#include "printdeck/platform/camera_receive_slice.hpp"

#include <cerrno>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace printdeck::platform {
namespace {
std::atomic<TaskHandle_t> receiving_task{nullptr};
const std::atomic<bool>* stop_requested = nullptr;
std::size_t maximum_packets = 0;
std::size_t received_packets = 0;
}  // namespace

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
  if (owner != nullptr && owner == xTaskGetCurrentTaskHandle() &&
      (stop_requested->load(std::memory_order_acquire) ||
       (maximum_packets != 0 && received_packets >= maximum_packets))) {
    errno = EWOULDBLOCK;
    return -1;
  }
  const auto received = __real_lwip_recvfrom(socket, buffer, length, flags, address, address_length);
  if (received > 0 && owner != nullptr && owner == xTaskGetCurrentTaskHandle())
    ++received_packets;
  return received;
}

}  // namespace printdeck::platform
