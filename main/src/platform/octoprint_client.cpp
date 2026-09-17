#include "printdeck/platform/octoprint_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include "cJSON.h"

namespace printdeck::platform {
namespace {
constexpr std::size_t kMaximumBody = 32768;
using Document = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
bool unique_members(const cJSON* value) {
  for (const auto* child = value->child; child; child = child->next) {
    if (cJSON_IsObject(value) && (!child->string || cJSON_GetObjectItemCaseSensitive(value, child->string) != child)) return false;
    if (!unique_members(child)) return false;
  }
  return true;
}
Document parse(std::string_view body) {
  if (body.empty() || body.size() > kMaximumBody || body.find('\0') != std::string_view::npos) return {nullptr, cJSON_Delete};
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (unsigned char ch : body) {
    if (quoted) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') quoted = false;
    } else if (ch == '"') quoted = true;
    else if (ch == '{' || ch == '[') { if (++depth > 20) return {nullptr, cJSON_Delete}; }
    else if (ch == '}' || ch == ']') { if (!depth) return {nullptr, cJSON_Delete}; --depth; }
  }
  if (depth || quoted) return {nullptr, cJSON_Delete};
  const char* end = nullptr;
  Document result(cJSON_ParseWithLengthOpts(body.data(), body.size(), &end, false), cJSON_Delete);
  if (!result || !cJSON_IsObject(result.get()) || !unique_members(result.get())) return {nullptr, cJSON_Delete};
  while (end < body.data() + body.size() && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
  if (end != body.data() + body.size()) return {nullptr, cJSON_Delete};
  return result;
}
const cJSON* field(const cJSON* doc, const char* key) { return cJSON_GetObjectItemCaseSensitive(doc, key); }
std::optional<std::string> text(const cJSON* doc, const char* key, std::size_t maximum) {
  const auto* value = field(doc, key);
  if (!cJSON_IsString(value) || !value->valuestring) return {};
  const std::string result(value->valuestring);
  if (result.size() > maximum || std::any_of(result.begin(), result.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) return {};
  return result;
}
std::optional<double> number(const cJSON* doc, const char* key, double minimum, double maximum) {
  const auto* value = field(doc, key);
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < minimum || value->valuedouble > maximum) return {};
  return value->valuedouble;
}
bool flag(const cJSON* doc, const char* key) { return cJSON_IsTrue(field(doc, key)); }
bool offline_state(std::string_view state) {
  return state == "Offline" || state == "Offline after error" || state == "Closed" ||
      state == "Connecting" || state == "Detecting serial connection";
}
PrusaLinkError response_error(const PrusaLinkHttpResponse& response, bool printer = false) {
  if (response.error != PrusaLinkError::none) return response.error;
  if (response.status == 401 || response.status == 403) return PrusaLinkError::authorization;
  if (printer && response.status == 409) return PrusaLinkError::printer_disconnected;
  if (response.status == 429 || response.status >= 500) return PrusaLinkError::service_not_ready;
  if (response.status != 200 || (!response.content_encoding.empty() && response.content_encoding != "identity"))
    return PrusaLinkError::unsupported_response;
  return PrusaLinkError::none;
}
}  // namespace

bool octoprint_identity(std::string_view body, PrusaLinkIdentity* identity) {
  const auto doc = parse(body);
  if (!doc) return false;
  const auto api = text(doc.get(), "api", 32), server = text(doc.get(), "server", 64), label = text(doc.get(), "text", 128);
  if (!api || *api != "0.1" || !server || server->empty() || !label || !label->starts_with("OctoPrint ") || label->size() <= 10) return false;
  if (identity) *identity = {.api_version = *api, .server_version = *server, .model = "OctoPrint"};
  return true;
}

std::optional<PrusaLinkSample> parse_octoprint_status(std::string_view printer_body,
    std::string_view job_body, std::uint32_t profile_id, std::uint64_t now_ms) {
  const auto printer_doc = parse(printer_body), job_doc = parse(job_body);
  if (!printer_doc || !job_doc) return {};
  const auto* state = field(printer_doc.get(), "state");
  const auto* flags = field(state, "flags");
  const auto printer_state = text(state, "text", 128), job_state = text(job_doc.get(), "state", 128);
  const auto* job_data = field(job_doc.get(), "job");
  const auto* progress = field(job_doc.get(), "progress");
  if (!cJSON_IsObject(flags) || !printer_state || !job_state || !cJSON_IsObject(job_data) || !cJSON_IsObject(progress)) return {};
  for (const char* key : {"operational", "printing", "paused", "error", "closedOrError", "ready", "pausing", "resuming", "cancelling", "finishing"}) {
    const auto* value = field(flags, key);
    if (value && !cJSON_IsBool(value)) return {};
  }
  if (!cJSON_IsBool(field(flags, "operational")) || !cJSON_IsBool(field(flags, "printing")) ||
      !cJSON_IsBool(field(flags, "paused"))) return {};
  PrusaLinkSample sample;
  auto& snapshot = sample.snapshot;
  snapshot.profile_id = profile_id; snapshot.updated_at_ms = now_ms;
  // A healthy OctoPrint host does not establish a connection to its printer.
  const bool connected = flag(flags, "operational") && !flag(flags, "closedOrError") &&
      !offline_state(*printer_state) && !offline_state(*job_state);
  snapshot.link = connected ? core::LinkState::online : core::LinkState::failed;
  auto& job = snapshot.job;
  job.reachable = connected;
  if (!connected) return sample;
  job.phase = core::JobPhase::idle;
  if (flag(flags, "error") || *job_state == "Error" || *printer_state == "Error") {
    job.phase = core::JobPhase::failed; job.condition = core::PrinterCondition::error;
  } else if (flag(flags, "cancelling") || *job_state == "Cancelling") {
    job.phase = core::JobPhase::cancelled;
  } else if (flag(flags, "paused") || flag(flags, "pausing") || *job_state == "Paused" || *job_state == "Pausing") {
    job.phase = core::JobPhase::paused;
  } else if (flag(flags, "resuming") || *job_state == "Resuming") {
    job.phase = core::JobPhase::preparing;
  } else if (flag(flags, "printing") || flag(flags, "finishing") || *job_state == "Printing" || *job_state == "Finishing") {
    job.phase = core::JobPhase::printing;
  } else if (*job_state != "Operational" && *job_state != "Ready") {
    job.phase = core::JobPhase::unknown;
  }
  const auto* temperature = field(printer_doc.get(), "temperature");
  if (temperature && !cJSON_IsObject(temperature)) return {};
  for (std::size_t index = 0; index < core::kMaximumToolheads; ++index) {
    const auto name = "tool" + std::to_string(index);
    const auto* tool = field(temperature, name.c_str());
    if (!tool || cJSON_IsNull(tool)) continue;
    if (!cJSON_IsObject(tool)) return {};
    auto& target = job.toolheads[index];
    target.present = true;
    job.toolhead_count = static_cast<std::uint8_t>(index + 1);
    if (const auto value = number(tool, "actual", -100, 1000)) { target.temperature_c = *value; target.temperature_known = true; }
    if (const auto value = number(tool, "target", 0, 1000)) { target.target_c = *value; target.target_known = true; }
  }
  // REST temperature data has no active-tool index. Show tool0 in the compact
  // temperature summary without claiming which tool a multi-extruder uses.
  if (job.toolheads[0].present) {
    job.temperatures.nozzle_c = job.toolheads[0].temperature_c;
    job.temperatures.nozzle_known = job.toolheads[0].temperature_known;
    job.temperatures.nozzle_target_c = job.toolheads[0].target_c;
    job.temperatures.nozzle_target_known = job.toolheads[0].target_known;
  }
  if (job.toolhead_count == 1 && job.toolheads[0].present) { job.active_toolhead = 0; job.toolheads[0].active = true; }
  const auto* bed = field(temperature, "bed"), *chamber = field(temperature, "chamber");
  if (const auto value = number(bed, "actual", -100, 1000)) { job.temperatures.bed_c = *value; job.temperatures.bed_known = true; }
  if (const auto value = number(bed, "target", 0, 1000)) { job.temperatures.bed_target_c = *value; job.temperatures.bed_target_known = true; }
  if (const auto value = number(chamber, "actual", -100, 1000)) { job.temperatures.chamber_c = *value; job.temperatures.chamber_known = true; }
  const auto* file = field(job_data, "file");
  const auto name = text(file, "display", 256).value_or(text(file, "name", 256).value_or(""));
  const auto path = text(file, "path", 512).value_or(text(file, "name", 256).value_or(""));
  const auto origin = text(file, "origin", 16).value_or("");
  if (!path.empty() && (origin == "local" || origin == "sdcard")) {
    job.name = name;
    job.gcode_file = origin + ":" + path;
  }
  if (const auto value = number(progress, "completion", 0, 100)) { job.completion = *value; job.completion_known = true; }
  if (const auto value = number(progress, "printTime", 0, 4294967295.0)) { job.elapsed_seconds = static_cast<std::uint32_t>(*value); job.elapsed_known = true; }
  if (const auto value = number(progress, "printTimeLeft", 0, 4294967295.0)) { job.remaining_seconds = static_cast<std::uint32_t>(*value); job.remaining_known = true; }
  // Empty idle state must not display a stale filename or retain old timing.
  if (job.gcode_file.empty()) {
    job.name.clear(); job.completion_known = false; job.elapsed_known = false; job.remaining_known = false;
  }
  // Preserve idle progress until the session can recognize a terminal transition.
  if (job.phase != core::JobPhase::idle) job.normalize();
  return sample;
}

bool OctoPrintClient::configure(std::string_view endpoint, std::string api_key,
    std::uint32_t profile_id, bool allow_loopback) {
  origin_.clear(); api_key_.clear(); identity_ = {}; previous_.reset(); last_sample_ms_ = 0;
  const auto origin = prusalink_origin(endpoint, allow_loopback);
  if (!origin || !prusalink_credential_valid(api_key, 128)) return false;
  origin_ = *origin; api_key_ = std::move(api_key); profile_id_ = profile_id;
  return true;
}

PrusaLinkPollResult OctoPrintClient::poll(std::uint64_t deadline, const std::function<bool()>& cancelled) {
  if (origin_.empty()) return {.error = PrusaLinkError::invalid_configuration};
  const auto stopped = [&] { return (cancelled && cancelled()) || now_ms_() >= deadline; };
  const auto failure = [&](PrusaLinkError error) {
    previous_.reset(); identity_ = {}; last_sample_ms_ = 0;
    return PrusaLinkPollResult{.error = error};
  };
  const auto get = [&](const char* path) {
    if (stopped()) return PrusaLinkHttpResponse{.error = cancelled && cancelled() ? PrusaLinkError::cancelled : PrusaLinkError::timeout};
    return transport_.get({.url = origin_ + path, .header_name = "X-Api-Key", .header_value = api_key_,
        .maximum_body = kMaximumBody, .deadline_ms = deadline}, cancelled);
  };
  if (identity_.model.empty()) {
    const auto response = get("/api/version");
    if (const auto error = response_error(response); error != PrusaLinkError::none) return failure(error);
    if (!octoprint_identity(response.body, &identity_)) return failure(PrusaLinkError::unsupported_response);
  }
  const auto printer = get("/api/printer");
  if (const auto error = response_error(printer, true); error != PrusaLinkError::none) return failure(error);
  const auto job = get("/api/job");
  if (const auto error = response_error(job); error != PrusaLinkError::none) return failure(error);
  if (stopped()) return failure(cancelled && cancelled() ? PrusaLinkError::cancelled : PrusaLinkError::timeout);
  const auto now = now_ms_();
  auto sample = parse_octoprint_status(printer.body, job.body, profile_id_, now);
  if (!sample) return failure(PrusaLinkError::unsupported_response);
  if (sample->snapshot.link != core::LinkState::online) return failure(PrusaLinkError::printer_disconnected);
  auto& current = sample->snapshot.job;
  if (previous_ && now >= last_sample_ms_ && now - last_sample_ms_ <= 20000) {
    const auto& before = previous_->snapshot.job;
    // OctoPrint has no REST "completed" state. A completion needs an observed
    // same-job transition and explicit 100% evidence; idle after an interrupted
    // session or partial/cancelled job can never become a successful print.
    if (current.phase == core::JobPhase::idle && !current.gcode_file.empty() &&
        current.gcode_file == before.gcode_file && current.completion_known && current.completion == 100 &&
        ((before.phase == core::JobPhase::printing && before.completion_known) || before.phase == core::JobPhase::completed)) {
      current.phase = core::JobPhase::completed;
      current.remaining_seconds = 0; current.remaining_known = true;
    }
  }
  // Retain raw terminal evidence privately; the published idle state is normalized.
  previous_ = std::make_unique<PrusaLinkSample>(*sample); last_sample_ms_ = now;
  current.normalize();
  return {.sample = std::move(sample)};
}

}  // namespace printdeck::platform
