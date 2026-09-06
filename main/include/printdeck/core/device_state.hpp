#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "printdeck/core/job_state.hpp"

namespace printdeck::core {

enum class PrinterProtocol : std::uint8_t {
  moonraker = 0, bambu_lan = 1, prusalink = 2, elegoo_sdcp = 3, elegoo_cc2 = 4,
  // Storage ID 5 is retired and must not be reused.
};
enum class HttpAuthMode : std::uint8_t { api_key = 0, digest = 1 };
enum class LinkState : std::uint8_t { stopped, waiting_for_network, connecting, online, failed };
enum class PrinterReachability : std::uint8_t { unknown, online, offline };

struct AutomaticProbeDecision {
  std::uint8_t consecutive_failures = 0;
  bool publish = false;
};

struct PrinterProfile {
  std::uint32_t id = 0;
  PrinterProtocol protocol = PrinterProtocol::moonraker;
  std::string display_name;
  std::string endpoint;
  std::string api_key;
  std::string serial;
  std::string access_code;
  std::string manufacturer;
  std::string model;
  std::string brand;
  HttpAuthMode http_auth_mode = HttpAuthMode::api_key;
  std::string http_username;
  std::string http_password;
};

struct DeviceState {
  std::vector<PrinterProfile> profiles;
  std::uint32_t selected_profile = 0;

  const PrinterProfile* selected() const;
  bool select(std::uint32_t profile_id);
};

struct PrinterSnapshot {
  std::uint32_t profile_id = 0;
  LinkState link = LinkState::stopped;
  std::string link_detail;
  JobState job;
  std::uint64_t updated_at_ms = 0;
};

bool dashboard_available(std::uint32_t selected_profile,
                         const PrinterSnapshot* snapshot);
bool printer_selection_unavailable(std::uint32_t selected_profile,
                                   const PrinterSnapshot* snapshot,
                                   bool connection_grace_elapsed);
bool retain_last_known_job_during_reconnect(PrinterSnapshot& current,
                                            const PrinterSnapshot& last_known);
bool printer_selectable(bool selected, PrinterReachability reachability);
bool printer_check_allowed(std::uint64_t now_ms,
                           std::uint64_t last_attempt_ms,
                           std::uint64_t cooldown_ms);
AutomaticProbeDecision automatic_probe_decision(
    bool connected, std::uint8_t previous_consecutive_failures);
bool same_printer_connection(const PrinterProfile& first,
                             const PrinterProfile& second);

class SnapshotStore {
 public:
  SnapshotStore();
  ~SnapshotStore();
  SnapshotStore(const SnapshotStore&) = delete;
  SnapshotStore& operator=(const SnapshotStore&) = delete;

  PrinterSnapshot read() const;
  void read_into(PrinterSnapshot& destination) const;
  void replace(const PrinterSnapshot& next);
  void replace(PrinterSnapshot&& next);
  // Retire a connection without allocating a replacement snapshot or retaining telemetry.
  void invalidate(std::uint32_t profile_id, LinkState link, std::uint64_t now_ms = 0);

 private:
  mutable std::mutex mutex_;
  PrinterSnapshot* value_ = nullptr;
};

}  // namespace printdeck::core
