#include "printdeck/platform/elegoo_sdcp_parser.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <set>

#include "cJSON.h"

namespace printdeck::platform {
namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }
bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
std::string lowercase(std::string_view text) {
  std::string out(text);
  for (char& c : out) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return out;
}
bool cc1_model(std::string_view model) {
  const auto value = lowercase(model);
  return value == "centauri carbon" || value == "elegoo centauri carbon";
}
bool ipv4(std::string_view host, std::array<unsigned, 4>& bytes) {
  for (unsigned i = 0; i != bytes.size(); ++i) {
    const auto dot = host.find('.');
    const auto part = host.substr(0, dot);
    if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0')) return false;
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), bytes[i]);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || bytes[i] > 255) return false;
    if (i == 3) return dot == std::string_view::npos;
    if (dot == std::string_view::npos) return false;
    host.remove_prefix(dot + 1);
  }
  return false;
}
bool local_host(std::string_view host) {
  std::array<unsigned, 4> bytes{};
  if (ipv4(host, bytes)) {
    return bytes[0] == 10 || (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
        (bytes[0] == 192 && bytes[1] == 168) || (bytes[0] == 169 && bytes[1] == 254);
  }
  if (host.empty() || host.size() > 128) return false;
  const auto lowered = lowercase(host);
  if (lowered.starts_with("0x") && lowered.size() > 2 &&
      std::all_of(lowered.begin() + 2, lowered.end(), hex)) return false;
  if (host.find('.') != std::string_view::npos && !lowered.ends_with(".local")) return false;
  // Numeric aliases must never be interpreted as alternative IPv4 spellings by DNS.
  if (!std::any_of(host.begin(), host.end(), letter)) return false;
  unsigned count = 0;
  char previous = 0;
  for (const char c : host) {
    if (c == '.') {
      if (!count || previous == '-') return false;
      count = 0;
    } else {
      if ((!letter(c) && !digit(c) && c != '-') || (!count && c == '-') || ++count > 63) return false;
    }
    previous = c;
  }
  return count && previous != '-';
}

bool valid_utf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80) { if (!first) return false; continue; }
    unsigned count = 0, code = 0, minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 31; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 15; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 7; minimum = 0x10000; }
    else return false;
    if (i + count > text.size()) return false;
    while (count--) {
      const auto c = static_cast<unsigned char>(text[i++]);
      if ((c & 0xc0) != 0x80) return false;
      code = (code << 6) | (c & 63);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}

