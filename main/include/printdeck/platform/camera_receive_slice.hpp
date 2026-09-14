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
enum class CameraIdleTraceSite { none, callback, outer };

struct CameraIdleDiagnostics {
  std::uint32_t calls = 0;
  std::uint32_t waits = 0;
  std::uint32_t expiries = 0;
  std::uint32_t cancellations = 0;
  std::uint32_t unregistered = 0;
  std::uint32_t maximum_idle_age_ms = 0;
  std::int64_t wait_total_us = 0;
  std::int64_t maximum_wait_us = 0;
};

struct CameraUdpTiming {
  std::uint32_t calls = 0;
  std::int64_t total_us = 0;
  std::int64_t maximum_us = 0;
};

struct CameraUdpReceiveDiagnostics {
  CameraUdpTiming positive;
  CameraUdpTiming zero;
  CameraUdpTiming error;
  std::uint64_t received_bytes = 0;
};

struct CameraUdpSendDiagnostics {
  CameraUdpTiming timing;
  std::uint32_t positive = 0;
  std::uint32_t zero = 0;
  std::uint32_t errors = 0;
  std::uint32_t minus_200 = 0;
  std::uint64_t requested_bytes = 0;
  std::uint64_t sent_bytes = 0;
};

struct CameraReceiveDiagnostics {
  std::uint32_t packets_received = 0;
  std::uint32_t packet_budget_blocks = 0;
  std::int64_t maximum_receive_gap_us = 0;
  // Header observations before SRTP authentication, restricted to payload 98.
  std::uint32_t rtp_header_packets = 0;
  std::uint32_t rtp_unique_packets = 0;
  std::uint32_t rtp_markers = 0;
  std::uint32_t rtp_reordered = 0;
  std::uint32_t rtp_duplicates = 0;
  std::uint32_t rtp_late_or_discontinuous = 0;
  std::uint32_t rtp_reseeds = 0;
  std::uint32_t rtp_untracked = 0;
  std::uint32_t rtp_invalid_headers = 0;
  std::uint32_t rtp_missing_finalized = 0;
  std::uint32_t rtp_missing_pending = 0;
  // Absolute monotonic microseconds; retained across reporting windows.
  std::int64_t first_rtp_us = -1;
  std::int64_t first_marker_us = -1;
  // Wall time, including preemption, peer processing and callback waits.
  // Main-loop counts include K2 negotiation; they are not an execution bound.
  std::uint32_t main_loop_calls = 0;
  std::int64_t main_loop_total_us = 0;
  std::int64_t maximum_main_loop_us = 0;
  // Actual receiver-owned srtp_unprotect calls, including failures. Wall time
  // includes preemption/blocking and is nested within the peer main-loop time.
  std::uint32_t srtp_unprotect_calls = 0;
  std::uint32_t srtp_unprotect_failures = 0;
  std::uint64_t srtp_unprotect_input_bytes = 0;
  std::int64_t srtp_unprotect_total_us = 0;
  std::int64_t maximum_srtp_unprotect_us = 0;
  CameraUdpSendDiagnostics udp_send;
  CameraUdpReceiveDiagnostics udp_receive_wait;
  CameraUdpReceiveDiagnostics udp_receive_nowait;
  CameraIdleDiagnostics callback_idle;
  CameraIdleDiagnostics outer_idle;
};

// Only the receiver that last reset the diagnostics may record these samples.
// Decoder calls use site=none and remain separate from these counters.
class CameraIdleWaitTrace {
 public:
  CameraIdleWaitTrace(CameraIdleTraceSite site, std::uint32_t idle_age_ms,
                      bool registered);
  ~CameraIdleWaitTrace();
  void will_wait();
  void outcome(bool expired, bool cancelled = false);
  CameraIdleWaitTrace(const CameraIdleWaitTrace&) = delete;
  CameraIdleWaitTrace& operator=(const CameraIdleWaitTrace&) = delete;

 private:
  CameraIdleDiagnostics* metrics_ = nullptr;
  std::int64_t started_us_ = -1;
};

void record_camera_receive_main_loop_duration(std::int64_t elapsed_us);

// The camera receiver task owns these counters. Reset between peer sessions;
// taking a reporting window preserves timestamps and the two fixed SSRC windows.
// Missing sequences are finalized only after leaving a 256-packet reorder
// window. Later arrivals cannot amend an already reported window; reseeds and
// untracked streams make coverage incomplete. This is not a driver-loss count.
void reset_camera_receive_diagnostics();
CameraReceiveDiagnostics take_camera_receive_diagnostics();
#endif

}  // namespace printdeck::platform
