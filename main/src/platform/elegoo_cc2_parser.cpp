#include "printdeck/platform/elegoo_cc2_parser.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include "cJSON.h"

namespace printdeck::platform {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const cJSON* field(const cJSON* object, const char* name) {
  return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, name) : nullptr;
}
bool ascii_id(std::string_view value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum &&
      std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
      });
}
bool text_valid(std::string_view value) {
  for (std::size_t i = 0; i < value.size();) {
    const unsigned char first = value[i++];
    if (first < 0x20 || first == 0x7f) return false;
    if (first < 0x80) continue;
    int trailing = first >= 0xc2 && first <= 0xdf ? 1 :
                   first >= 0xe0 && first <= 0xef ? 2 :
                   first >= 0xf0 && first <= 0xf4 ? 3 : -1;
    if (trailing < 0 || i + trailing > value.size()) return false;
    std::uint32_t code = first & (0x7fU >> (trailing + 1));
    const int width = trailing;
    while (trailing--) {
      const unsigned char next = value[i++];
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    if ((width == 2 && code < 0x800) || (width == 3 && code < 0x10000) ||
        code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}
bool inspect_tree(const cJSON* node, unsigned& nodes) {
  if (!node || ++nodes > 768) return false;
  if (cJSON_IsString(node) && (!node->valuestring || !text_valid(node->valuestring))) return false;
  if (cJSON_IsNumber(node) && !std::isfinite(node->valuedouble)) return false;
  unsigned siblings = 0;
  for (const cJSON* child = node->child; child; child = child->next) {
    if (++siblings > 128) return false;
    if (cJSON_IsObject(node)) {
      if (!child->string || !text_valid(child->string)) return false;
      for (const cJSON* before = node->child; before != child; before = before->next)
        if (std::strcmp(before->string, child->string) == 0) return false;
    }
    if (!inspect_tree(child, nodes)) return false;
  }
  return true;
}
Json parse(std::string_view body, std::size_t maximum = kElegooCc2MaximumMessageBytes) {
  Json none(nullptr, cJSON_Delete);
  if (body.empty() || body.size() > maximum || body.find('\0') != body.npos) return none;
  unsigned depth = 0;
  bool quoted = false;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const unsigned char ch = body[i];
    if (quoted) {
      if (ch < 0x20) return none;
      if (ch == '\\') {
        if (body.substr(i, 6) == "\\u0000" || ++i == body.size()) return none;
      } else if (ch == '"') quoted = false;
    } else if (ch == '"') quoted = true;
    else if (ch == '{' || ch == '[') { if (++depth > 12) return none; }
    else if (ch == '}' || ch == ']') { if (!depth) return none; --depth; }
    else if (ch == '-' || (ch >= '0' && ch <= '9')) {
      auto end = i;
      if (body[end] == '-' && ++end == body.size()) return none;
      if (body[end] == '0') ++end;
      else {
        if (body[end] < '1' || body[end] > '9') return none;
        while (end < body.size() && body[end] >= '0' && body[end] <= '9') ++end;
      }
      if (end < body.size() && body[end] == '.') {
        const auto start = ++end;
        while (end < body.size() && body[end] >= '0' && body[end] <= '9') ++end;
        if (start == end) return none;
      }
      if (end < body.size() && (body[end] == 'e' || body[end] == 'E')) {
        ++end;
        if (end < body.size() && (body[end] == '+' || body[end] == '-')) ++end;
        const auto start = end;
        while (end < body.size() && body[end] >= '0' && body[end] <= '9') ++end;
        if (start == end) return none;
      }
      if (end < body.size() && std::string_view(",]} \t\r\n").find(body[end]) == std::string_view::npos) return none;
      i = end - 1;
    }
  }
  if (quoted || depth) return none;
  const char* end = nullptr;
  Json document(cJSON_ParseWithLengthOpts(body.data(), body.size(), &end, false), cJSON_Delete);
  if (!document || !cJSON_IsObject(document.get())) return none;
  while (end < body.data() + body.size() && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
  unsigned nodes = 0;
  if (end != body.data() + body.size() || !inspect_tree(document.get(), nodes)) return none;
  return document;
}
bool number(const cJSON* value, double minimum, double maximum, double& output) {
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < minimum || value->valuedouble > maximum) return false;
  output = value->valuedouble;
  return true;
}
bool integer(const cJSON* value, std::uint32_t& result) {
  double n = 0;
  if (!number(value, 0, std::numeric_limits<std::uint32_t>::max(), n) || std::floor(n) != n) return false;
  result = static_cast<std::uint32_t>(n);
  return true;
}
bool boolean(const cJSON* value, bool& result) {
  if (cJSON_IsBool(value)) { result = cJSON_IsTrue(value); return true; }
  std::uint32_t n = 0;
  if (!integer(value, n) || n > 1) return false;
  result = n != 0;
  return true;
}
std::string text(const cJSON* value, std::size_t maximum) {
  if (!cJSON_IsString(value) || !value->valuestring) return {};
  const std::string_view value_text(value->valuestring);
  return value_text.size() <= maximum && text_valid(value_text) ? std::string(value_text) : std::string{};
}
std::string model(const cJSON* value) {
  std::string name = text(value, 48);
  if (name.rfind("ELEGOO ", 0) == 0) name.erase(0, 7);
  if (name == "Centauri Carbon 2" || name == "Centauri Carbon 2 Combo") return name;
  return {};
}
bool terminal(core::JobPhase phase) {
  return phase == core::JobPhase::completed || phase == core::JobPhase::cancelled || phase == core::JobPhase::failed;
}
bool active(core::JobPhase phase) {
  return phase == core::JobPhase::printing || phase == core::JobPhase::preparing || phase == core::JobPhase::paused;
}
void machine(core::JobState& job, int state, int substate) {
  using P = core::JobPhase;
  using A = core::PrinterActivity;
  using C = core::PrinterCondition;
  job.phase = P::unknown;
  job.activity = A::unknown;
  job.condition = C::normal;
  job.kind = core::JobKind::print;
  if (state == 1) { job.phase = P::idle; job.activity = A::standby; return; }
  if (state == 14 || state == 15) {
    job.phase = P::paused; job.activity = A::paused;
    job.condition = state == 14 ? C::error : C::attention;
    return;
  }
  if (state == 2) {
    job.phase = P::preparing; job.activity = A::preparing;
    switch (substate) {
      case 2075: case 2402: job.phase = P::printing; job.activity = A::printing; break;
      case 2077: job.phase = P::completed; job.activity = A::completed; break;
      case 2504: job.phase = P::cancelled; job.activity = A::cancelled; break;
      case 2501: case 2502: case 2505: case 2503:
        job.phase = P::paused; job.activity = A::paused; break;
      case 1045: case 1096: job.activity = A::nozzle_heating; break;
      case 1405: case 1906: job.activity = A::bed_heating; break;
      case 2801: case 2802: job.activity = A::homing; break;
      case 2901: case 2902: job.activity = A::bed_leveling; break;
      case 2401: break;
      default: job.condition = C::busy; break;
    }
    return;
  }
  job.condition = state < 0 ? C::unknown : C::busy;
  if (state == 0 || (state >= 3 && state <= 13)) {
    job.phase = P::idle;
    if (state == 3 || state == 4) {
      if (substate >= 1133 && substate <= 1135) job.activity = A::filament_loading;
      else if (substate == 1144) job.activity = A::filament_unloading;
    } else if (state == 5) {
      job.kind = core::JobKind::calibration; job.activity = A::bed_leveling;
    } else if (state >= 6 && state <= 8) {
      job.kind = core::JobKind::calibration; job.activity = A::calibrating;
    } else if (state == 10) {
      job.activity = A::homing;
    } else if (state == 13) {
      if (substate == 1061) job.activity = A::filament_loading;
      else if (substate == 1062) job.activity = A::filament_unloading;
    }
  }
}
void sample(const cJSON* object, const char* key, float& value, bool& known,
            double low, double high, bool full) {
  const cJSON* item = field(object, key);
  if (!item && !full) return;
  double parsed = 0;
  known = number(item, low, high, parsed);
  value = known ? static_cast<float>(parsed) : 0;
}
void seconds(const cJSON* object, const char* key, std::uint32_t& value, bool& known, bool full) {
  const cJSON* item = field(object, key);
  if (!item && !full) return;
  double parsed = 0;
  known = number(item, 0, std::numeric_limits<std::uint32_t>::max(), parsed);
  value = known ? static_cast<std::uint32_t>(parsed) : 0;
}
void clear_job(core::JobState& job) {
  job.name.clear(); job.gcode_file.clear(); job.preview.reset(); job.preview_hint.clear();
  job.completion = 0; job.completion_known = false;
  job.elapsed_seconds = 0; job.remaining_seconds = 0;
  job.elapsed_known = false; job.remaining_known = false;
  job.current_layer = 0; job.total_layers = 0;
}
}  // namespace

bool elegoo_cc2_serial_valid(std::string_view serial) { return ascii_id(serial, 32); }

std::optional<std::string> elegoo_cc2_host(std::string_view endpoint, bool allow_loopback) {
  if (endpoint.empty() || endpoint.size() > 128 || endpoint.find_first_of("/@?#[]\\ \t\r\n") != endpoint.npos) return {};
  const auto colon = endpoint.find(':');
  if (colon != endpoint.npos) {
    if (endpoint.substr(colon) != ":1883") return {};
    endpoint = endpoint.substr(0, colon);
  }
  if (endpoint.empty()) return {};
  unsigned octets[4]{};
  std::size_t cursor = 0;
  bool ipv4 = true;
  for (int i = 0; i < 4; ++i) {
    auto end = endpoint.find('.', cursor);
    if (i == 3) end = endpoint.size();
    if (end == endpoint.npos || end <= cursor || end - cursor > 3) { ipv4 = false; break; }
    const auto part = endpoint.substr(cursor, end - cursor);
    if (part.size() > 1 && part.front() == '0') { ipv4 = false; break; }
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), octets[i]);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || octets[i] > 255) { ipv4 = false; break; }
    cursor = end + 1;
  }
  if (ipv4) {
    const bool local = octets[0] == 10 || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
        (octets[0] == 192 && octets[1] == 168) || (octets[0] == 169 && octets[1] == 254) ||
        (allow_loopback && octets[0] == 127);
    return local ? std::optional<std::string>(endpoint) : std::nullopt;
  }
  // Reject numeric and legacy hexadecimal/octal address spellings before DNS.
  if (std::all_of(endpoint.begin(), endpoint.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) return {};
  if (endpoint == "localhost" || endpoint.rfind("0x", 0) == 0) return {};
  if (endpoint.find('.') != endpoint.npos && !endpoint.ends_with(".local")) return {};
  std::size_t label = 0;
  char previous = 0;
  for (const char ch : endpoint) {
    if (ch == '.') { if (!label || label > 63 || previous == '-') return {}; label = 0; }
    else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-') {
      if (!label && ch == '-') return {};
      ++label;
    } else return {};
    previous = ch;
  }
  if (!label || label > 63 || previous == '-') return {};
  return std::string(endpoint);
}
bool elegoo_cc2_endpoint_valid(std::string_view endpoint) { return elegoo_cc2_host(endpoint).has_value(); }

