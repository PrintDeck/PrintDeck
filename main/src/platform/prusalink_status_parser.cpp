#include "printdeck/platform/prusalink_status_parser.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <unordered_set>

#include "cJSON.h"

namespace printdeck::platform {
namespace {

using Document = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool valid_utf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80) continue;
    unsigned width = 0;
    std::uint32_t code = 0;
    if (first >= 0xc2 && first <= 0xdf) { width = 2; code = first & 0x1f; }
    else if (first >= 0xe0 && first <= 0xef) { width = 3; code = first & 0x0f; }
    else if (first >= 0xf0 && first <= 0xf4) { width = 4; code = first & 0x07; }
    else return false;
    if (i + width - 1 > text.size()) return false;
    for (unsigned j = 1; j < width; ++j) {
      const auto next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    if ((width == 3 && code < 0x800) || (width == 4 && code < 0x10000) ||
        code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}

bool bounded_json(std::string_view body, std::size_t limit) {
  if (body.empty() || body.size() > limit || !valid_utf8(body)) return false;
  unsigned depth = 0;
  bool quoted = false;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const unsigned char ch = body[i];
    if (ch == 0) return false;
    if (quoted) {
      if (ch < 0x20) return false;
      if (ch == '\\') {
        // cJSON stores C strings: embedded decoded NUL would hide a suffix.
        if (body.substr(i, 6) == "\\u0000") return false;
        if (++i == body.size()) return false;
      } else if (ch == '"') quoted = false;
    } else if (ch == '"') quoted = true;
    else if (ch == '-' || (ch >= '0' && ch <= '9')) {
      // cJSON accepts some non-JSON numeric spellings; enforce the wire grammar
      // before allowing a malformed numeric prefix to become valid telemetry.
      std::size_t end = i;
      if (body[end] == '-' && ++end == body.size()) return false;
      if (body[end] == '0') ++end;
      else {
        if (body[end] < '1' || body[end] > '9') return false;
        while (end < body.size() && body[end] >= '0' && body[end] <= '9') ++end;
      }
      if (end < body.size() && body[end] == '.') {
        const auto begin = ++end;
        while (end < body.size() && body[end] >= '0' && body[end] <= '9') ++end;
        if (begin == end) return false;
      }
      if (end < body.size() && (body[end] == 'e' || body[end] == 'E')) {
        ++end;
        if (end < body.size() && (body[end] == '+' || body[end] == '-')) ++end;
        const auto begin = end;
        while (end < body.size() && body[end] >= '0' && body[end] <= '9') ++end;
        if (begin == end) return false;
      }
      if (end < body.size() && std::string_view(",]} \r\n\t").find(body[end]) == std::string_view::npos) return false;
      i = end - 1;
    }
    else if (ch == '{' || ch == '[') { if (++depth > 16) return false; }
    else if (ch == '}' || ch == ']') { if (depth == 0) return false; --depth; }
  }
  return !quoted && depth == 0;
}

bool unique_members(const cJSON* node, unsigned& count) {
  if (++count > 2048) return false;
  std::unordered_set<std::string_view> keys;
  for (const cJSON* child = node->child; child; child = child->next) {
    if (cJSON_IsObject(node) &&
        (!child->string || !keys.emplace(child->string).second)) return false;
    if (!unique_members(child, count)) return false;
  }
  return true;
}

Document parse(std::string_view body, std::size_t limit = 32 * 1024) {
  Document doc(nullptr, cJSON_Delete);
  if (!bounded_json(body, limit)) return doc;
  const char* end = nullptr;
  doc.reset(cJSON_ParseWithLengthOpts(body.data(), body.size(), &end, false));
  if (!doc || !cJSON_IsObject(doc.get())) return Document(nullptr, cJSON_Delete);
  const char* stop = body.data() + body.size();
  while (end < stop && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
  unsigned count = 0;
  if (end != stop || !unique_members(doc.get(), count)) doc.reset();
  return doc;
}

const cJSON* member(const cJSON* object, const char* key) {
  return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

std::string str(const cJSON* object, const char* key, std::size_t limit = 128) {
  const cJSON* value = member(object, key);
  if (!cJSON_IsString(value) || !value->valuestring) return {};
  std::string_view text(value->valuestring);
  for (const unsigned char ch : text) if (ch < 0x20 || ch == 0x7f) return {};
  if (text.size() > limit) {
    while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80) --limit;
    text = text.substr(0, limit);
  }
  return std::string(text);
}

bool number(const cJSON* object, const char* key, float& out, double low, double high) {
  const cJSON* value = member(object, key);
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < low || value->valuedouble > high) return false;
  out = static_cast<float>(value->valuedouble);
  return true;
}

std::optional<std::uint32_t> integer(const cJSON* object, const char* key) {
  const cJSON* value = member(object, key);
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < 0 || value->valuedouble > std::numeric_limits<std::uint32_t>::max() ||
      std::floor(value->valuedouble) != value->valuedouble) return {};
  return static_cast<std::uint32_t>(value->valuedouble);
}