// Validate syntax before cJSON allocation: its permissive number parser alone is
// unsuitable at a network boundary. Limits also bound recursion and object count.
class JsonGate {
 public:
  explicit JsonGate(std::string_view value) : input_(value) {}
  bool valid() { return parse(0) && (space(), position_ == input_.size()); }
 private:
  void space() { while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' || input_[position_] == '\r' || input_[position_] == '\n')) ++position_; }
  bool take(char c) { if (position_ < input_.size() && input_[position_] == c) { ++position_; return true; } return false; }
  int unicode() {
    unsigned value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      if (position_ == input_.size() || !hex(input_[position_])) return -1;
      const char c = input_[position_++];
      value = value * 16 + (digit(c) ? c - '0' : (c | 32) - 'a' + 10);
    }
    return static_cast<int>(value);
  }
  bool string() {
    if (!take('"')) return false;
    while (position_ < input_.size()) {
      const unsigned char c = input_[position_++];
      if (c == '"') return true;
      if (c < 32) return false;
      if (c != '\\') continue;
      if (position_ == input_.size()) return false;
      const char escape = input_[position_++];
      if (escape == 'u') {
        const int value = unicode();
        if (value <= 0 || (value >= 0xdc00 && value <= 0xdfff)) return false;
        if (value >= 0xd800 && value <= 0xdbff) {
          if (!take('\\') || !take('u')) return false;
          const int low = unicode();
          if (low < 0xdc00 || low > 0xdfff) return false;
        }
      } else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos) return false;
    }
    return false;
  }
  bool parse(unsigned depth) {
    if (depth > 16 || ++nodes_ > 1024) return false;
    space();
    if (position_ == input_.size()) return false;
    if (input_[position_] == '"') return string();
    if (take('{')) {
      space(); if (take('}')) return true;
      do { space(); if (!string()) return false; space(); if (!take(':') || !parse(depth + 1)) return false; space(); if (take('}')) return true; } while (take(','));
      return false;
    }
    if (take('[')) {
      space(); if (take(']')) return true;
      do { if (!parse(depth + 1)) return false; space(); if (take(']')) return true; } while (take(','));
      return false;
    }
    for (const std::string_view literal : {"true", "false", "null"}) {
      if (input_.substr(position_, literal.size()) == literal) { position_ += literal.size(); return true; }
    }
    take('-');
    if (!take('0')) {
      if (position_ == input_.size() || input_[position_] < '1' || input_[position_] > '9') return false;
      while (position_ < input_.size() && digit(input_[position_])) ++position_;
    }
    if (take('.')) {
      const auto start = position_;
      while (position_ < input_.size() && digit(input_[position_])) ++position_;
      if (position_ == start) return false;
    }
    if (take('e') || take('E')) {
      if (!take('+')) take('-');
      const auto start = position_;
      while (position_ < input_.size() && digit(input_[position_])) ++position_;
      if (position_ == start) return false;
    }
    return true;
  }
  std::string_view input_;
  std::size_t position_ = 0;
  unsigned nodes_ = 0;
};

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
bool unique_keys(const cJSON* value) {
  std::set<std::string_view> keys;
  for (const auto* child = value->child; child; child = child->next) {
    if (cJSON_IsObject(value) && (!child->string || !keys.emplace(child->string).second)) return false;
    if ((cJSON_IsObject(child) || cJSON_IsArray(child)) && !unique_keys(child)) return false;
  }
  return true;
}
Json decode(std::string_view value) {
  if (value.empty() || value.size() > kElegooSdcpMaximumMessage || !valid_utf8(value)) return {nullptr, cJSON_Delete};
  // One observed firmware variant prepends the exact decimal JSON byte count.
  // Do not search for a brace through arbitrary data or accept a mismatching count.
  if (digit(value.front())) {
    const auto brace = value.find('{');
    if (brace == std::string_view::npos || !brace || brace > 5) return {nullptr, cJSON_Delete};
    std::size_t count = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + brace, count);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + brace || count != value.size() - brace) return {nullptr, cJSON_Delete};
    value.remove_prefix(brace);
  }
  if (!JsonGate(value).valid()) return {nullptr, cJSON_Delete};
  Json root(cJSON_ParseWithLength(value.data(), value.size()), cJSON_Delete);
  if (!root || !cJSON_IsObject(root.get()) || !unique_keys(root.get())) return {nullptr, cJSON_Delete};
  return root;
}
const cJSON* member(const cJSON* value, const char* name) { return cJSON_IsObject(value) ? cJSON_GetObjectItemCaseSensitive(value, name) : nullptr; }
std::optional<std::string> text(const cJSON* value, std::size_t maximum) {
  if (!cJSON_IsString(value) || !value->valuestring) return std::nullopt;
  std::string_view string(value->valuestring);
  if (string.size() > maximum || !valid_utf8(string)) return std::nullopt;
  for (const unsigned char c : string) if (c < 32 || c == 127) return std::nullopt;
  return std::string(string);
}
bool number(const cJSON* value, double minimum, double maximum, double& out) {
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < minimum || value->valuedouble > maximum) return false;
  out = value->valuedouble;
  return true;
}
bool integer(const cJSON* value, int minimum, int maximum, int& out) {
  double numeric = 0;
  if (!number(value, minimum, maximum, numeric) || std::floor(numeric) != numeric) return false;
  out = static_cast<int>(numeric);
  return true;
}
bool decode_identity(const cJSON* body, ElegooIdentity& identity) {
  auto model = text(member(body, "MachineName"), 80);
  auto serial = text(member(body, "MainboardID"), 32);
  if (!model || !cc1_model(*model) || !serial || !elegoo_sdcp_valid_mainboard_id(*serial)) return false;
  auto firmware = text(member(body, "FirmwareVersion"), 80);
  identity = {.manufacturer = "ELEGOO", .model = "Centauri Carbon", .serial = lowercase(*serial), .firmware = firmware.value_or("")};
  return true;
}
void scalar(const cJSON* value, float& out, bool& known, double maximum) {
  if (!value) return;
  double numeric = 0;
  known = number(value, -40, maximum, numeric);
  out = known ? static_cast<float>(numeric) : 0;
}
void temperature(const cJSON* status, const char* actual_name, const char* target_name,
                 float& actual, bool& actual_known, float& target, bool& target_known, double max) {
  const auto* value = member(status, actual_name);
  if (cJSON_IsArray(value)) {
    if (cJSON_GetArraySize(value) == 2) {
      scalar(cJSON_GetArrayItem(value, 0), target, target_known, max);
      scalar(cJSON_GetArrayItem(value, 1), actual, actual_known, max);
    } else { actual_known = target_known = false; actual = target = 0; }
  } else scalar(value, actual, actual_known, max);
  scalar(member(status, target_name), target, target_known, max);
}
void clear_job(core::JobState& job) {
  job.name.clear(); job.gcode_file.clear(); job.preview_hint.clear(); job.preview.reset();
  job.completion = 0; job.completion_known = false;
  job.elapsed_seconds = job.remaining_seconds = 0;
  job.elapsed_known = job.remaining_known = false;
  job.current_layer = job.total_layers = 0;
}
void metric(const cJSON* parent, const char* key, float& out, bool& known, double maximum, double divisor = 1) {
  const auto* value = member(parent, key);
  if (!value) return;
  double numeric = 0;
  known = number(value, 0, maximum, numeric);
  out = known ? static_cast<float>(numeric / divisor) : 0;
}

}  // namespace

