#pragma once

#include <cstdint>
#include <string_view>

#include "printdeck/core/device_state.hpp"

namespace printdeck::core {

enum class PrinterCapability : std::uint32_t {
  api_key = 1U << 0U,
  serial_number = 1U << 1U,
  access_code = 1U << 2U,
  material_system = 1U << 3U,
  local_camera = 1U << 4U,
  chamber_light = 1U << 5U,
  compatibility_report = 1U << 6U,
  http_endpoint = 1U << 7U,
  http_digest = 1U << 8U,
};

struct PrinterDriverDescriptor {
  PrinterProtocol protocol;
  std::uint8_t storage_id;
  const char* id;
  const char* default_manufacturer;
  const char* default_brand;
  std::uint32_t capabilities;
  std::uint16_t default_port = 0;
  bool experimental = false;
  bool resin = false;
  bool dashboard = true;
};

const PrinterDriverDescriptor& printer_driver(PrinterProtocol protocol);
bool printer_protocol_supported(PrinterProtocol protocol);
bool printer_protocol_from_id(std::string_view id, PrinterProtocol& protocol);
bool printer_protocol_from_storage_id(std::uint8_t storage_id, PrinterProtocol& protocol);
bool printer_supports(PrinterProtocol protocol, PrinterCapability capability);

}  // namespace printdeck::core