bool flag(const cJSON* object, const char* key) { return cJSON_IsTrue(member(object, key)); }

void phase(std::string_view state, bool has_job, core::JobState& job) {
  using core::JobPhase;
  using core::PrinterCondition;
  job.condition = PrinterCondition::normal;
  if (state == "IDLE" || state == "READY") {
    job.phase = JobPhase::idle;
    if (state == "READY") job.condition = PrinterCondition::ready;
  } else if (state == "PRINTING") job.phase = JobPhase::printing;
  else if (state == "PAUSED") job.phase = JobPhase::paused;
  else if (state == "FINISHED") job.phase = JobPhase::completed;
  else if (state == "STOPPED") job.phase = JobPhase::cancelled;
  else if (state == "ATTENTION") {
    job.condition = PrinterCondition::attention;
    job.phase = has_job ? JobPhase::paused : JobPhase::unknown;
  } else if (state == "BUSY") job.condition = PrinterCondition::busy;
  else if (state == "ERROR") job.condition = PrinterCondition::error;
  else job.condition = PrinterCondition::unknown;
}

PrusaLinkSample sample(std::uint32_t profile_id, std::uint64_t now_ms) {
  PrusaLinkSample result;
  result.snapshot.profile_id = profile_id;
  result.snapshot.updated_at_ms = now_ms;
  result.snapshot.link = core::LinkState::online;
  result.snapshot.job.reachable = true;
  return result;
}

bool carries_job(const core::JobState& job) {
  return job.phase != core::JobPhase::idle;
}

void progress(const cJSON* values, const char* percent, const char* elapsed,
              const char* remaining, bool fraction, core::JobState& job) {
  if (!carries_job(job)) return;
  job.completion_known = number(values, percent, job.completion, 0, fraction ? 1 : 100);
  if (fraction && job.completion_known) job.completion *= 100;
  if (const auto value = integer(values, elapsed)) {
    job.elapsed_known = true;
    job.elapsed_seconds = *value;
  }
  if (const auto value = integer(values, remaining)) {
    job.remaining_known = true;
    job.remaining_seconds = *value;
  }
}

void filename(const cJSON* file, core::JobState& job, const char* display_key) {
  if (!carries_job(job)) return;
  job.name = str(file, display_key, 256);
  if (job.name.empty()) job.name = str(file, "name", 256);
  // Paths are metadata only. They never become credential-bearing requests.
  job.gcode_file = str(file, "path", 512);
  if (job.name.empty()) job.name = job.gcode_file;
  const auto slash = job.name.find_last_of('/');
  if (slash != std::string::npos) job.name.erase(0, slash + 1);
}

}  // namespace

std::optional<PrusaLinkIdentity> parse_prusalink_identity(std::string_view body) {
  auto doc = parse(body, 16 * 1024);
  const auto text = str(doc.get(), "text");
  // The standalone suffix is a version, while old Buddy used MINI.
  if (text != "PrusaLink" && text != "PrusaLink MINI" &&
      !(text.starts_with("PrusaLink ") && text.size() > 10 &&
        text[10] >= '0' && text[10] <= '9')) return {};
  PrusaLinkIdentity result;
  result.api_version = str(doc.get(), "api");
  result.server_version = str(doc.get(), "server");
  result.firmware_version = str(doc.get(), "firmware");
  if (result.api_version.empty() || result.server_version.empty()) return {};
  const auto model = str(doc.get(), "printer");
  if (model == "1.4.0") result.model = "MK4";
  else if (text == "PrusaLink MINI") result.model = "MINI";
  // Unknown future model codes stay generic until a mapping is verified.
  return result;
}

std::optional<PrusaLinkSample> parse_prusalink_v1_status(
    std::string_view body, std::uint32_t profile_id, std::uint64_t now_ms) {
  auto doc = parse(body);
  const auto* printer = member(doc.get(), "printer");
  const auto state = str(printer, "state", 64);
  if (state.empty()) return {};
  auto result = sample(profile_id, now_ms);
  const auto* info = member(doc.get(), "job");
  if (info && !cJSON_IsObject(info) && !cJSON_IsNull(info)) return {};
  result.job_id = integer(info, "id");
  if (member(info, "id") && !result.job_id) return {};
  auto& job = result.snapshot.job;
  phase(state, result.job_id.has_value(), job);
  auto& temp = job.temperatures;
  temp.nozzle_known = number(printer, "temp_nozzle", temp.nozzle_c, -100, 600);
  temp.nozzle_target_known = number(printer, "target_nozzle", temp.nozzle_target_c, 0, 600);
  temp.bed_known = number(printer, "temp_bed", temp.bed_c, -100, 300);
  temp.bed_target_known = number(printer, "target_bed", temp.bed_target_c, 0, 300);
  auto& motion = job.motion;
  motion.x_known = number(printer, "axis_x", motion.x_mm, -100000, 100000);
  motion.y_known = number(printer, "axis_y", motion.y_mm, -100000, 100000);
  motion.z_known = number(printer, "axis_z", motion.z_mm, -100000, 100000);
  motion.position_known = motion.x_known && motion.y_known && motion.z_known;
  motion.speed_multiplier_known = number(printer, "speed", motion.speed_multiplier, 0, 1000);
  motion.extrusion_multiplier_known = number(printer, "flow", motion.extrusion_multiplier, 0, 1000);
  // Prusa reports fan RPM, not percent. Do not populate fan_percent.
  progress(info, "progress", "time_printing", "time_remaining", false, job);
  job.normalize();
  return result;
}

