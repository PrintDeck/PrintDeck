#pragma once

#include <cstdint>
#include <string>

#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {

struct BambuLocalConnection {
  std::string host;
  std::string serial;
  std::string access_code;
  std::string mqtt_username = "bblp";
  std::uint16_t mqtt_port = 8883;

  bool is_ready() const { return !host.empty() && !serial.empty() && !access_code.empty(); }
};

inline BambuLocalConnection bambu_local_connection(const core::PrinterProfile* profile) {
  if (profile == nullptr || profile->protocol != core::PrinterProtocol::bambu_lan) return {};
  return {.host = profile->endpoint,
          .serial = profile->serial,
          .access_code = profile->access_code,
          .mqtt_username = "bblp",
          .mqtt_port = 8883};
}

}  // namespace printdeck::platform
