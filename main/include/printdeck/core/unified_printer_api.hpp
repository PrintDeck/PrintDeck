#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "printdeck/core/device_state.hpp"
#include "printdeck/core/print_events.hpp"

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
  std::string brand;
  bool selected = false;
  bool printer_control_enabled = false;
  PrinterReachability reachability = PrinterReachability::unknown;
  UnifiedApiDetailLevel detail_level = UnifiedApiDetailLevel::summary;
  PrinterSnapshot snapshot;
  bool stale = true;
  PrintEventHistory print_events;
  std::uint64_t observed_at_ms = 0;
};

struct UnifiedDevicePower {
  bool available = false;
  bool battery_present = false;
  std::uint8_t battery_percent = 0;
  bool charging = false;
  bool external_power = false;
};

// Transport-independent state contract. Local image URLs are supplied only by
// Web Config; an eventual cloud transport must resolve media independently.
struct PrinterStateMedia {
  std::string model;
  std::string layer;
  std::uint64_t layer_valid_for_ms = 0;
};
std::string printer_state_json(const UnifiedPrinterView& printer, std::uint64_t now_ms,
                              std::int64_t now_unix, const PrinterStateMedia& media = {});
std::string printer_states_json(std::span<const UnifiedPrinterView> printers,
                               std::uint64_t now_ms, std::int64_t now_unix);
std::string printer_display_address(const UnifiedPrinterView& printer);
bool printer_light_available(const UnifiedPrinterView& printer);

std::string unified_api_print_event_json(const PrintEventHistory& history, const PrintEvent& event);
std::string unified_api_printers_json(std::span<const UnifiedPrinterView> printers);
std::string unified_api_statuses_json(std::span<const UnifiedPrinterView> printers);
std::string unified_api_snapshot_json(std::span<const UnifiedPrinterView> printers,
                                      const UnifiedDevicePower& power = {});
std::string unified_api_printer_json(const UnifiedPrinterView& printer);
std::string unified_api_status_json(const UnifiedPrinterView& printer);
std::string unified_api_nozzles_json(const UnifiedPrinterView& printer);
std::string unified_api_materials_json(const UnifiedPrinterView& printer);

}  // namespace printdeck::core
