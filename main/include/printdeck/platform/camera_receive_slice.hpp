#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "sdkconfig.h"

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

#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
struct CameraReceiveDiagnostics {
  std::uint32_t packets_received = 0;
  std::uint32_t packet_budget_blocks = 0;
  std::int64_t maximum_receive_gap_us = 0;
};

// The camera receiver task owns these counters. Reset between peer sessions;
// taking a reporting window preserves the last receive time across windows.
void reset_camera_receive_diagnostics();
CameraReceiveDiagnostics take_camera_receive_diagnostics();
#endif

}  // namespace printdeck::platform