std::optional<ElegooSdcpEndpoint> elegoo_sdcp_endpoint(std::string_view endpoint) {
  if (endpoint.empty() || endpoint.size() > 136 || endpoint.find_first_of("/@?#\\") != std::string_view::npos) return std::nullopt;
  ElegooSdcpEndpoint result;
  const auto colon = endpoint.find(':');
  if (colon != std::string_view::npos) {
    if (endpoint.find(':', colon + 1) != std::string_view::npos) return std::nullopt;
    unsigned port = 0;
    const auto value = endpoint.substr(colon + 1);
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !port || port > 65535) return std::nullopt;
    result.port = static_cast<std::uint16_t>(port);
    endpoint = endpoint.substr(0, colon);
  }
  if (!local_host(endpoint)) return std::nullopt;
  result.host = lowercase(endpoint);
  return result;
}

bool elegoo_sdcp_valid_mainboard_id(std::string_view value) {
  return (value.size() == 16 || value.size() == 32) && std::all_of(value.begin(), value.end(), hex);
}

bool elegoo_sdcp_decode_discovery(std::string_view packet, ElegooIdentity& identity, std::string* outer_id) {
  auto root = decode(packet);
  if (!root) return false;
  ElegooIdentity candidate;
  if (!decode_identity(member(root.get(), "Data"), candidate)) return false;
  const auto outer = text(member(root.get(), "Id"), 32);
  if (!outer || !elegoo_sdcp_valid_mainboard_id(*outer)) return false;
  identity = std::move(candidate);
  if (outer_id) *outer_id = *outer;
  return true;
}

std::optional<std::string> elegoo_sdcp_read_request(unsigned command, std::string_view mainboard_id,
    std::uint64_t request_id, std::uint64_t timestamp_ms, std::string_view outer_id) {
  if (command > 1 || !elegoo_sdcp_valid_mainboard_id(mainboard_id)) return std::nullopt;
  if (outer_id.empty()) outer_id = mainboard_id;
  if (!elegoo_sdcp_valid_mainboard_id(outer_id)) return std::nullopt;
  std::array<char, 384> output{};
  const int length = std::snprintf(output.data(), output.size(),
      "{\"Id\":\"%.*s\",\"Topic\":\"sdcp/request/%.*s\",\"Data\":{\"Cmd\":%u,\"Data\":{},\"RequestID\":\"%016llx\",\"MainboardID\":\"%.*s\",\"TimeStamp\":%llu,\"From\":1}}",
      static_cast<int>(outer_id.size()), outer_id.data(), static_cast<int>(mainboard_id.size()), mainboard_id.data(), command,
      static_cast<unsigned long long>(request_id), static_cast<int>(mainboard_id.size()), mainboard_id.data(),
      static_cast<unsigned long long>(timestamp_ms));
  if (length <= 0 || static_cast<std::size_t>(length) >= output.size()) return std::nullopt;
  return std::string(output.data(), length);
}

