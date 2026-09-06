#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {

enum class ElegooError : std::uint8_t {
  none, invalid_configuration, cancelled, timeout, unavailable,
  authorization, unsupported_response, identity_mismatch, service_not_ready,
  capacity,
};

struct ElegooIdentity {
  std::string manufacturer = "ELEGOO";
  std::string model;
  std::string serial;
  std::string firmware;
};

struct ElegooPollResult {
  ElegooError error = ElegooError::none;
  std::optional<core::PrinterSnapshot> snapshot;
};

}  // namespace printdeck::platform