std::optional<ElegooCc2Discovery> parse_elegoo_cc2_discovery(std::string_view body) {
  auto doc = parse(body, 4096);
  std::uint32_t id = 1;
  if (!doc || !integer(field(doc.get(), "id"), id) || id != 0) return {};
  const cJSON* result = field(doc.get(), "result");
  ElegooCc2Discovery value;
  value.identity.model = model(field(result, "machine_model"));
  value.identity.serial = text(field(result, "sn"), 32);
  if (value.identity.model.empty() || !elegoo_cc2_serial_valid(value.identity.serial) ||
      !boolean(field(result, "lan_status"), value.lan_mode) ||
      !boolean(field(result, "token_status"), value.access_code_required)) return {};
  value.identity.firmware = text(field(field(result, "software_version"), "ota_version"), 48);
  return value;
}
bool elegoo_cc2_registration_accepted(std::string_view body, std::string_view client_id, bool& capacity_error) {
  capacity_error = false;
  auto doc = parse(body, 2048);
  if (!doc || text(field(doc.get(), "client_id"), 48) != client_id) return false;
  const std::string error = text(field(doc.get(), "error"), 128);
  capacity_error = error.find("too many clients") != std::string::npos;
  return error == "ok";
}
bool elegoo_cc2_pong(std::string_view body) {
  auto doc = parse(body, 256);
  return doc && text(field(doc.get(), "type"), 8) == "PONG";
}

