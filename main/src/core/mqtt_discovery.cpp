#include "printdeck/core/mqtt_discovery.hpp"
#include "printdeck/core/unified_printer_api.hpp"

#include <array>
#include <initializer_list>
#include <utility>

namespace printdeck::core {
namespace {

constexpr MqttSensor kPrinterSensors[] = {
    {"phase", "Job state", "status.job.phase", "", "enum", false, false},
    {"activity", "Activity", "status.job.activity", "", "enum", false, true},
    {"progress", "Progress", "status.job.progress_percent", "%", "", false, false},
    {"job_name", "Job name", "status.job.name", "", "", false, false},
    {"job_kind", "Job type", "status.job.kind", "", "enum", false, false},
    {"remaining_time", "Remaining time", "status.job.remaining_seconds", "s", "duration", false, false},
    {"elapsed_time", "Elapsed time", "status.job.elapsed_seconds", "s", "duration", false, false},
    {"current_layer", "Current layer", "status.job.current_layer", "", "", false, true},
    {"total_layers", "Total layers", "status.job.total_layers", "", "", false, true},
    {"nozzle_temperature", "Nozzle temperature", "status.temperatures.nozzle_current_c", "°C", "temperature", false, true},
    {"nozzle_target_temperature", "Nozzle target", "status.temperatures.nozzle_target_c", "°C", "temperature", false, true},
    {"bed_temperature", "Bed temperature", "status.temperatures.bed_current_c", "°C", "temperature", false, true},
    {"bed_target_temperature", "Bed target", "status.temperatures.bed_target_c", "°C", "temperature", false, true},
    {"chamber_temperature", "Chamber temperature", "status.temperatures.chamber_current_c", "°C", "temperature", false, true},
    {"network_address", "Network address", "printer.network.address", "", "", false, false},
    {"network_port", "Network port", "printer.network.port", "", "", false, false},
    {"connection_state", "Connection state", "status.connection.state", "", "enum", false, false},
    {"reachability", "Reachability", "status.connection.reachability", "", "enum", false, false},
    {"detail_level", "Detail level", "status.connection.detail_level", "", "enum", false, false},
    {"online", "Online", "status.connection.reachability", "", "connectivity", true, false},
    {"selected", "Selected", "printer.selected", "", "", true, false},
    {"data_stale", "Data stale", "status.connection.stale", "", "problem", true, false},
    {"condition", "Printer condition", "status.job.condition", "", "enum", false, false},
    {"print_events", "Print events", "event.event_type", "", "", false, false, true},
};
constexpr MqttSensor kDeviceSensors[] = {
    {"battery_level", "Battery", "device.power.battery_percent", "%", "battery", false, false},
    {"battery_charging", "Battery charging", "device.power.charging", "", "battery_charging", true, false},
    {"external_power", "External power", "device.power.external_power", "", "power", true, false},
};

// BEGIN MQTT ENTITY NAMES
constexpr const char* kNames[][6] = {
    {"Print phase", "Faza wydruku", "Fase de impresión", "Phase d’impression", "Druckphase", "打印阶段"},
    {"Printer activity", "Aktywność drukarki", "Actividad de la impresora", "Activité de l’imprimante", "Druckeraktivität", "打印机活动"},
    {"Print progress", "Postęp wydruku", "Progreso de impresión", "Progression de l’impression", "Druckfortschritt", "打印进度"},
    {"Print name", "Nazwa wydruku", "Nombre de impresión", "Nom de l’impression", "Druckname", "打印名称"},
    {"Job type", "Typ zadania", "Tipo de trabajo", "Type de tâche", "Auftragstyp", "任务类型"},
    {"Remaining time", "Pozostały czas", "Tiempo restante", "Temps restant", "Restzeit", "剩余时间"},
    {"Elapsed time", "Czas od rozpoczęcia", "Tiempo transcurrido", "Temps écoulé", "Verstrichene Zeit", "已用时间"},
    {"Current layer", "Bieżąca warstwa", "Capa actual", "Couche actuelle", "Aktuelle Schicht", "当前层"},
    {"Total layers", "Liczba warstw", "Capas totales", "Nombre total de couches", "Gesamtschichten", "总层数"},
    {"Nozzle temperature", "Temperatura dyszy", "Temperatura de la boquilla", "Température de la buse", "Düsentemperatur", "喷嘴温度"},
    {"Nozzle target temperature", "Docelowa temperatura dyszy", "Temperatura objetivo de la boquilla", "Température cible de la buse", "Düsen-Solltemperatur", "喷嘴目标温度"},
    {"Bed temperature", "Temperatura stołu", "Temperatura de la cama", "Température du plateau", "Druckbetttemperatur", "热床温度"},
    {"Bed target temperature", "Docelowa temperatura stołu", "Temperatura objetivo de la cama", "Température cible du plateau", "Druckbett-Solltemperatur", "热床目标温度"},
    {"Chamber temperature", "Temperatura komory", "Temperatura de la cámara", "Température de la chambre", "Kammertemperatur", "腔体温度"},
    {"Network address", "Adres sieciowy", "Dirección de red", "Adresse réseau", "Netzwerkadresse", "网络地址"},
    {"Network port", "Port sieciowy", "Puerto de red", "Port réseau", "Netzwerkport", "网络端口"},
    {"Connection state", "Stan połączenia", "Estado de conexión", "État de la connexion", "Verbindungsstatus", "连接状态"},
    {"Reachability", "Dostępność", "Disponibilidad", "Accessibilité", "Erreichbarkeit", "可达性"},
    {"Data detail", "Szczegółowość danych", "Detalle de datos", "Niveau de détail", "Datendetails", "数据详情"},
    {"Connection", "Połączenie", "Conexión", "Connexion", "Verbindung", "连接"},
    {"Selected printer", "Wybrana drukarka", "Impresora seleccionada", "Imprimante sélectionnée", "Ausgewählter Drucker", "已选打印机"},
    {"Stale data", "Nieaktualne dane", "Datos obsoletos", "Données obsolètes", "Veraltete Daten", "过期数据"},
    {"Printer condition", "Stan drukarki", "Condición de la impresora", "État de l’imprimante", "Druckerzustand", "打印机状况"},
    {"Print events", "Zdarzenia wydruku", "Eventos de impresión", "Événements d’impression", "Druckereignisse", "打印事件"},
    {"Battery level", "Poziom baterii", "Nivel de batería", "Niveau de batterie", "Batteriestand", "电池电量"},
    {"Battery charging", "Ładowanie baterii", "Batería cargando", "Batterie en charge", "Batterie wird geladen", "电池充电"},
    {"External power", "Zasilanie zewnętrzne", "Alimentación externa", "Alimentation externe", "Externe Stromversorgung", "外部供电"},
};
// END MQTT ENTITY NAMES
static_assert(std::size(kNames) == std::size(kPrinterSensors) + std::size(kDeviceSensors));

constexpr std::string_view kPhaseOptions = R"(["unknown","idle","preparing","printing","paused","completed","failed","cancelled"])";
constexpr std::string_view kActivityOptions = R"(["unknown","standby","preparing","nozzle_heating","bed_heating","homing","bed_leveling","nozzle_cleaning","calibrating","filament_changing","filament_unloading","filament_loading","filament_purging","printing","paused","completed","failed","cancelled"])";

std::string_view options(std::size_t index) {
  switch (index) {
    case 0: return kPhaseOptions;
    case 1: return kActivityOptions;
    case 4: return R"(["print","calibration"])";
    case 16: return R"(["unknown","stopped","waiting_for_network","connecting","online","offline"])";
    case 17: return R"(["unknown","online","offline"])";
    case 18: return R"(["summary","full"])";
    case 22: return R"(["unknown","normal","ready","busy","attention","error"])";
    default: return {};
  }
}

// One bounded result allocation; no JSON tree or profile/telemetry copy.
class Json {
 public:
  Json() { text_.reserve(kMqttDiscoveryMaxBytes); }
  void append(std::string_view value) {
    if (failed_ || value.size() > kMqttDiscoveryMaxBytes - text_.size()) {
      failed_ = true;
      return;
    }
    text_.append(value);
  }
  void quote(std::initializer_list<std::string_view> values) {
    append("\"");
    constexpr char hex[] = "0123456789abcdef";
    for (auto value : values) {
      for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
          const char escaped[] = {'\\', static_cast<char>(ch)};
          append({escaped, 2});
        } else if (ch < 0x20) {
          const char escaped[] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 15]};
          append({escaped, 6});
        } else {
          const char byte = static_cast<char>(ch);
          append({&byte, 1});
        }
      }
    }
    append("\"");
  }
  void property(std::string_view key, std::string_view value) {
    append(","); quote({key}); append(":"); quote({value});
  }
  std::string finish() { return failed_ ? std::string{} : std::move(text_); }
 private:
  std::string text_;
  bool failed_ = false;
};