ElegooSdcpParser::ElegooSdcpParser(std::uint32_t profile_id, std::string mainboard_id)
    : snapshot_(std::make_unique<core::PrinterSnapshot>()), expected_id_(lowercase(mainboard_id)) {
  snapshot_->profile_id = profile_id;
  snapshot_->link = core::LinkState::connecting;
  if (!elegoo_sdcp_valid_mainboard_id(expected_id_)) error_ = ElegooError::invalid_configuration;
}
ElegooSdcpParser::~ElegooSdcpParser() = default;

bool ElegooSdcpParser::seed_identity(const ElegooIdentity& identity) {
  if (!cc1_model(identity.model) || !elegoo_sdcp_valid_mainboard_id(identity.serial)) {
    error_ = ElegooError::unsupported_response; return false;
  }
  if (lowercase(identity.serial) != expected_id_) { error_ = ElegooError::identity_mismatch; return false; }
  identity_ = identity;
  identity_.serial = expected_id_;
  identity_known_ = true;
  return true;
}

ElegooSdcpMessage ElegooSdcpParser::ingest(std::string_view message, std::uint64_t now_ms) {
  if (error_ != ElegooError::none) return ElegooSdcpMessage::invalid;
  auto root = decode(message);
  if (!root) { error_ = ElegooError::unsupported_response; return ElegooSdcpMessage::invalid; }
  const auto* data = member(root.get(), "Data");
  const auto* status = member(root.get(), "Status");
  const auto* attributes = member(root.get(), "Attributes");
  if (!status) status = member(data, "Status");
  if (!attributes) attributes = member(data, "Attributes");
  if ((member(root.get(), "Status") && member(data, "Status")) || (member(root.get(), "Attributes") && member(data, "Attributes"))) {
    error_ = ElegooError::unsupported_response; return ElegooSdcpMessage::invalid;
  }
  // Validate all explicit routing IDs. IDs are identity, not authentication.
  for (const cJSON* object : {static_cast<const cJSON*>(root.get()), data, status, attributes}) {
    const auto* raw = member(object, "MainboardID");
    if (!raw) continue;
    const auto serial = text(raw, 32);
    if (!serial || lowercase(*serial) != expected_id_) { error_ = ElegooError::identity_mismatch; return ElegooSdcpMessage::invalid; }
  }
  for (const cJSON* envelope : {static_cast<const cJSON*>(root.get()), data}) {
    const auto* raw = member(envelope, "Topic");
    if (!raw) continue;
    const auto topic = text(raw, 96);
    if (!topic) { error_ = ElegooError::unsupported_response; return ElegooSdcpMessage::invalid; }
    if (!topic->empty()) {
      const auto slash = topic->rfind('/');
      const auto prefix = topic->substr(0, slash == std::string::npos ? 0 : slash + 1);
      if (slash == std::string::npos || lowercase(topic->substr(slash + 1)) != expected_id_) {
        error_ = ElegooError::identity_mismatch; return ElegooSdcpMessage::invalid;
      }
      if ((status && prefix != "sdcp/status/") || (attributes && prefix != "sdcp/attributes/")) {
        error_ = ElegooError::unsupported_response; return ElegooSdcpMessage::invalid;
      }
    }
  }
  if (attributes) {
    ElegooIdentity candidate;
    if (!decode_identity(attributes, candidate) || !seed_identity(candidate)) {
      if (error_ == ElegooError::none) error_ = ElegooError::unsupported_response;
      return ElegooSdcpMessage::invalid;
    }
    if (!status) return ElegooSdcpMessage::attributes;
  }
  // ACKs and unrelated messages never refresh status age or establish online.
  if (!status) return ElegooSdcpMessage::ignored;
  if (!cJSON_IsObject(status) || !status->child) return ElegooSdcpMessage::ignored;
  auto& job = snapshot_->job;
  bool began_epoch = false;
  const auto begin_job = [&] {
    if (began_epoch) return;
    began_epoch = true;
    if (++job_epoch_ == 0) { job_epoch_ = 1; completed_epoch_ = 0; }
    clear_job(job); task_id_.clear(); print_state_ = -1; total_ticks_known_ = false;
    completed_until_ms_ = 0;
    job.phase = core::JobPhase::unknown;
    job.activity = core::PrinterActivity::unknown;
  };
  // Cmd0 is polled for a current machine-state baseline. Ancillary deltas are
  // not evidence that the cached print phase/ETA is still current. Never merge
  // a late baseline into job data retained across a state-freshness gap.
  if (baseline_known_ && now_ms >= last_state_ms_ && now_ms - last_state_ms_ >= kElegooSdcpStatusLifetimeMs) {
    begin_job(); machine_state_ = -1; baseline_known_ = false;
  }
  const int previous_machine = machine_state_;
  const int previous_print = print_state_;
  bool meaningful = false;
  if (const auto* current = member(status, "CurrentStatus")) {
    if (!cJSON_IsArray(current) || cJSON_GetArraySize(current) < 1 || cJSON_GetArraySize(current) > 8) {
      error_ = ElegooError::unsupported_response; return ElegooSdcpMessage::invalid;
    }
    int state = -1;
    for (int i = 0; i < cJSON_GetArraySize(current); ++i) {
      int candidate = -1;
      if (!integer(cJSON_GetArrayItem(current, i), 0, 255, candidate)) {
        error_ = ElegooError::unsupported_response; return ElegooSdcpMessage::invalid;
      }
      if (i == 0 || (state == 2 && i == 1)) state = candidate;
    }
    if (previous_machine == 0 && state == 1) begin_job();
    machine_state_ = state; baseline_known_ = true; last_state_ms_ = now_ms; meaningful = true;
  }
  const auto* print = member(status, "PrintInfo");
  if (print && !cJSON_IsObject(print) && !cJSON_IsNull(print)) {
    error_ = ElegooError::unsupported_response; return ElegooSdcpMessage::invalid;
  }
  if (cJSON_IsNull(print) && machine_state_ != 0) {
    clear_job(job); task_id_.clear(); print_state_ = -1; total_ticks_known_ = false; meaningful = true;
  }
  if (cJSON_IsObject(print)) {
    for (const auto* field : {"TaskId", "Filename", "Progress", "CurrentTicks", "TotalTicks", "CurrentLayer", "TotalLayer", "PrintSpeedPct"})
      meaningful = meaningful || member(print, field);
    if (const auto* raw = member(print, "TaskId")) {
      const auto value = text(raw, 128);
      // Learning an identifier late is not evidence of a different job. A
      // changed known identifier is; anonymous boundaries use the local epoch.
      if (value && !value->empty() && !task_id_.empty() && *value != task_id_) begin_job();
      if (value || cJSON_IsNull(raw)) task_id_ = value.value_or("");
    }
    if (const auto* raw = member(print, "Status")) {
      int incoming_print = -1;
      if (!integer(raw, 0, 255, incoming_print)) {
        error_ = ElegooError::unsupported_response; return ElegooSdcpMessage::invalid;
      }
      const bool was_terminal = previous_print == 8 || previous_print == 9 || previous_print == 14;
      const bool becomes_active = incoming_print == 0 || incoming_print == 1 || incoming_print == 10 || incoming_print == 11 ||
          incoming_print == 12 || incoming_print == 13 || (incoming_print >= 15 && incoming_print <= 22);
      if (machine_state_ == 1 && was_terminal && becomes_active) {
        // Adjacent jobs need not publish an idle frame. Terminal -> active is
        // a new local epoch even when TaskId is absent and filename identical.
        const auto current_id = task_id_;
        begin_job(); task_id_ = current_id;
      }
      print_state_ = incoming_print;
      meaningful = true;
    }
    if (const auto* raw = member(print, "Filename")) {
      const auto value = text(raw, 256);
      job.name = value.value_or(""); job.gcode_file = job.name;
    }
    metric(print, "Progress", job.completion, job.completion_known, 100);
    for (const auto& entry : {std::pair{"CurrentLayer", &job.current_layer}, {"TotalLayer", &job.total_layers}}) {
      if (const auto* raw = member(print, entry.first)) {
        int value = 0;
        *entry.second = integer(raw, 0, 65535, value) ? static_cast<std::uint16_t>(value) : 0;
      }
    }
    if (const auto* raw = member(print, "CurrentTicks")) {
      double value = 0;
      job.elapsed_known = number(raw, 0, 31536000, value);
      job.elapsed_seconds = job.elapsed_known ? static_cast<std::uint32_t>(value) : 0;
    }
    if (const auto* raw = member(print, "TotalTicks")) {
      total_ticks_known_ = number(raw, 0, 31536000, total_ticks_) && total_ticks_ > 0;
    }
    job.remaining_known = job.elapsed_known && total_ticks_known_ && total_ticks_ >= job.elapsed_seconds;
    job.remaining_seconds = job.remaining_known && total_ticks_ > job.elapsed_seconds ? static_cast<std::uint32_t>(total_ticks_ - job.elapsed_seconds) : 0;
    metric(print, "PrintSpeedPct", job.motion.speed_multiplier, job.motion.speed_multiplier_known, 1000);
  }
  auto& temp = job.temperatures;
  temperature(status, "TempOfNozzle", "TempTargetNozzle", temp.nozzle_c, temp.nozzle_known, temp.nozzle_target_c, temp.nozzle_target_known, 500);
  temperature(status, "TempOfHotbed", "TempTargetHotbed", temp.bed_c, temp.bed_known, temp.bed_target_c, temp.bed_target_known, 200);
  if (const auto* raw = member(status, "TempOfBox")) {
    scalar(raw, temp.chamber_c, temp.chamber_known, 150);
  }
  if (const auto* fans = member(status, "CurrentFanSpeed")) {
    if (cJSON_IsNull(fans)) { job.motion.fan_percent_known = false; job.motion.fan_percent = 0; }
    else metric(fans, "ModelFan", job.motion.fan_percent, job.motion.fan_percent_known, 100);
  }
  if (const auto* coordinate = member(status, "CurrenCoord")) {
    const auto value = text(coordinate, 96);
    std::array<float, 3> parsed{};
    bool valid = value.has_value();
    std::string_view remaining = value ? std::string_view(*value) : std::string_view{};
    for (unsigned i = 0; valid && i < 3; ++i) {
      const auto comma = remaining.find(',');
      const auto part = remaining.substr(0, comma);
      const auto result = std::from_chars(part.data(), part.data() + part.size(), parsed[i]);
      valid = !part.empty() && result.ec == std::errc{} && result.ptr == part.data() + part.size() && std::isfinite(parsed[i]) && std::abs(parsed[i]) <= 10000 && (i == 2 ? comma == std::string_view::npos : comma != std::string_view::npos);
      if (i != 2 && comma != std::string_view::npos) remaining.remove_prefix(comma + 1);
    }
    job.motion.position_known = job.motion.x_known = job.motion.y_known = job.motion.z_known = valid;
    job.motion.x_mm = valid ? parsed[0] : 0; job.motion.y_mm = valid ? parsed[1] : 0; job.motion.z_mm = valid ? parsed[2] : 0;
  }
  // Temperature-only deltas remain useful, but cannot supply the initial state baseline.
  for (const auto* field : {"TempOfNozzle", "TempTargetNozzle", "TempOfHotbed", "TempTargetHotbed", "TempOfBox", "CurrenCoord", "CurrentFanSpeed"})
    meaningful = meaningful || member(status, field);
  if (!meaningful) return ElegooSdcpMessage::ignored;
  job.toolhead_count = 1; job.active_toolhead = 0;
  auto& tool = job.toolheads[0];
  tool.present = tool.active = true; tool.temperature_known = temp.nozzle_known;
  tool.target_known = temp.nozzle_target_known;
  tool.temperature_c = temp.nozzle_c; tool.target_c = temp.nozzle_target_c;
  update_phase(now_ms);
  received_status_ = true; last_status_ms_ = now_ms;
  snapshot_->updated_at_ms = now_ms;
  // Normalize only exported snapshots: normalizing an idle delta here would
  // destroy cached fields needed when the next delta resumes the same job.
  return ElegooSdcpMessage::status;
}