void ElegooCc2Reducer::configure(std::uint32_t id, const ElegooIdentity& identity) {
  *this = ElegooCc2Reducer{};
  identity_ = identity;
  current_.profile_id = id;
  current_.link = core::LinkState::connecting;
}

ElegooCc2Update ElegooCc2Reducer::apply(std::string_view body, std::uint64_t now, std::uint32_t expected) {
  auto document = parse(body);
  if (!document) return ElegooCc2Update::invalid;
  const cJSON* root = document.get();
  std::uint32_t method = 0, id = 0;
  if (!integer(field(root, "method"), method) || !integer(field(root, "id"), id)) return ElegooCc2Update::invalid;
  if (method != 1001 && method != 1002 && method != 6000 && method != 6008) return ElegooCc2Update::ignored;
  if ((method < 6000 && (!expected || expected != id)) || (method >= 6000 && expected)) return ElegooCc2Update::ignored;
  const cJSON* result = field(root, "result");
  if (!cJSON_IsObject(result) || field(root, "error")) return ElegooCc2Update::invalid;
  if (const auto sn = field(result, "sn"); sn && text(sn, 32) != identity_.serial) return ElegooCc2Update::identity_mismatch;
  if (method == 1001 || method == 6008) {
    if (const auto type = field(result, "machine_model"); type) {
      const auto parsed = model(type);
      if (parsed.empty() || parsed != identity_.model) return ElegooCc2Update::identity_mismatch;
    }
    const auto version = text(field(field(result, "software_version"), "ota_version"), 48);
    if (!version.empty()) identity_.firmware = version;
    return ElegooCc2Update::attributes;
  }
  const bool full = method == 1002;
  const bool resetting_sequence = needs_baseline_;
  if (!full) {
    if (!baseline_ready_ || needs_baseline_) return ElegooCc2Update::need_baseline;
    if (have_sequence_) {
      if (id == last_sequence_) return ElegooCc2Update::ignored;
      if (id < last_sequence_ || id != last_sequence_ + 1) {
        needs_baseline_ = true;
        return ElegooCc2Update::need_baseline;
      }
    }
  }
  bool recognized = false;
  for (const char* key : {"machine_status", "print_status", "extruder", "heater_bed", "ztemperature_sensor", "fans", "gcode_move_inf"})
    recognized = recognized || field(result, key);
  if (!recognized && !full) {
    have_sequence_ = true; last_sequence_ = id;
    return ElegooCc2Update::ignored;
  }
  const cJSON* machine_object = field(result, "machine_status");
  std::uint32_t next_state = 0;
  if (full && (!cJSON_IsObject(machine_object) || !integer(field(machine_object, "status"), next_state))) return ElegooCc2Update::invalid;
  core::PrinterSnapshot next = current_;
  const auto previous_phase = current_.job.phase;
  int next_machine = machine_state_, next_substate = substate_;
  if (full) { next.job = {}; next_machine = -1; next_substate = -1; }
  if (machine_object) {
    if (!cJSON_IsObject(machine_object)) return ElegooCc2Update::invalid;
    if (const cJSON* state = field(machine_object, "status"); state) {
      std::uint32_t n = 0;
      if (!integer(state, n) || n > 65535) return ElegooCc2Update::invalid;
      next_machine = static_cast<int>(n);
      if (next_machine != machine_state_) next_substate = -1;
    }
    if (const cJSON* sub = field(machine_object, "sub_status"); sub) {
      std::uint32_t n = 0;
      if (!integer(sub, n) || n > 65535) return ElegooCc2Update::invalid;
      next_substate = static_cast<int>(n);
    }
  }
  machine(next.job, next_machine, next_substate);
  bool next_exception = full ? false : has_exception_;
  if (const auto exceptions = field(machine_object, "exception_status"); exceptions) {
    if (!cJSON_IsArray(exceptions) && !cJSON_IsNull(exceptions)) return ElegooCc2Update::invalid;
    next_exception = cJSON_IsArray(exceptions) && cJSON_GetArraySize(exceptions) > 0;
  }
  if (next_exception && next.job.condition != core::PrinterCondition::error) next.job.condition = core::PrinterCondition::attention;
  const cJSON* print = field(result, "print_status");
  std::string next_task = task_id_;
  if (const auto task = field(print, "task_id"); task) {
    if (cJSON_IsString(task)) next_task = text(task, 96);
    else { std::uint32_t n = 0; next_task = integer(task, n) ? std::to_string(n) : std::string{}; }
  }
  double elapsed = 0;
  const auto next_name = text(field(print, "filename"), 256);
  const bool changed_file = !next_name.empty() && !current_.job.name.empty() && next_name != current_.job.name;
  const bool restarted_elapsed = number(field(print, "print_duration"), 0,
      std::numeric_limits<std::uint32_t>::max(), elapsed) && current_.job.elapsed_known &&
      elapsed + 2 < current_.job.elapsed_seconds;
  const bool new_job = (active(next.job.phase) && !active(previous_phase)) ||
      (active(next.job.phase) && (changed_file || restarted_elapsed)) ||
      (!next_task.empty() && !task_id_.empty() && next_task != task_id_);
  if (new_job || next.job.phase == core::JobPhase::idle) clear_job(next.job);
  if (print && !cJSON_IsObject(print)) { clear_job(next.job); next_task.clear(); }
  if (new_job) { terminal_.reset(); terminal_until_ms_ = 0; }
  if (machine_object || full) {
    sample(machine_object, "progress", next.job.completion, next.job.completion_known, 0, 100, full || new_job);
    field_times_[0] = now;
  }
  if (next_machine == 2 && (next_substate == 1081 || next_substate == 1082 || next_substate == 1086)) next.job.completion_known = false;
  if (active(next.job.phase) || terminal(next.job.phase)) {
    if (print || full) {
      if (const auto name = field(print, "filename"); name || full || new_job) {
        next.job.name = text(name, 256);
        next.job.gcode_file = next.job.name;
      }
      seconds(print, "print_duration", next.job.elapsed_seconds, next.job.elapsed_known, full || !cJSON_IsObject(print));
      seconds(print, "remaining_time_sec", next.job.remaining_seconds, next.job.remaining_known, full || !cJSON_IsObject(print));
      for (const auto& mapping : {std::pair{"current_layer", &next.job.current_layer}, std::pair{"total_layer", &next.job.total_layers}}) {
        const cJSON* item = field(print, mapping.first);
        if (item || full) { std::uint32_t n = 0; *mapping.second = integer(item, n) && n <= 65535 ? static_cast<std::uint16_t>(n) : 0; }
      }
      field_times_[1] = now;
    }
    if (next.job.phase == core::JobPhase::preparing && !next.job.remaining_seconds) next.job.remaining_known = false;
  }
  auto& temperature = next.job.temperatures;
  const auto extruder = field(result, "extruder");
  if (extruder || full) {
    const bool replace = full || !cJSON_IsObject(extruder);
    sample(extruder, "temperature", temperature.nozzle_c, temperature.nozzle_known, -50, 500, replace);
    sample(extruder, "target", temperature.nozzle_target_c, temperature.nozzle_target_known, 0, 500, replace);
    field_times_[2] = now;
  }
  const auto bed = field(result, "heater_bed");
  if (bed || full) {
    const bool replace = full || !cJSON_IsObject(bed);
    sample(bed, "temperature", temperature.bed_c, temperature.bed_known, -50, 200, replace);
    sample(bed, "target", temperature.bed_target_c, temperature.bed_target_known, 0, 200, replace);
    field_times_[3] = now;
  }
  const auto chamber = field(result, "ztemperature_sensor");
  if (chamber || full) {
    sample(chamber, "temperature", temperature.chamber_c, temperature.chamber_known, -50, 200, full || !cJSON_IsObject(chamber));
    field_times_[4] = now;
  }
  const auto fans = field(result, "fans");
  if (fans || full) {
    const auto fan = field(fans, "fan");
    if (fan || full || !cJSON_IsObject(fans))
      sample(fan, "speed", next.job.motion.fan_percent, next.job.motion.fan_percent_known, 0, 100, full || !cJSON_IsObject(fan));
    field_times_[5] = now;
  }
  if (const auto motion = field(result, "gcode_move_inf"); motion) {
    const bool replace = !cJSON_IsObject(motion);
    sample(motion, "x", next.job.motion.x_mm, next.job.motion.x_known, -10000, 10000, replace);
    sample(motion, "y", next.job.motion.y_mm, next.job.motion.y_known, -10000, 10000, replace);
    sample(motion, "z", next.job.motion.z_mm, next.job.motion.z_known, -10000, 10000, replace);
    next.job.motion.position_known = next.job.motion.x_known && next.job.motion.y_known && next.job.motion.z_known;
    field_times_[6] = now;
  }
  next.job.toolhead_count = 1;
  next.job.active_toolhead = 0;
  auto& tool = next.job.toolheads[0];
  tool.present = true; tool.active = true;
  tool.temperature_known = temperature.nozzle_known;
  tool.target_known = temperature.nozzle_target_known;
  tool.temperature_c = temperature.nozzle_c;
  tool.target_c = temperature.nozzle_target_c;
  next.job.reachable = true;
  const auto activity = next.job.activity;
  const auto kind = next.job.kind;
  next.job.normalize();
  // Maintenance can be idle from a print-job perspective while still having
  // a concrete device activity. It must not manufacture a completed print.
  if (next.job.phase == core::JobPhase::idle) {
    next.job.activity = activity; next.job.kind = kind;
  }
  next.link = core::LinkState::online;
  next.updated_at_ms = now;
  if (terminal(next.job.phase) && next.job.phase != previous_phase) {
    terminal_ = next;
    terminal_until_ms_ = now + kElegooCc2TerminalHoldMs;
  }
  current_ = std::move(next);
  task_id_ = std::move(next_task);
  machine_state_ = next_machine; substate_ = next_substate;
  has_exception_ = next_exception;
  last_status_ms_ = now;
  baseline_ready_ = true; needs_baseline_ = false;
  if (full) { if (resetting_sequence) have_sequence_ = false; }
  else { have_sequence_ = true; last_sequence_ = id; }
  return ElegooCc2Update::status;
}