bool valid_id(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (unsigned char ch : id) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
  }
  return true;
}

bool valid_utf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80) continue;
    unsigned remaining;
    std::uint32_t point;
    std::uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) { remaining = 1; point = first & 0x1f; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { remaining = 2; point = first & 0x0f; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; point = first & 7; minimum = 0x10000; }
    else return false;
    if (remaining > text.size() - i) return false;
    while (remaining--) {
      const auto next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xc0) != 0x80) return false;
      point = (point << 6) | (next & 0x3f);
    }
    if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
  }
  return true;
}

std::size_t sensor_index(const MqttSensor& sensor, bool device) {
  const auto registry = device ? mqtt_device_sensors() : mqtt_printer_sensors();
  for (std::size_t i = 0; i < registry.size(); ++i) {
    if (&sensor == &registry[i]) return device ? i + std::size(kPrinterSensors) : i;
  }
  return std::size(kPrinterSensors) + std::size(kDeviceSensors);
}

std::size_t language_index(std::string_view language) {
  if (language == "pl") return 1;
  if (language == "es") return 2;
  if (language == "fr") return 3;
  if (language == "de") return 4;
  if (language == "zh-CN" || language == "zh-Hans") return 5;
  return 0;
}

// Path segments come exclusively from the immutable registry, never a profile.
std::string template_prefix(const MqttSensor& sensor) {
  std::string result;
  result.reserve(384);
  std::string_view path(sensor.path);
  const auto first = path.find('.');
  const auto second = path.find('.', first + 1);
  result = "{% set s=value_json.get('";
  result.append(path.substr(0, first));
  result += "') if value_json is mapping else none %}";
  result += "{% set c=s.get('connection') if s is mapping else none %}";
  if (second != std::string_view::npos) {
    result += "{% set p=s.get('";
    result.append(path.substr(first + 1, second - first - 1));
    result += "') if s is mapping else none %}{% set v=p.get('";
    result.append(path.substr(second + 1));
    result += "') if p is mapping else none %}";
  } else {
    result += "{% set v=s.get('";
    result.append(path.substr(first + 1));
    result += "') if s is mapping else none %}";
  }
  return result;
}