void ElegooSdcpParser::update_phase(std::uint64_t now_ms) {
  auto& job = snapshot_->job;
  const bool was_active = job.phase == core::JobPhase::printing || job.phase == core::JobPhase::preparing || job.phase == core::JobPhase::paused;
  job.kind = core::JobKind::print;
  job.condition = core::PrinterCondition::normal;
  job.activity = core::PrinterActivity::unknown;
  if (machine_state_ == 0) {
    // Some final reports already set the machine idle while PrintInfo carries
    // the terminal transition. Only latch if this session observed an active job.
    if (print_state_ == 9 && was_active && completed_epoch_ != job_epoch_) {
      completed_epoch_ = job_epoch_; completed_until_ms_ = now_ms + 2500;
    }
    job.phase = core::JobPhase::idle;
    job.activity = core::PrinterActivity::standby;
    // Sticky PrintInfo after idle is not an active or repeatedly completed job.
    return;
  }
  job.phase = core::JobPhase::unknown;
  if (machine_state_ != 1) { job.condition = core::PrinterCondition::busy; return; }
  switch (print_state_) {
    case 0: job.phase = core::JobPhase::preparing; job.activity = core::PrinterActivity::preparing; break;
    case 1: case 21: job.phase = core::JobPhase::preparing; job.activity = core::PrinterActivity::homing; break;
    case 5: case 6: job.phase = core::JobPhase::paused; job.activity = core::PrinterActivity::paused; break;
    case 7: case 8: job.phase = core::JobPhase::cancelled; job.activity = core::PrinterActivity::cancelled; break;
    case 9:
      job.phase = core::JobPhase::completed; job.activity = core::PrinterActivity::completed;
      job.completion_known = true; job.completion = 100;
      job.remaining_known = true; job.remaining_seconds = 0;
      if (completed_epoch_ != job_epoch_) {
        completed_epoch_ = job_epoch_; completed_until_ms_ = now_ms + 2500;
      }
      break;
    case 10: case 11: case 18: job.phase = core::JobPhase::preparing; job.activity = core::PrinterActivity::preparing; break;
    case 12: case 13: job.phase = core::JobPhase::printing; job.activity = core::PrinterActivity::printing; break;
    case 14: job.phase = core::JobPhase::failed; job.activity = core::PrinterActivity::failed; job.condition = core::PrinterCondition::error; break;
    case 15: case 19: job.phase = core::JobPhase::preparing; job.activity = core::PrinterActivity::bed_leveling; break;
    case 16: case 20: job.phase = core::JobPhase::preparing; job.activity = core::PrinterActivity::nozzle_heating; break;
    case 17: case 22: job.phase = core::JobPhase::preparing; job.activity = core::PrinterActivity::calibrating; job.kind = core::JobKind::calibration; break;
    default: job.condition = core::PrinterCondition::busy; break;
  }
  if (job.phase == core::JobPhase::printing && job.completion_known && job.completion == 100 &&
      completed_epoch_ != job_epoch_) {
    completed_epoch_ = job_epoch_; completed_until_ms_ = now_ms + 2500;
  }
}

