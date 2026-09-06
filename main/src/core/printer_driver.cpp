#include "printdeck/core/printer_driver.hpp"

#include <array>
#include <initializer_list>

namespace printdeck::core {
namespace {

constexpr std::uint32_t capability_mask(
    std::initializer_list<PrinterCapability> capabilities) {
  std::uint32_t result = 0;
  for (const PrinterCapability capability : capabilities) {
    result |= static_cast<std::uint32_t>(capability);
  }
  return result;
}

constexpr std::array<PrinterDriverDescriptor, 5> kDrivers{{
    {
        .protocol = PrinterProtocol::moonraker,
        .storage_id = 0,
        .id = "moonraker",
        .default_manufacturer = "Klipper",
        .default_brand = "klipper",
        .capabilities = capability_mask({PrinterCapability::api_key, PrinterCapability::http_endpoint}),
    },
    {
        .protocol = PrinterProtocol::bambu_lan,
        .storage_id = 1,
        .id = "bambu_lan",
        .default_manufacturer = "Bambu Lab",
        .default_brand = "bambu",
        .capabilities = capability_mask({PrinterCapability::serial_number,
                                         PrinterCapability::access_code,
                                         PrinterCapability::material_system,
                                         PrinterCapability::local_camera,
                                         PrinterCapability::chamber_light,
                                         PrinterCapability::compatibility_report}),
    },
    {
        .protocol = PrinterProtocol::prusalink,
        .storage_id = 2,
        .id = "prusalink",
        .default_manufacturer = "Prusa Research",
        .default_brand = "prusa",
        .capabilities = capability_mask({PrinterCapability::api_key,
                                         PrinterCapability::http_endpoint,
                                         PrinterCapability::http_digest}),
        .experimental = true,
    },
    {
        .protocol = PrinterProtocol::elegoo_sdcp,
        .storage_id = 3,
        .id = "elegoo_sdcp",
        .default_manufacturer = "ELEGOO",
        .default_brand = "elegoo",
        .capabilities = capability_mask({PrinterCapability::serial_number}),
        .experimental = true,
    },
    {
        .protocol = PrinterProtocol::elegoo_cc2,
        .storage_id = 4,
        .id = "elegoo_cc2",
        .default_manufacturer = "ELEGOO",
        .default_brand = "elegoo",
        .capabilities = capability_mask({PrinterCapability::serial_number,
                                         PrinterCapability::access_code}),
        .experimental = true,
    },
}};

constexpr PrinterDriverDescriptor kUnsupported{
    .protocol = static_cast<PrinterProtocol>(255), .storage_id = 255,
    .id = "unsupported", .default_manufacturer = "", .default_brand = "", .capabilities = 0};

}  // namespace

const PrinterDriverDescriptor& printer_driver(PrinterProtocol protocol) {
  for (const PrinterDriverDescriptor& driver : kDrivers) {
    if (driver.protocol == protocol) return driver;
  }
  return kUnsupported;
}

bool printer_protocol_supported(PrinterProtocol protocol) {
  return &printer_driver(protocol) != &kUnsupported;
}

bool printer_protocol_from_id(std::string_view id, PrinterProtocol& protocol) {
  for (const PrinterDriverDescriptor& driver : kDrivers) {
    if (id == driver.id) {
      protocol = driver.protocol;
      return true;
    }
  }
  return false;
}

bool printer_protocol_from_storage_id(std::uint8_t storage_id, PrinterProtocol& protocol) {
  for (const PrinterDriverDescriptor& driver : kDrivers) {
    if (storage_id == driver.storage_id) {
      protocol = driver.protocol;
      return true;
    }
  }
  return false;
}

bool printer_supports(PrinterProtocol protocol, PrinterCapability capability) {
  return (printer_driver(protocol).capabilities & static_cast<std::uint32_t>(capability)) != 0;
}

}  // namespace printdeck::core