std::string validity(std::size_t index) {
  if (const auto list = options(index); !list.empty()) return "v is string and v in " + std::string(list);
  if (index == 19) return "v is string and v in ['online','offline']";
  if (index == 20 || index == 21 || index == 25 || index == 26) return "v is boolean";
  if (index == 3 || index == 14) return "v is string and v|length > 0";
  std::string check = "v is number and v is not boolean and v == v";
  // Bounds also exclude JSON infinities. Temperatures permit cold environments.
  if (index >= 9 && index <= 13) return check + " and -273.15 <= v <= 3000";
  if (index == 2 || index == 24) return check + " and 0 <= v <= 100";
  if (index == 15) return check + " and 1 <= v <= 65535";
  return check + " and 0 <= v <= 4294967295";
}

}  // namespace

std::span<const MqttSensor> mqtt_printer_sensors() { return kPrinterSensors; }
std::span<const MqttSensor> mqtt_device_sensors() { return kDeviceSensors; }
std::size_t mqtt_discovery_entity_count(std::uint32_t profile_id) {
  return profile_id == 0 ? std::size(kDeviceSensors) : std::size(kPrinterSensors);
}
const MqttSensor* mqtt_discovery_sensor_at(std::uint32_t profile_id, std::size_t index) {
  const auto registry = profile_id == 0 ? mqtt_device_sensors() : mqtt_printer_sensors();
  return index < registry.size() ? &registry[index] : nullptr;
}

std::string mqtt_discovery_topic(std::string_view device_id, std::uint32_t profile_id,
                                 const MqttSensor& sensor) {
  if (!valid_id(device_id) || sensor_index(sensor, profile_id == 0) >= std::size(kNames)) return {};
  std::string result = sensor.event ? "homeassistant/event/" : sensor.binary ? "homeassistant/binary_sensor/" : "homeassistant/sensor/";
  result.append(device_id);
  if (profile_id != 0) { result += '_'; result += std::to_string(profile_id); }
  result += '_'; result += sensor.key; result += "/config";
  return result;
}