bool ElegooSdcpParser::ready(std::uint64_t now_ms) const {
  return error_ == ElegooError::none && identity_known_ && baseline_known_ && received_status_ &&
      now_ms >= last_status_ms_ && now_ms - last_status_ms_ < kElegooSdcpStatusLifetimeMs &&
      now_ms >= last_state_ms_ && now_ms - last_state_ms_ < kElegooSdcpStatusLifetimeMs;
}
void ElegooSdcpParser::snapshot_into(core::PrinterSnapshot& out, std::uint64_t now_ms) const {
  out = *snapshot_;
  out.link = ready(now_ms) ? core::LinkState::online : core::LinkState::failed;
  out.job.reachable = out.link == core::LinkState::online;
  if (out.job.reachable && out.job.phase == core::JobPhase::idle && completed_until_ms_ > now_ms) {
    out.job.phase = core::JobPhase::completed; out.job.activity = core::PrinterActivity::completed;
    out.job.completion_known = true; out.job.completion = 100;
    out.job.remaining_known = true; out.job.remaining_seconds = 0;
  }
  if (!out.job.reachable || out.job.phase == core::JobPhase::idle) clear_job(out.job);
  out.job.normalize();
}

bool elegoo_sdcp_valid_upgrade(std::string_view headers, std::string_view expected_accept) {
  if (headers.size() > 4096 || !headers.ends_with("\r\n\r\n")) return false;
  for (const unsigned char c : headers) if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c > 126) return false;
  const auto first = headers.find("\r\n");
  if (first == std::string_view::npos || !headers.substr(0, first).starts_with("HTTP/1.1 101 ")) return false;
  headers.remove_prefix(first + 2);
  bool upgrade = false, connection = false, accept = false;
  std::set<std::string> names;
  while (headers != "\r\n") {
    const auto end = headers.find("\r\n");
    if (end == std::string_view::npos) return false;
    auto line = headers.substr(0, end);
    headers.remove_prefix(end + 2);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || !colon) return false;
    const auto name = lowercase(line.substr(0, colon));
    if (!names.emplace(name).second) return false;
    for (const auto c : name) if (!letter(c) && !digit(c) && c != '-') return false;
    auto value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    for (const unsigned char c : value) if (c < 32 || c == 127) return false;
    if (name == "upgrade") upgrade = lowercase(value) == "websocket";
    else if (name == "sec-websocket-accept") accept = value == expected_accept;
    else if (name == "connection") {
      while (!value.empty()) {
        const auto comma = value.find(',');
        auto token = value.substr(0, comma);
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        connection = connection || lowercase(token) == "upgrade";
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
      }
    } else if (name == "location" || name == "transfer-encoding" || name == "sec-websocket-extensions" || name == "sec-websocket-protocol") return false;
    else if (name == "content-length" && value != "0") return false;
  }
  return upgrade && connection && accept;
}

