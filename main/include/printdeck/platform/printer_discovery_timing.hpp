#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace printdeck::platform {

struct PrinterDiscoveryTiming {
  // Bambu printers may answer one M-SEARCH inconsistently. Space five rounds
  // across four seconds, then retain the listeners for the complete final MX=2
  // response window while the bounded subnet probe continues.
  static constexpr std::size_t bambu_ssdp_search_rounds = 5;
  static constexpr std::uint32_t bambu_ssdp_search_interval_ms = 1000;
  static constexpr std::uint32_t bambu_ssdp_reply_wait_ms = 2000;

  // Keep the per-host Bambu TCP probe bounded so a full /24 stays inside the
  // safety limit. Once 8883 is reachable, allow enough time for the separate,
  // verified TLS handshake. Other PrintDeck Bambu paths use six seconds or longer.
  static constexpr std::uint32_t bambu_tcp_connect_timeout_ms = 250;
  static constexpr std::uint32_t bambu_tls_handshake_timeout_ms = 4000;

  static constexpr std::uint32_t bounded_wait_ms(std::uint64_t now_ms,
                                                 std::uint64_t deadline_ms,
                                                 std::uint32_t preferred_ms) {
    if (now_ms >= deadline_ms) return 0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        deadline_ms - now_ms, preferred_ms));
  }

  static constexpr std::uint32_t bambu_ssdp_search_offset_ms(std::size_t round) {
    return static_cast<std::uint32_t>(round * bambu_ssdp_search_interval_ms);
  }

  static constexpr bool bambu_ssdp_should_send(std::size_t rounds_sent,
                                               std::uint64_t now_ms,
                                               std::uint64_t next_send_ms) {
    return rounds_sent < bambu_ssdp_search_rounds && now_ms >= next_send_ms;
  }

  static constexpr bool bambu_ssdp_responses_complete(std::size_t rounds_sent,
                                                      std::uint64_t now_ms,
                                                      std::uint64_t last_send_ms) {
    return rounds_sent >= bambu_ssdp_search_rounds && now_ms >= last_send_ms &&
           now_ms - last_send_ms >= bambu_ssdp_reply_wait_ms;
  }
};

static_assert(PrinterDiscoveryTiming::bambu_ssdp_search_rounds > 0);
static_assert(
    PrinterDiscoveryTiming::bambu_ssdp_search_offset_ms(
        PrinterDiscoveryTiming::bambu_ssdp_search_rounds - 1) +
        PrinterDiscoveryTiming::bambu_ssdp_reply_wait_ms == 6000,
    "The nominal SSDP window must include every retry and the final MX=2 wait");

}  // namespace printdeck::platform