std::string mqtt_discovery_json(std::string_view device_id, std::string_view root,
                                const UnifiedPrinterView* printer, const MqttSensor& sensor,
                                std::string_view language) {
  const auto index = sensor_index(sensor, printer == nullptr);
  if (!valid_id(device_id) || index >= std::size(kNames) ||
      root.size() != 13 + device_id.size() || !root.starts_with("printdeck/") ||
      root.substr(10, device_id.size()) != device_id || !root.ends_with("/v1") ||
      (printer && (printer->id == 0 || printer->display_name.size() > 96 ||
                   printer->manufacturer.size() > 48 || printer->model.size() > 48 ||
                   !valid_utf8(printer->display_name) || !valid_utf8(printer->manufacturer) ||
                   !valid_utf8(printer->model)))) return {};

  const bool device = printer == nullptr;
  const bool metadata = index == 14 || index == 15 || index == 20;
  const auto id = device ? std::string{} : std::to_string(printer->id);
  std::string identity(device_id);
  if (!device) { identity += '_'; identity += id; }
  std::string topic(root);
  if (device) topic += "/device";
  else { topic += "/printers/"; topic += id; topic += sensor.event ? "/events" : metadata ? "/info" : "/status"; }
  const auto prefix = template_prefix(sensor);
  const auto valid = validity(index);

  Json json;
  json.append("{\"name\":"); json.quote({kNames[index][language_index(language)]});
  json.append(",\"unique_id\":"); json.quote({identity, "_", sensor.key});
  json.property("state_topic", topic);
  if (sensor.event) {
    json.append(",\"event_types\":[\"started\",\"paused\",\"resumed\",\"completed\",\"failed\",\"cancelled\",\"milestone\",\"attention\",\"attention_cleared\"]");
    json.property("availability_topic", std::string(root) + "/availability");
  } else {
    json.append(",\"qos\":0,\"expire_after\":90,\"availability_mode\":\"all\",\"availability\":[{\"topic\":");
    json.quote({root, "/availability"});
    json.append("},{\"topic\":"); json.quote({topic});
    json.append(",\"value_template\":");
    std::string gates = "value_json is mapping and value_json.get('api_version') == 'v1' and (" + valid + ")";
    if (!device && (index < 14 || index == 22)) {
      gates += " and c is mapping and c.get('stale') is sameas false";
      if (sensor.full) gates += " and c.get('detail_level') == 'full'";
    } else if (device) {
      gates += " and p is mapping and p.get('available') is sameas true";
      if (index != 26) gates += " and p.get('battery_present') is sameas true";
    }
    json.quote({prefix, "{{ 'online' if ", gates, " else 'offline' }}"});
    json.append("}],\"value_template\":");
    if (sensor.binary) {
      json.quote({prefix, "{{ ('ON' if ", index == 19 ? "v == 'online'" : "v is sameas true",
                  " else 'OFF') if (", valid, ") else none }}"});
      json.property("payload_on", "ON"); json.property("payload_off", "OFF");
    } else {
      json.quote({prefix, "{{ v if (", options(index).empty() ? std::string_view(valid) : "v is string",
                  ") else none }}"});
    }
    if (sensor.unit[0]) json.property("unit_of_measurement", sensor.unit);
    if (sensor.device_class[0]) json.property("device_class", sensor.device_class);
    if (const auto list = options(index); !list.empty()) {
      json.append(",\"options\":"); json.append(list);
    }
    if (index == 2 || (index >= 9 && index <= 13) || index == 24) json.property("state_class", "measurement");
    if ((index >= 14 && index <= 18) || index == 20 || index == 21) json.property("entity_category", "diagnostic");
  }
  json.append(",\"device\":{\"identifiers\":["); json.quote({identity}); json.append("],\"name\":");
  if (device || printer->display_name.empty()) json.quote({"PrintDeck", device ? "" : " ", id});
  else json.quote({printer->display_name});
  json.property("manufacturer", device || printer->manufacturer.empty() ? "PrintDeck" : printer->manufacturer);
  if (!device) {
    if (!printer->model.empty()) json.property("model", printer->model);
    json.property("via_device", device_id);
  }
  json.append("}}");
  return json.finish();
}

}  // namespace printdeck::core