ElegooSdcpFrames::Result ElegooSdcpFrames::append(unsigned opcode, bool final,
    std::size_t frame_size, std::size_t offset, std::string_view chunk) {
  const auto invalid = [&] { reset(); return Result::invalid; };
  if (opcode == 8 || opcode == 9 || opcode == 10) {
    return final && frame_size <= 125 && offset <= frame_size && chunk.size() <= frame_size - offset ? Result::ignored : invalid();
  }
  if (opcode != 0 && opcode != 1) return invalid();
  if (!in_frame_) {
    if (offset || (opcode == 0 && !fragmented_) || (opcode == 1 && fragmented_)) return invalid();
    if (complete_) { message_.clear(); complete_ = false; }
    if (frame_size > kElegooSdcpMaximumMessage - message_.size()) return invalid();
    frame_size_ = frame_size; frame_offset_ = 0; frame_opcode_ = opcode; frame_final_ = final; in_frame_ = true;
  }
  if (opcode != frame_opcode_ || final != frame_final_ || frame_size != frame_size_ || offset != frame_offset_ ||
      offset > frame_size || chunk.size() > frame_size - offset) return invalid();
  message_.append(chunk); frame_offset_ += chunk.size();
  if (frame_offset_ != frame_size_) return Result::incomplete;
  in_frame_ = false; fragmented_ = !final;
  if (!final) return Result::incomplete;
  complete_ = true;
  return Result::complete;
}
std::string ElegooSdcpFrames::take() {
  if (!complete_) return {};
  auto value = std::move(message_); reset(); return value;
}
void ElegooSdcpFrames::reset() {
  message_.clear(); frame_size_ = frame_offset_ = 0; frame_opcode_ = 0;
  frame_final_ = in_frame_ = fragmented_ = complete_ = false;
}

}  // namespace printdeck::platform
