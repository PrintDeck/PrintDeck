#include "printdeck/platform/uniformation_sdcp_parser.hpp"
#include "printdeck/platform/ctb_preview.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <set>
#include "cJSON.h"

namespace printdeck::platform {
namespace {
const cJSON* member(const cJSON* value, const char* key) {
  return cJSON_IsObject(value) ? cJSON_GetObjectItemCaseSensitive(value, key) : nullptr;
}
std::optional<std::string> text(const cJSON* value, std::size_t limit) {
  if (!cJSON_IsString(value) || !value->valuestring) return std::nullopt;
  std::string result(value->valuestring);
  if (result.empty() || result.size() > limit) return std::nullopt;
  for (unsigned char c : result) if (c < 32 || c > 126) return std::nullopt;
  return result;
}
bool integer(const cJSON* value, int max, int& out) {
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < 0 || value->valuedouble > max ||
      std::floor(value->valuedouble) != value->valuedouble) return false;
  out = static_cast<int>(value->valuedouble); return true;
}
std::optional<std::uint64_t> uptime(const cJSON* value) {
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < 0 || value->valuedouble > 4294967295.0 ||
      std::floor(value->valuedouble) != value->valuedouble) return {};
  return static_cast<std::uint64_t>(value->valuedouble) * 1000;
}
std::string lower(std::string value) {
  for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}
bool unique(const cJSON* value) {
  std::set<std::string_view> keys;
  for (const auto* child = value->child; child; child = child->next) {
    if (cJSON_IsObject(value) && (!child->string || !keys.insert(child->string).second)) return false;
    if ((cJSON_IsObject(child) || cJSON_IsArray(child)) && !unique(child)) return false;
  }
  return true;
}
// Bound nesting before invoking the JSON parser, including ignored fields.
bool bounded_json(std::string_view value) {
  if (value.empty() || value.size() > kElegooSdcpMaximumMessage || value.find('\0') != value.npos || value.find("\\u0000") != value.npos) return false;
  bool quoted = false, escaped = false;
  int depth = 0;
  for (const unsigned char c : value) {
    if (quoted) {
      if (escaped) { escaped = false; continue; }
      if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') { if (++depth > 12) return false; }
    else if (c == '}' || c == ']') { if (--depth < 0) return false; }
  }
  return !quoted && depth == 0;
}
}

std::optional<UniformationLayerExecution> uniformation_layer_execution(std::string_view log) {
  if (log.empty() || log.size() > 32768 || log.find('\0') != log.npos) return {};
  std::optional<UniformationLayerExecution> pending, result;
  while (!log.empty()) {
    const auto end = log.find('\n');
    if (end == log.npos) break;  // A partially flushed last record is not evidence.
    auto line = log.substr(0, end); log.remove_prefix(end + 1);
    if (!line.starts_with("[printer][Info][")) continue;
    line.remove_prefix(16);
    std::uint64_t stamp = 0;
    const auto parsed = std::from_chars(line.data(), line.data() + line.size(), stamp);
    if (parsed.ec != std::errc{} || stamp > 4294967295000ULL) continue;
    line.remove_prefix(parsed.ptr - line.data());
    if (!line.starts_with("]:")) continue;
    line.remove_prefix(2);
    if (line.starts_with("layer ")) {
      line.remove_prefix(6);
      unsigned layer = 0;
      const auto count = std::from_chars(line.data(), line.data() + line.size(), layer);
      pending.reset(); result.reset();
      if (count.ec == std::errc{} && count.ptr < line.data() + line.size() &&
          *count.ptr == ':' && layer <= 65535)
        pending = UniformationLayerExecution{stamp, static_cast<std::uint16_t>(layer), 0};
    } else if (line.starts_with("execute: ") && pending && pending->uptime_ms == stamp) {
      constexpr std::string_view field = " exposure_time ";
      const auto at = line.find(field);
      if (at == line.npos || line.find(field, at + field.size()) != line.npos) continue;
      auto value = line.substr(at + field.size());
      const auto space = value.find_first_of(" \r");
      if (space != value.npos) value = value.substr(0, space);
      if (value.empty() || value.size() > 20) continue;
      // Stock firmware prints decimal milliseconds with six fractional digits.
      // Parse the decimal without locale or an unbounded temporary string.
      std::uint32_t milliseconds = 0;
      const auto whole = std::from_chars(value.data(), value.data() + value.size(), milliseconds);
      if (whole.ec != std::errc{} || milliseconds == 0 || milliseconds > 86400000) continue;
      auto suffix = value.substr(whole.ptr - value.data());
      if (!suffix.empty()) {
        if (suffix.front() != '.' || suffix.size() == 1) continue;
        suffix.remove_prefix(1);
        if (std::any_of(suffix.begin(), suffix.end(), [](char c) { return c < '0' || c > '9'; })) continue;
      }
      result = *pending; result->exposure_ms = milliseconds;
    } else if (line.find("exec_args.exposure_time") != line.npos) {
      // Exposure-zone adjustments can override the ordinary execution record.
      result.reset(); pending.reset();
    }
  }
  return result;
}

