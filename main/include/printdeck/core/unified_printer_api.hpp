#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "printdeck/core/device_state.hpp"

namespace printdeck::core {

enum class UnifiedApiDetailLevel : std::uint8_t { summary, full };

// A credential-free view assembled from normalized PrintDeck state. Public API
// serializers accept this type instead of PrinterProfile so credentials and
// printer serial numbers cannot be exposed accidentally.
struct UnifiedPrinterView {
  std::uint32_t id = 0;
  PrinterProtocol protocol = PrinterProtocol::moonraker;
  std::string display_name;
  std::string endpoint;
  std::string manufacturer;
  std::string model;
  bool selected = false;
  PrinterReachability reachability = PrinterReachability::unknown;
  UnifiedApiDetailLevel detail_level = UnifiedApiDetailLevel::summary;
  PrinterSnapshot snapshot;
  bool stale = true;
};

struct UnifiedDevicePower {
  bool available = false;
  bool battery_present = false;
  std::uint8_t battery_percent = 0;
  bool charging = false;
  bool external_power = false;
};

std::string unified_api_printers_json(std::span<const UnifiedPrinterView> printers);
std::string unified_api_statuses_json(std::span<const UnifiedPrinterView> printers);
std::string unified_api_snapshot_json(std::span<const UnifiedPrinterView> printers,
                                      const UnifiedDevicePower& power = {});
std::string unified_api_printer_json(const UnifiedPrinterView& printer);
std::string unified_api_status_json(const UnifiedPrinterView& printer);
std::string unified_api_nozzles_json(const UnifiedPrinterView& printer);
std::string unified_api_materials_json(const UnifiedPrinterView& printer);

}  // namespace printdeck::core