core::PrinterSnapshot ElegooCc2Reducer::snapshot(std::uint64_t now) const {
  core::PrinterSnapshot result;
  snapshot_into(result, now);
  return result;
}
void ElegooCc2Reducer::snapshot_into(core::PrinterSnapshot& result, std::uint64_t now) const {
  result = terminal_ && now < terminal_until_ms_ && current_.job.phase == core::JobPhase::idle
      ? *terminal_ : current_;
  const auto stale = [now](std::uint64_t then) { return now < then || now - then > kElegooCc2FreshnessMs; };
  if (!baseline_ready_ || stale(last_status_ms_)) {
    result.link = baseline_ready_ ? core::LinkState::failed : core::LinkState::connecting;
    result.job.reachable = false;
  }
  if (stale(field_times_[0])) result.job.completion_known = false;
  if (stale(field_times_[1])) { result.job.elapsed_known = false; result.job.remaining_known = false; }
  if (stale(field_times_[2])) {
    result.job.temperatures.nozzle_known = false;
    result.job.temperatures.nozzle_target_known = false;
    result.job.toolheads[0].temperature_known = false;
    result.job.toolheads[0].target_known = false;
  }
  if (stale(field_times_[3])) { result.job.temperatures.bed_known = false; result.job.temperatures.bed_target_known = false; }
  if (stale(field_times_[4])) result.job.temperatures.chamber_known = false;
  if (stale(field_times_[5])) result.job.motion.fan_percent_known = false;
  if (stale(field_times_[6])) {
    result.job.motion.x_known = result.job.motion.y_known = result.job.motion.z_known = false;
    result.job.motion.position_known = false;
  }
}

}  // namespace printdeck::platform
