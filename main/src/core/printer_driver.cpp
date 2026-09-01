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

constexpr std::array<PrinterDriverDescriptor, 2> kDrivers{{
    {
        .protocol = PrinterProtocol::moonraker,
        .storage_id = 0,
        .id = "moonraker",
        .default_manufacturer = "Klipper",
        .default_brand = "klipper",
        .capabilities = capability_mask({PrinterCapability::api_key}),
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
}};

}  // namespace

const PrinterDriverDescriptor& printer_driver(PrinterProtocol protocol) {
  for (const PrinterDriverDescriptor& driver : kDrivers) {
    if (driver.protocol == protocol) return driver;
  }
  return kDrivers.front();
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
