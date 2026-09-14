#include "printdeck/platform/camera_receive_slice.hpp"

#include <cerrno>
#ifdef PRINTDECK_K2_UDP_PREFETCH
#include "printdeck/platform/camera_udp_prefetch.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#include "esp_timer.h"
#include "srtp.h"
#include "printdeck/platform/camera_transport_trace.h"
#endif

namespace printdeck::platform {
namespace {
std::atomic<TaskHandle_t> receiving_task{nullptr};
const std::atomic<bool>* stop_requested = nullptr;
std::size_t maximum_packets = 0;
std::size_t received_packets = 0;
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
CameraReceiveDiagnostics diagnostics;
TaskHandle_t diagnostics_task = nullptr;
std::int64_t last_receive_us = -1;
std::int64_t first_rtp_us = -1;
std::int64_t first_marker_us = -1;
constexpr unsigned kSequenceWindow = 256;
constexpr unsigned kMaximumForwardStep = 4096;
struct RtpSequenceWindow {
  std::uint64_t seen[4]{};  // Bit zero is the highest accepted sequence.
  std::uint32_t ssrc = 0;
  int socket = -1;
  std::uint16_t highest = 0;
  std::uint16_t span = 0;
  std::uint16_t candidate_next = 0;
  bool candidate = false;
};
RtpSequenceWindow rtp_windows[2];

unsigned received_in_window(const RtpSequenceWindow& window) {
  unsigned result = 0;
  for (const auto word : window.seen) result += __builtin_popcountll(word);
  return result;
}

void seed_window(RtpSequenceWindow& window, std::uint16_t sequence) {
  for (auto& word : window.seen) word = 0;
  window.seen[0] = 1;
  window.highest = sequence;
  window.span = 1;
  window.candidate = false;
}

// Constant-size bitmap work, even when a burst advances across many missing
// sequence numbers. Do not infer loss across large jumps or an inferred restart.
bool observe_sequence(RtpSequenceWindow& window, std::uint16_t sequence) {
  const unsigned forward = static_cast<std::uint16_t>(sequence - window.highest);
  if (forward != 0 && forward <= kMaximumForwardStep) {
    const unsigned previous_received = received_in_window(window);
    const unsigned combined_span = window.span + forward;
    if (forward >= kSequenceWindow) {
      for (auto& word : window.seen) word = 0;
    } else {
      const unsigned words = forward / 64;
      const unsigned bits = forward % 64;
      for (int index = 3; index >= 0; --index) {
        std::uint64_t value = 0;
        if (index >= static_cast<int>(words)) {
          value = window.seen[index - words] << bits;
          if (bits != 0 && index > static_cast<int>(words)) {
            value |= window.seen[index - words - 1] >> (64 - bits);
          }
        }
        window.seen[index] = value;
      }
    }
    const unsigned expired_received = previous_received - received_in_window(window);
    if (combined_span > kSequenceWindow) {
      diagnostics.rtp_missing_finalized += combined_span - kSequenceWindow - expired_received;
    }
    window.seen[0] |= 1;
    window.span = combined_span < kSequenceWindow ? combined_span : kSequenceWindow;
    window.highest = sequence;
    window.candidate = false;
    return true;
  }
  const unsigned behind = static_cast<std::uint16_t>(window.highest - sequence);
  if (behind < window.span) {
    window.candidate = false;
    const auto bit = std::uint64_t{1} << (behind % 64);
    auto& word = window.seen[behind / 64];
    if ((word & bit) != 0) {
      ++diagnostics.rtp_duplicates;
      return false;
    }
    word |= bit;
    ++diagnostics.rtp_reordered;
    return true;
  }
  ++diagnostics.rtp_late_or_discontinuous;
  if (window.candidate && sequence == window.candidate_next) {
    // Two sequential out-of-window headers suggest a new sequence origin.
    // This remains an inference: report it and discard unresolved old gaps.
    seed_window(window, sequence);
    ++diagnostics.rtp_reseeds;
    return true;
  }
  window.candidate = true;
  window.candidate_next = static_cast<std::uint16_t>(sequence + 1);
  return false;
}

void observe_rtp_header(int socket, const void* data, std::size_t size, std::int64_t now) {
  if (data == nullptr || size < 2) return;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  // STUN/DTLS do not use RTP version 2. RTCP's full second octet identifies
  // the multiplexed control range; do not confuse its low bits with a payload.
  if ((bytes[0] >> 6) != 2 || (bytes[1] >= 192 && bytes[1] <= 223) ||
      (bytes[1] & 127) != 98) return;
  std::size_t header_size = 12 + (bytes[0] & 15) * 4;
  if (size < header_size) { ++diagnostics.rtp_invalid_headers; return; }
  if ((bytes[0] & 16) != 0) {
    if (size - header_size < 4) { ++diagnostics.rtp_invalid_headers; return; }
    const unsigned extension_words = (static_cast<unsigned>(bytes[header_size + 2]) << 8) |
                                     bytes[header_size + 3];
    header_size += 4;
    if (extension_words > (size - header_size) / 4) {
      ++diagnostics.rtp_invalid_headers;
      return;
    }
  }
  ++diagnostics.rtp_header_packets;
  if (first_rtp_us < 0) first_rtp_us = now;
  const bool marker = (bytes[1] & 128) != 0;
  if (marker && first_marker_us < 0) first_marker_us = now;
  const std::uint16_t sequence = (static_cast<unsigned>(bytes[2]) << 8) | bytes[3];
  const std::uint32_t ssrc = (static_cast<std::uint32_t>(bytes[8]) << 24) |
      (static_cast<std::uint32_t>(bytes[9]) << 16) |
      (static_cast<std::uint32_t>(bytes[10]) << 8) | bytes[11];
  RtpSequenceWindow* vacant = nullptr;
  for (auto& window : rtp_windows) {
    if (window.span == 0) { if (vacant == nullptr) vacant = &window; continue; }
    if (window.ssrc == ssrc && window.socket == socket) {
      if (observe_sequence(window, sequence)) {
        ++diagnostics.rtp_unique_packets;
        if (marker) ++diagnostics.rtp_markers;
      }
      return;
    }
  }
  if (vacant == nullptr) { ++diagnostics.rtp_untracked; return; }
  vacant->socket = socket;
  vacant->ssrc = ssrc;
  seed_window(*vacant, sequence);
  ++diagnostics.rtp_unique_packets;
  if (marker) ++diagnostics.rtp_markers;
}
#endif
}  // namespace

#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
void reset_camera_receive_diagnostics() {
  diagnostics = {};
  diagnostics_task = xTaskGetCurrentTaskHandle();
  last_receive_us = -1;
  first_rtp_us = first_marker_us = -1;
  for (auto& window : rtp_windows) window = {};
}

CameraReceiveDiagnostics take_camera_receive_diagnostics() {
  CameraReceiveDiagnostics result = diagnostics;
  result.first_rtp_us = first_rtp_us;
  result.first_marker_us = first_marker_us;
  for (const auto& window : rtp_windows) {
    result.rtp_missing_pending += window.span - received_in_window(window);
  }
  diagnostics = {};
  return result;
}

CameraIdleWaitTrace::CameraIdleWaitTrace(CameraIdleTraceSite site,
                                         std::uint32_t idle_age_ms,
                                         bool registered) {
  if (site == CameraIdleTraceSite::none || diagnostics_task == nullptr ||
      diagnostics_task != xTaskGetCurrentTaskHandle()) return;
  metrics_ = site == CameraIdleTraceSite::callback
      ? &diagnostics.callback_idle : &diagnostics.outer_idle;
  ++metrics_->calls;
  if (!registered) ++metrics_->unregistered;
  if (idle_age_ms > metrics_->maximum_idle_age_ms)
    metrics_->maximum_idle_age_ms = idle_age_ms;
}

void CameraIdleWaitTrace::will_wait() {
  if (metrics_ != nullptr && started_us_ < 0) {
    ++metrics_->waits;
    started_us_ = esp_timer_get_time();
  }
}

void CameraIdleWaitTrace::outcome(bool expired, bool cancelled) {
  if (metrics_ == nullptr) return;
  if (expired) ++metrics_->expiries;
  if (cancelled) ++metrics_->cancellations;
}

CameraIdleWaitTrace::~CameraIdleWaitTrace() {
  if (metrics_ == nullptr || started_us_ < 0) return;
  const auto elapsed_us = esp_timer_get_time() - started_us_;
  metrics_->wait_total_us += elapsed_us;
  if (elapsed_us > metrics_->maximum_wait_us) metrics_->maximum_wait_us = elapsed_us;
}

void record_camera_receive_main_loop_duration(std::int64_t elapsed_us) {
  if (diagnostics_task == nullptr || diagnostics_task != xTaskGetCurrentTaskHandle() ||
      elapsed_us < 0) return;
  ++diagnostics.main_loop_calls;
  diagnostics.main_loop_total_us += elapsed_us;
  if (elapsed_us > diagnostics.maximum_main_loop_us)
    diagnostics.maximum_main_loop_us = elapsed_us;
}

extern "C" decltype(srtp_unprotect) __real_srtp_unprotect;
extern "C" decltype(srtp_unprotect) __wrap_srtp_unprotect;

extern "C" srtp_err_status_t __wrap_srtp_unprotect(srtp_t ctx,
                                                 const std::uint8_t* srtp,
                                                 std::size_t srtp_len,
                                                 std::uint8_t* rtp,
                                                 std::size_t* rtp_len) {
  const TaskHandle_t owner = receiving_task.load(std::memory_order_acquire);
  if (owner == nullptr || owner != xTaskGetCurrentTaskHandle() || owner != diagnostics_task)
    return __real_srtp_unprotect(ctx, srtp, srtp_len, rtp, rtp_len);
  const auto started_us = esp_timer_get_time();
  const auto result = __real_srtp_unprotect(ctx, srtp, srtp_len, rtp, rtp_len);
  const auto elapsed_us = esp_timer_get_time() - started_us;
  ++diagnostics.srtp_unprotect_calls;
  diagnostics.srtp_unprotect_input_bytes += srtp_len;
  if (result != srtp_err_status_ok) ++diagnostics.srtp_unprotect_failures;
  diagnostics.srtp_unprotect_total_us += elapsed_us;
  if (elapsed_us > diagnostics.maximum_srtp_unprotect_us)
    diagnostics.maximum_srtp_unprotect_us = elapsed_us;
  return result;
}

extern "C" bool printdeck_camera_transport_trace_owned() {
  const TaskHandle_t owner = receiving_task.load(std::memory_order_acquire);
  return owner != nullptr && owner == xTaskGetCurrentTaskHandle() && owner == diagnostics_task;
}

static void record_udp_timing(CameraUdpTiming& timing, std::int64_t elapsed_us) {
  ++timing.calls;
  timing.total_us += elapsed_us;
  if (elapsed_us > timing.maximum_us) timing.maximum_us = elapsed_us;
}

extern "C" void printdeck_camera_trace_udp_send(int requested_bytes, int result,
                                               std::int64_t elapsed_us) {
  if (!printdeck_camera_transport_trace_owned()) return;
  auto& metrics = diagnostics.udp_send;
  record_udp_timing(metrics.timing, elapsed_us);
  if (requested_bytes > 0) metrics.requested_bytes += static_cast<unsigned>(requested_bytes);
  if (result > 0) {
    ++metrics.positive;
    metrics.sent_bytes += static_cast<unsigned>(result);
  } else if (result == 0) {
    ++metrics.zero;
  } else {
    ++metrics.errors;
    if (result == -200) ++metrics.minus_200;
  }
}

extern "C" void printdeck_camera_trace_udp_receive(bool nowait, int result,
                                                  std::int64_t elapsed_us) {
  if (!printdeck_camera_transport_trace_owned()) return;
  auto& metrics = nowait ? diagnostics.udp_receive_nowait : diagnostics.udp_receive_wait;
  record_udp_timing(result > 0 ? metrics.positive : result == 0 ? metrics.zero : metrics.error,
                    elapsed_us);
  if (result > 0) metrics.received_bytes += static_cast<unsigned>(result);
}
#endif

#ifdef PRINTDECK_K2_UDP_PREFETCH
extern "C" uintptr_t printdeck_camera_udp_prefetch_task() {
  return reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
}
extern "C" int printdeck_camera_udp_prefetch_scope() {
  const TaskHandle_t owner = receiving_task.load(std::memory_order_acquire);
  if (owner == nullptr || owner != xTaskGetCurrentTaskHandle()) return 0;
  return stop_requested->load(std::memory_order_acquire) ? 2 : 1;
}
#endif

CameraReceiveSlice::CameraReceiveSlice(const std::atomic<bool>& stop,
                                       std::size_t packet_limit) {
  stop_requested = &stop;
  maximum_packets = packet_limit;
  received_packets = 0;
#ifdef PRINTDECK_K2_UDP_PREFETCH
  printdeck_camera_udp_prefetch_slice_begin();
#endif
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
      // recvfrom may report a datagram's original size when truncating; never
      // inspect beyond the caller's buffer. The receive result is untouched.
      observe_rtp_header(socket, buffer,
                         static_cast<std::size_t>(received) < length ? received : length, now);
    }
#endif
  }
  return received;
}

}  // namespace printdeck::platform
