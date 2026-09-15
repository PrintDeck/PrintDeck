#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace printdeck::core {

struct UnifiedPrinterView;

struct MqttSensor {
  const char* key;
  const char* name;
  const char* path;
  const char* unit;
  const char* device_class;
  bool binary;
  bool full;
  bool event = false;
};

inline constexpr std::size_t kMqttDiscoveryMaxBytes = 2048;
std::span<const MqttSensor> mqtt_printer_sensors();
std::span<const MqttSensor> mqtt_device_sensors();

// profile_id=0 selects the device registry. The complete registry is independent
// of current telemetry so every previously advertised entity can be removed.
std::size_t mqtt_discovery_entity_count(std::uint32_t profile_id);
const MqttSensor* mqtt_discovery_sensor_at(std::uint32_t profile_id, std::size_t index);

// Accept only references returned by the registries above. Invalid inputs return
// an empty string. IDs allow 1..64 ASCII letters/digits/'_'/'-'; root must equal
// printdeck/<device_id>/v1. Profile names are limited to 96 UTF-8 bytes and
// manufacturer/model to 48 each. The returned config never exceeds 2048 bytes.
std::string mqtt_discovery_topic(std::string_view device_id, std::uint32_t profile_id,
                                 const MqttSensor& sensor);
std::string mqtt_discovery_json(std::string_view device_id, std::string_view root,
                                const UnifiedPrinterView* printer, const MqttSensor& sensor,
                                std::string_view language = "en");

}  // namespace printdeck::core