UniformationSdcpParser::UniformationSdcpParser(std::uint32_t profile_id, std::string expected_id)
    : snapshot_(std::make_unique<core::PrinterSnapshot>()), expected_id_(lower(std::move(expected_id))) {
  snapshot_->profile_id = profile_id;
  identity_.manufacturer = "UniFormation";
  if (!expected_id_.empty() && !elegoo_sdcp_valid_mainboard_id(expected_id_))
    error_ = ElegooError::invalid_configuration;
}

ElegooSdcpMessage UniformationSdcpParser::ingest(std::string_view message, std::uint64_t now_ms) {
  const auto fail = [&](ElegooError error = ElegooError::unsupported_response) {
    error_ = error; return ElegooSdcpMessage::invalid;
  };
  if (error_ != ElegooError::none) return ElegooSdcpMessage::invalid;
  if (message == "pong") return ElegooSdcpMessage::ignored;
  if (!bounded_json(message)) return fail();
  const char* end = nullptr;
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
      cJSON_ParseWithLengthOpts(message.data(), message.size(), &end, false), cJSON_Delete);
  if (!root || !cJSON_IsObject(root.get()) || !unique(root.get())) return fail();
  while (end < message.data() + message.size() && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
  if (end != message.data() + message.size()) return fail();
  const auto* data = member(root.get(), "Data");
  const auto* attributes = member(root.get(), "Attributes");
  const auto* status = member(root.get(), "Status");
  if (attributes && member(data, "Attributes")) return fail();
  if (status && member(data, "Status")) return fail();
  if (!attributes) attributes = member(data, "Attributes");
  if (!status) status = member(data, "Status");
  std::string message_id;
  for (const auto* object : {static_cast<const cJSON*>(root.get()), data, attributes, status}) {
    const auto* field = member(object, "MainboardID");
    if (!field) continue;
    const auto id = text(field, 32);
    if (!id || !elegoo_sdcp_valid_mainboard_id(*id)) return fail();
    const auto normalized = lower(*id);
    if ((!expected_id_.empty() && expected_id_ != normalized) ||
        (!message_id.empty() && message_id != normalized)) return fail(ElegooError::identity_mismatch);
    message_id = normalized;
  }
  if (const auto* field = member(root.get(), "Id")) {
    const auto id = text(field, 32);
    if (!id || !elegoo_sdcp_valid_mainboard_id(*id)) return fail();
    outer_id_ = *id;
  }
  for (const auto* object : {static_cast<const cJSON*>(root.get()), data}) {
    if (const auto* field = member(object, "Topic")) {
      const auto topic = text(field, 96);
      if (!topic) return fail();
      const auto slash = topic->rfind('/');
      if (slash == topic->npos) return fail();
      const auto id = lower(topic->substr(slash + 1));
      if (!elegoo_sdcp_valid_mainboard_id(id) ||
          (!message_id.empty() && id != message_id) ||
          (!expected_id_.empty() && id != expected_id_)) return fail(ElegooError::identity_mismatch);
      const auto prefix = topic->substr(0, slash + 1);
      if (prefix != (attributes ? "sdcp/attributes/" : status ? "sdcp/status/" : "sdcp/response/")) return fail();
    }
  }
  if (attributes) {
    const auto model = text(member(attributes, "MachineName"), 48);
    const auto brand = text(member(attributes, "BrandName"), 48);
    const auto protocol = text(member(attributes, "ProtocolVersion"), 24);
    const auto firmware = text(member(attributes, "FirmwareVersion"), 48);
    if (!model || (*model != "GK 3 Ultra" && *model != "GK3 Ultra") || !brand ||
        lower(*brand) != "uniformation" || !protocol || *protocol != "V3.0.0" ||
        !firmware || message_id.empty()) return fail();
    expected_id_ = message_id;
    identity_ = {.manufacturer = "UniFormation", .model = "GK3 Ultra", .serial = message_id, .firmware = *firmware};
    identity_known_ = true;
    if (!status) return ElegooSdcpMessage::attributes;
  }
  if (!status) {
    int command = 0;
    if (!identity_known_ || message_id != identity_.serial ||
        !integer(member(data, "Cmd"), 65535, command) || command != 321)
      return ElegooSdcpMessage::ignored;
    const auto* list = member(member(data, "Data"), "HistoryDetailList");
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > 16) return ElegooSdcpMessage::ignored;
    for (const auto* item = list->child; item; item = item->next) {
      const auto id = text(member(item, "TaskId"), 36);
      const auto* name = member(item, "TaskName");
      if (!id || *id != task_id_ || !cJSON_IsString(name) || !name->valuestring) continue;
      const std::string candidate(name->valuestring);
      if (candidate.empty() || candidate.size() > 512 ||
          std::any_of(candidate.begin(), candidate.end(), [](unsigned char c) { return c < 32 || c == 127; })) continue;
      task_name_ = candidate.substr(candidate.find_last_of("/\\") + 1);
      task_begin_ms_ = uptime(member(item, "BeginTime"));
      preview_path_ = ctb_preview_path(candidate).value_or("");
      task_details_known_ = true;
      task_settings_ = {};
      const auto* settings = member(item, "SliceInformation");
      const auto number = [&](const char* key, float maximum) -> std::optional<float> {
        const auto* value = member(settings, key);
        if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
            value->valuedouble < 0 || value->valuedouble > maximum) return {};
        return static_cast<float>(value->valuedouble);
      };
      const auto count = [&](const char* key) -> std::optional<std::uint16_t> {
        int value = 0;
        if (!integer(member(settings, key), 65535, value)) return {};
        return static_cast<std::uint16_t>(value);
      };
      task_settings_.layer_height_mm = number("layer_height", 10);
      if (task_settings_.layer_height_mm == 0) task_settings_.layer_height_mm.reset();
      task_settings_.volume_ml = number("volume", 100000);
      task_settings_.weight_g = number("weight", 100000);
      task_settings_.bottom_layers = count("bottom_layer_numbers");
      task_settings_.transition_layers = count("transition_layer_numbers");
      for (const auto* prefix : {"bottom_layer_", "normal_layer_"}) {
        auto& layer = prefix[0] == 'b' ? task_settings_.bottom : task_settings_.normal;
        const auto field = [&](const char* key, float maximum) {
          return number((std::string(prefix) + key).c_str(), maximum);
        };
        const auto lift = field("lift_height", 1000), lift2 = field("lift_height2", 1000);
        const auto drop2 = field("drop_height2", 1000);
        if (lift && lift2 && *lift2 <= *lift) layer.lift_mm = {*lift - *lift2, *lift2};
        if (lift && drop2 && *drop2 <= *lift) layer.retract_mm = {*lift - *drop2, *drop2};
        layer.lift_mm_s = {field("lift_speed", 1000), field("lift_speed2", 1000)};
        layer.retract_mm_s = {field("drop_speed", 1000), field("drop_speed2", 1000)};
        if (const auto exposure = field("exposure_time", 86400000)) layer.exposure_s = *exposure / 1000;
      }
      const auto* profile = member(settings, "profile_name");
      if (cJSON_IsString(profile) && profile->valuestring) {
        const std::string value(profile->valuestring);
        if (value.size() <= 128 && std::none_of(value.begin(), value.end(),
              [](unsigned char c) { return c < 32 || c == 127; }))
          task_settings_.profile_name = value;
      }
      if (snapshot_->job.preview_hint == task_id_) {
        snapshot_->job.resin_settings = task_settings_;
        snapshot_->job.name = core::job_name_for_display(task_name_);
        snapshot_->job.gcode_file = task_name_;
      }
    }
    return ElegooSdcpMessage::ignored;
  }
  // An ACK, temperature delta or an unrelated SDCP device cannot verify a printer.
  if (!identity_known_ || message_id != identity_.serial) return ElegooSdcpMessage::ignored;
  const auto* current = member(status, "CurrentStatus");
  if (!current) return ElegooSdcpMessage::ignored;
  if (!cJSON_IsArray(current) || cJSON_GetArraySize(current) < 1 || cJSON_GetArraySize(current) > 8) return fail();
  int machine = 0;
  for (int index = 0; index < cJSON_GetArraySize(current); ++index) {
    int value = 0;
    if (!integer(cJSON_GetArrayItem(current, index), 4, value)) return fail();
    if (value != 0) machine = value;
  }
  const auto* info = member(status, "PrintInfo");
  int phase = 0;
  if (machine == 1 && !integer(member(info, "Status"), 255, phase)) return fail();
  if (machine != 1) integer(member(info, "Status"), 255, phase);
  auto task = text(member(info, "TaskId"), 36).value_or("");
  if (!uniformation_valid_task_id(task)) task.clear();
  if (task != task_id_) {
    task_id_ = task; task_name_.clear(); preview_path_.clear(); task_settings_ = {}; task_details_known_ = false;
    task_begin_ms_.reset(); execution_.reset(); exposure_started_ms_.reset();
  }
  auto& job = snapshot_->job;
  const bool observed_lowering = job.resin_stage == core::ResinStage::lowering &&
      job.preview_hint == task && now_ms >= last_status_ms_ && now_ms - last_status_ms_ <= 2000;
  const auto previous_stage = job.resin_stage;
  const auto previous_layer = job.current_layer;
  job = {};
  using Phase = core::JobPhase;
  using Stage = core::ResinStage;
  job.phase = machine == 0 ? Phase::idle : Phase::unknown;
  job.resin_stage = machine == 0 ? Stage::standby : Stage::unknown;
  if (machine > 1) {
    job.condition = core::PrinterCondition::busy;
    job.resin_stage = machine == 2 ? Stage::transferring_file : machine == 3 ? Stage::exposure_test : Stage::device_test;
  }
  // Terminal job state can arrive with CurrentStatus already back at idle.
  if (machine == 1 || (machine == 0 && !task.empty() && phase >= 5 && phase <= 9)) {
    switch (phase) {
      case 0: job.phase = Phase::preparing; break;
      case 1: job.phase = Phase::preparing; job.resin_stage = Stage::homing; break;
      case 2: job.phase = Phase::printing; job.resin_stage = Stage::lowering; break;
      case 3: job.phase = Phase::printing; job.resin_stage = Stage::exposing; break;
      case 4: job.phase = Phase::printing; job.resin_stage = Stage::lifting; break;
      case 5: job.phase = Phase::paused; job.resin_stage = Stage::pausing; break;
      case 6: job.phase = Phase::paused; job.resin_stage = Stage::paused; break;
      case 7: job.phase = Phase::cancelled; job.resin_stage = Stage::stopping; break;
      case 8: job.phase = Phase::cancelled; job.resin_stage = Stage::stopped; break;
      case 9: job.phase = Phase::completed; job.resin_stage = Stage::completed; break;
      case 10: job.phase = Phase::preparing; job.resin_stage = Stage::checking_file; break;
      default: job.resin_stage = Stage::unknown; break;
    }
  }
  int error = 0;
  if (integer(member(info, "ErrorNumber"), 65535, error) && error != 0) {
    job.condition = core::PrinterCondition::error;
    job.phase = Phase::failed;
  }
  const bool has_job = job.phase != Phase::idle && (machine == 1 || !task.empty());
  if (has_job) {
    job.preview_hint = task_id_;
    job.resin_settings = task_settings_;
    const auto* filename = member(info, "Filename");
    if (cJSON_IsString(filename) && filename->valuestring) {
      const std::string candidate(filename->valuestring);
      if (!candidate.empty() && candidate.size() <= 512 &&
          std::none_of(candidate.begin(), candidate.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        task_name_ = candidate.substr(candidate.find_last_of("/\\") + 1);
    }
    job.gcode_file = task_name_;
    job.name = core::job_name_for_display(task_name_);
    int current = 0, total = 0;
    if (integer(member(info, "CurrentLayer"), 65535, current) &&
        integer(member(info, "TotalLayer"), 65535, total) && total > 0 && current <= total) {
      job.current_layer = current; job.total_layers = total;
      job.completion = 100.0F * current / total; job.completion_known = true;
    }
    // SDCP resin ticks are milliseconds. Do not subtract an unknown total or
    // let an overrun wrap into an enormous remaining time.
    int elapsed_ms = 0, total_ms = 0;
    if (integer(member(info, "CurrentTicks"), 2147483647, elapsed_ms)) {
      job.elapsed_seconds = static_cast<std::uint32_t>(elapsed_ms / 1000);
      job.elapsed_known = true;
      if (integer(member(info, "TotalTicks"), 2147483647, total_ms) && total_ms > 0) {
        job.remaining_seconds = static_cast<std::uint32_t>(std::max(0, total_ms - elapsed_ms) / 1000);
        job.remaining_known = true;
      }
    }
    if (job.phase == Phase::completed) {
      job.completion = 100; job.completion_known = true;
      job.remaining_seconds = 0; job.remaining_known = true;
    }
  }
  const auto* temperature = member(status, "TempOfBox");
  if (cJSON_IsNumber(temperature) && std::isfinite(temperature->valuedouble) &&
      temperature->valuedouble >= -20 && temperature->valuedouble <= 150) {
    job.temperatures.chamber_c = temperature->valuedouble;
    job.temperatures.chamber_known = true;
  }
  const auto telemetry = [&](const char* key, float minimum, float maximum) -> std::optional<float> {
    const auto* value = member(status, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < minimum || value->valuedouble > maximum) return {};
    return static_cast<float>(value->valuedouble);
  };
  job.resin_telemetry.chamber_target_c = telemetry("TempTargetBox", -20, 150);
  job.resin_telemetry.bottle_ml = telemetry("Currentvolum", 0, 100000);
  int feed = 0;
  if (integer(member(status, "Feed switch"), 1, feed)) job.resin_telemetry.feeder_enabled = feed != 0;
  printer_uptime_ms_ = uptime(member(root.get(), "TimeStamp"));
  if (!printer_uptime_ms_) printer_uptime_ms_ = uptime(member(data, "TimeStamp"));
  if (!active_layer_cycle() || previous_layer != job.current_layer) execution_.reset();
  if (job.resin_stage != Stage::exposing || job.phase != Phase::printing ||
      previous_layer != job.current_layer) exposure_started_ms_.reset();
  else if (previous_stage != Stage::exposing && observed_lowering)
    exposure_started_ms_ = now_ms;
  update_exposure();
  status_known_ = true; last_status_ms_ = now_ms;
  snapshot_->updated_at_ms = now_ms;
  return ElegooSdcpMessage::status;
}

bool UniformationSdcpParser::active_layer_cycle() const {
  const auto& job = snapshot_->job;
  return job.phase == core::JobPhase::printing && job.condition != core::PrinterCondition::error &&
      job.total_layers > 0 && !task_id_.empty() &&
      (job.resin_stage == core::ResinStage::lowering || job.resin_stage == core::ResinStage::exposing ||
       job.resin_stage == core::ResinStage::lifting);
}
bool UniformationSdcpParser::needs_layer_execution(std::uint64_t now_ms) const {
  return ready(now_ms) && active_layer_cycle() && task_begin_ms_ && printer_uptime_ms_ && !execution_ &&
      snapshot_->job.resin_stage != core::ResinStage::lifting;
}
void UniformationSdcpParser::ingest_layer_execution(const UniformationLayerExecution& value,
                                                   std::uint64_t now_ms) {
  if (!needs_layer_execution(now_ms) || value.layer != snapshot_->job.current_layer ||
      value.exposure_ms == 0 || value.exposure_ms > 86400000 ||
      value.uptime_ms < *task_begin_ms_ || value.uptime_ms > *printer_uptime_ms_ + 2000 ||
      *printer_uptime_ms_ > value.uptime_ms + 120000) return;
  execution_ = value;
  update_exposure();
}
void UniformationSdcpParser::update_exposure() {
  snapshot_->job.resin_exposure.reset();
  if (execution_ && exposure_started_ms_ && active_layer_cycle() &&
      snapshot_->job.resin_stage == core::ResinStage::exposing)
    snapshot_->job.resin_exposure = core::ResinExposureTiming{*exposure_started_ms_, execution_->exposure_ms};
}

bool UniformationSdcpParser::ready(std::uint64_t now_ms) const {
  return error_ == ElegooError::none && identity_known_ && status_known_ &&
      now_ms >= last_status_ms_ && now_ms - last_status_ms_ < kElegooSdcpStatusLifetimeMs;
}
void UniformationSdcpParser::snapshot_into(core::PrinterSnapshot& out, std::uint64_t now_ms) const {
  out = *snapshot_;
  out.link = ready(now_ms) ? core::LinkState::online : core::LinkState::failed;
  out.job.reachable = ready(now_ms);
  if (!out.job.reachable || now_ms < last_status_ms_ || now_ms - last_status_ms_ > 2500)
    out.job.resin_exposure.reset();
  out.job.normalize();
}

static std::string uniformation_request_envelope(unsigned command,
    std::string_view mainboard_id, std::string_view outer_id,
    std::uint64_t request_id, std::uint64_t timestamp_seconds, std::string_view task_id) {
  char id[33]; std::snprintf(id, sizeof(id), "%032llx", static_cast<unsigned long long>(request_id));
  const std::string data = command == 321 ? "{\"Id\":[\"" + std::string(task_id) + "\"]}" : "{}";
  std::string body = "{\"Id\":\"" + std::string(outer_id) + "\",\"Data\":{\"Cmd\":" +
      std::to_string(command) + ",\"Data\":" + data + ",\"RequestID\":\"" + id + "\",\"TimeStamp\":" +
      std::to_string(timestamp_seconds) + ",\"From\":0";
  if (!mainboard_id.empty()) body += ",\"MainboardID\":\"" + std::string(mainboard_id) + "\"";
  body += "}";
  if (!mainboard_id.empty()) body += ",\"Topic\":\"sdcp/request/" + std::string(mainboard_id) + "\"";
  return body + "}";
}

std::optional<std::string> uniformation_sdcp_read_request(unsigned command,
    std::string_view mainboard_id, std::string_view outer_id,
    std::uint64_t request_id, std::uint64_t timestamp_seconds, std::string_view task_id) {
  if ((command != 0 && command != 1 && command != 321) ||
      (command == 321 && (!uniformation_valid_task_id(task_id) || mainboard_id.empty())) ||
      (command != 321 && !task_id.empty()) || !elegoo_sdcp_valid_mainboard_id(outer_id) ||
      (!mainboard_id.empty() && !elegoo_sdcp_valid_mainboard_id(mainboard_id)) ||
      (command == 0 && mainboard_id.empty())) return std::nullopt;
  return uniformation_request_envelope(command, mainboard_id, outer_id, request_id,
                                       timestamp_seconds, task_id);
}
std::optional<std::string> uniformation_sdcp_control_request(core::ResinControl action,
    std::string_view mainboard_id, std::string_view outer_id,
    std::uint64_t request_id, std::uint64_t timestamp_seconds) {
  if (!elegoo_sdcp_valid_mainboard_id(mainboard_id) || !elegoo_sdcp_valid_mainboard_id(outer_id)) return {};
  unsigned command = 0;
  switch (action) {
    case core::ResinControl::pause: command = 129; break;
    case core::ResinControl::resume: command = 131; break;
    case core::ResinControl::stop: command = 130; break;
    default: return {};
  }
  return uniformation_request_envelope(command, mainboard_id, outer_id, request_id,
                                       timestamp_seconds, {});
}
bool uniformation_valid_task_id(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool uniformation_decode_preview_bmp(const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& pixels, std::uint16_t& width, std::uint16_t& height,
    std::size_t maximum_decoded_bytes) {
  pixels.clear(); width = height = 0;
  if (encoded.size() < 54 || encoded.size() > 1048576 || encoded[0] != 'B' || encoded[1] != 'M') return false;
  const auto u16 = [&](std::size_t at) -> std::uint16_t { return encoded[at] | (encoded[at + 1] << 8); };
  const auto u32 = [&](std::size_t at) -> std::uint32_t { return u16(at) | (static_cast<std::uint32_t>(u16(at + 2)) << 16); };
  const auto dib = u32(14), w = u32(18);
  const auto signed_h = static_cast<std::int32_t>(u32(22));
  if (dib != 40 || w == 0 || w > 512 || signed_h == 0 || signed_h < -512 || signed_h > 512 || u16(26) != 1) return false;
  const std::uint32_t h = signed_h < 0 ? -signed_h : signed_h;
  if (w * h * 4 > maximum_decoded_bytes) return false;
  const auto bpp = u16(28); const auto compression = u32(30);
  if ((bpp != 24 && bpp != 32 && bpp != 16) || (compression != 0 && !(bpp == 16 && compression == 3))) return false;
  std::size_t offset = u32(10);
  const std::size_t minimum = compression == 3 ? 66 : 54;
  if (offset == 0) offset = minimum; // Known stock GK3 header omits this value.
  if (offset < minimum || offset > encoded.size()) return false;
  std::uint32_t red_mask = 0x7c00, green_mask = 0x03e0, blue_mask = 0x001f;
  if (compression == 3) {
    if (encoded.size() < 66) return false;
    red_mask = u32(54); green_mask = u32(58); blue_mask = u32(62);
    if (blue_mask != 0x1f || !((red_mask == 0xf800 && green_mask == 0x07e0) ||
        (red_mask == 0x7c00 && green_mask == 0x03e0))) return false;
  }
  const std::size_t stride = ((w * bpp + 31) / 32) * 4;
  if (stride * h > encoded.size() - offset) return false;
  std::vector<std::uint8_t> decoded(w * h * 4);
  for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x) {
    const auto source = offset + (signed_h < 0 ? y : h - 1 - y) * stride + x * (bpp / 8);
    const auto destination = (y * w + x) * 4;
    if (bpp == 16) {
      const auto pixel = u16(source); const bool rgb565 = green_mask == 0x7e0;
      decoded[destination] = (pixel & blue_mask) * 255 / 31;
      decoded[destination + 1] = ((pixel & green_mask) >> 5) * 255 / (rgb565 ? 63 : 31);
      decoded[destination + 2] = ((pixel & red_mask) >> (rgb565 ? 11 : 10)) * 255 / 31;
    } else {
      std::copy_n(encoded.data() + source, 3, decoded.data() + destination);
    }
    decoded[destination + 3] = 255;
  }
  width = w; height = h; pixels = std::move(decoded); return true;
}
}  // namespace printdeck::platform
