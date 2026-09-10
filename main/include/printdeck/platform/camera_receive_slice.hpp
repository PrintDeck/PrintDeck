#pragma once

#include <atomic>
#include <cstddef>

namespace printdeck::platform {

// Bound a camera peer's internal UDP drain without closing a socket from
// another task. The peer sees its ordinary would-block result and unwinds.
class CameraReceiveSlice {
 public:
  CameraReceiveSlice(const std::atomic<bool>& stop, std::size_t maximum_packets);
  ~CameraReceiveSlice();
  CameraReceiveSlice(const CameraReceiveSlice&) = delete;
  CameraReceiveSlice& operator=(const CameraReceiveSlice&) = delete;
};

}  // namespace printdeck::platform