bool apply_prusalink_v1_job(std::string_view body, PrusaLinkSample& sample) {
  auto doc = parse(body);
  const auto id = integer(doc.get(), "id");
  if (!id || !sample.job_id || *id != *sample.job_id ||
      !cJSON_IsObject(member(doc.get(), "file"))) return false;
  filename(member(doc.get(), "file"), sample.snapshot.job, "display_name");
  sample.preview_path = str(member(member(doc.get(), "file"), "refs"), "thumbnail", 512);
  sample.metadata_loaded = true;
  return true;
}

std::optional<PrusaLinkSample> parse_prusalink_legacy_status(
    std::string_view printer_body, std::string_view job_body,
    std::uint32_t profile_id, std::uint64_t now_ms) {
  auto printer_doc = parse(printer_body);
  auto job_doc = parse(job_body);
  const auto* state = member(printer_doc.get(), "state");
  const auto* flags = member(state, "flags");
  if (!printer_doc || !job_doc || !cJSON_IsObject(flags) ||
      !cJSON_IsObject(member(job_doc.get(), "job")) ||
      !cJSON_IsObject(member(job_doc.get(), "progress"))) return {};
  bool has_state_evidence = false;
  for (const auto* key : {"printing", "paused", "operational", "ready", "finished", "cancelling", "error", "closedOrError"}) {
    if (const auto* value = member(flags, key)) {
      if (!cJSON_IsBool(value)) return {};
      has_state_evidence = true;
    }
  }
  if (member(flags, "link_state") && str(flags, "link_state", 64).empty()) return {};
  if (!has_state_evidence && !member(flags, "link_state")) return {};
  auto result = sample(profile_id, now_ms);
  auto& job = result.snapshot.job;
  auto link_state = str(flags, "link_state", 64);
  if (link_state.empty()) {
    if (flag(flags, "error") || flag(flags, "closedOrError")) link_state = "ERROR";
    else if (flag(flags, "paused")) link_state = "PAUSED";
    else if (flag(flags, "printing")) link_state = "PRINTING";
    else if (flag(flags, "finished")) link_state = "FINISHED";
    else if (flag(flags, "cancelling")) link_state = "STOPPED";
    else if (flag(flags, "operational") || flag(flags, "ready")) link_state = "IDLE";
    else if (!member(flags, "printing") && !member(flags, "operational")) return {};
  }
  phase(link_state, flag(flags, "printing") || flag(flags, "paused"), job);
  const auto* temperatures = member(printer_doc.get(), "temperature");
  const auto* nozzle = member(temperatures, "tool0");
  const auto* bed = member(temperatures, "bed");
  auto& temp = job.temperatures;
  temp.nozzle_known = number(nozzle, "actual", temp.nozzle_c, -100, 600);
  temp.nozzle_target_known = number(nozzle, "target", temp.nozzle_target_c, 0, 600);
  temp.bed_known = number(bed, "actual", temp.bed_c, -100, 300);
  temp.bed_target_known = number(bed, "target", temp.bed_target_c, 0, 300);
  const auto* telemetry = member(printer_doc.get(), "telemetry");
  auto& motion = job.motion;
  motion.x_known = number(telemetry, "axis_x", motion.x_mm, -100000, 100000);
  motion.y_known = number(telemetry, "axis_y", motion.y_mm, -100000, 100000);
  motion.z_known = number(telemetry, "axis_z", motion.z_mm, -100000, 100000);
  if (!motion.z_known) motion.z_known = number(telemetry, "z-height", motion.z_mm, -100000, 100000);
  motion.position_known = motion.x_known && motion.y_known && motion.z_known;
  motion.speed_multiplier_known = number(telemetry, "print-speed", motion.speed_multiplier, 0, 1000);
  const auto* job_progress = member(job_doc.get(), "progress");
  progress(job_progress, "completion", "printTime", "printTimeLeft", true, job);
  if (!motion.z_known) motion.z_known = number(job_progress, "pos_z_mm", motion.z_mm, -100000, 100000);
  motion.position_known = motion.x_known && motion.y_known && motion.z_known;
  if (!motion.speed_multiplier_known)
    motion.speed_multiplier_known = number(job_progress, "printSpeed", motion.speed_multiplier, 0, 1000);
  motion.extrusion_multiplier_known = number(job_progress, "flow_factor", motion.extrusion_multiplier, 0, 1000);
  filename(member(member(job_doc.get(), "job"), "file"), job, "display");
  job.normalize();
  return result;
}

}  // namespace printdeck::platform
