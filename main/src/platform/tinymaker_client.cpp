#include "printdeck/platform/tinymaker_client.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include "cJSON.h"

namespace printdeck::platform {
namespace {
using Document = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Document parse(std::string_view body) {
  if (body.empty() || body.size() > 16384) return {nullptr, cJSON_Delete};
  // Bound nesting before entering cJSON's recursive parser.
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (const unsigned char ch : body) {
    if (quoted) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') quoted = false;
    } else if (ch == '"') quoted = true;
    else if (ch == '{' || ch == '[') { if (++depth > 24) return {nullptr, cJSON_Delete}; }
    else if (ch == '}' || ch == ']') { if (!depth) return {nullptr, cJSON_Delete}; --depth; }
  }
  const char* end = nullptr;
  Document doc(cJSON_ParseWithLengthOpts(body.data(), body.size(), &end, false), cJSON_Delete);
  if (!doc || !cJSON_IsObject(doc.get())) return {nullptr, cJSON_Delete};
  while (end < body.data() + body.size() && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
  if (end != body.data() + body.size()) return {nullptr, cJSON_Delete};
  for (auto* field = doc->child; field; field = field->next)
    if (cJSON_GetObjectItemCaseSensitive(doc.get(), field->string) != field) return {nullptr, cJSON_Delete};
  return doc;
}
const cJSON* field(const cJSON* doc, const char* key) { return cJSON_GetObjectItemCaseSensitive(doc, key); }
std::optional<std::string> text(const cJSON* doc, const char* key, std::size_t limit = 128) {
  const auto* item = field(doc, key);
  if (!cJSON_IsString(item) || !item->valuestring) return {};
  std::string result(item->valuestring);
  if (result.size() > limit || std::any_of(result.begin(), result.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) return {};
  return result;
}
std::optional<double> number(const cJSON* doc, const char* key, double maximum) {
  const auto* item = field(doc, key);
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 || item->valuedouble > maximum) return {};
  return item->valuedouble;
}
std::optional<std::uint32_t> integer(const cJSON* doc, const char* key, double maximum = 4294967295.0) {
  const auto value = number(doc, key, maximum);
  if (!value || std::floor(*value) != *value) return {};
  return static_cast<std::uint32_t>(*value);
}
bool flag(const cJSON* doc, const char* key) { return cJSON_IsTrue(field(doc, key)); }
PrusaLinkError response_error(const PrusaLinkHttpResponse& response) {
  if (response.error != PrusaLinkError::none) return response.error;
  if (response.status != 200 || (!response.content_encoding.empty() && response.content_encoding != "identity"))
    return PrusaLinkError::unsupported_response;
  return PrusaLinkError::none;
}
std::optional<core::ResinPrintSettings> print_settings(const cJSON* doc) {
  if (!flag(doc, "ok") || !flag(doc, "locked")) return {};
  const auto height = number(doc, "layerHeight", 1);
  const auto base = number(doc, "baseExposure", 3600), regular = number(doc, "regularExposure", 3600);
  const auto bottom = integer(doc, "baseLayers", 65535), transition = integer(doc, "transitionLayers", 65535);
  if (!height || *height <= 0 || !base || !regular || !bottom || !transition) return {};
  core::ResinPrintSettings result;
  result.layer_height_mm = *height; result.bottom_layers = *bottom; result.transition_layers = *transition;
  result.bottom.exposure_s = *base; result.normal.exposure_s = *regular;
  const auto slow = number(doc, "slowLiftDistance", 1000), fast = number(doc, "fastLiftDistance", 1000);
  const auto slow_rate = number(doc, "slowLiftFeedrate", 100000), fast_rate = number(doc, "fastLiftFeedrate", 100000);
  const auto drop_rate = number(doc, "dropBackFeedrate", 100000);
  for (auto* layer : {&result.bottom, &result.normal}) {
    if (slow) layer->lift_mm[0] = *slow;
    if (fast) layer->lift_mm[1] = *fast;
    if (slow && fast) layer->retract_mm[0] = *slow + *fast;
    // TinyMaker feedrates are mm/min; the protocol-neutral model uses mm/s.
    if (slow_rate) layer->lift_mm_s[0] = *slow_rate / 60;
    if (fast_rate) layer->lift_mm_s[1] = *fast_rate / 60;
    if (drop_rate) layer->retract_mm_s[0] = *drop_rate / 60;
  }
  return result;
}

}

bool tinymaker_identity(std::string_view body) {
  auto doc = parse(body);
  return doc && text(doc.get(), "text") == "Prusa SLA (TinyMaker)" &&
      text(doc.get(), "api") == "0.1" && text(doc.get(), "server").value_or("").size() > 0;
}

bool TinyMakerClient::configure(std::string_view endpoint, std::uint32_t profile_id, bool allow_loopback) {
  origin_.clear(); identity_ = {}; previous_.reset(); uptime_.reset(); last_sample_ms_ = 0;
  settings_.reset(); next_settings_attempt_ms_ = 0;
  const auto origin = prusalink_origin(endpoint, allow_loopback);
  if (!origin || !origin->starts_with("http://")) return false;
  origin_ = *origin; profile_id_ = profile_id;
  return true;
}

bool TinyMakerClient::identify(std::string_view version) {
  if (!tinymaker_identity(version)) return false;
  identity_.model = "TinyMaker";
  identity_.api_version = "0.1";
  return true;
}

PrusaLinkPollResult TinyMakerClient::poll(std::uint64_t deadline, const std::function<bool()>& cancelled) {
  if (origin_.empty()) return {.error = PrusaLinkError::invalid_configuration};
  const auto stopped = [&] { return (cancelled && cancelled()) || now_ms_() >= deadline; };
  const auto get = [&](const char* path) {
    if (stopped()) return PrusaLinkHttpResponse{.error = cancelled && cancelled() ? PrusaLinkError::cancelled : PrusaLinkError::timeout};
    return transport_.get({.url = origin_ + path, .maximum_body = 16384, .deadline_ms = deadline}, cancelled);
  };
  if (identity_.model.empty()) {
    const auto response = get("/api/version");
    if (const auto error = response_error(response); error != PrusaLinkError::none) return {.error = error};
    if (!identify(response.body)) return {.error = PrusaLinkError::unsupported_response};
    // /api/version.server is the emulated SLA server, not the installed firmware.
  }
  // Only the selected live client reaches a second active sample. Discovery
  // and inactive one-shot probes stay at identity + status. Config is locked
  // during printing; retain only print parameters, never integration settings.
  const bool previous_active = previous_ && previous_->snapshot.job.total_layers > 0 &&
      (previous_->snapshot.job.phase == core::JobPhase::printing ||
       previous_->snapshot.job.phase == core::JobPhase::preparing ||
       previous_->snapshot.job.phase == core::JobPhase::paused);
  if (previous_active && !settings_ && now_ms_() >= next_settings_attempt_ms_ && !stopped()) {
    next_settings_attempt_ms_ = now_ms_() + 30000;
    const auto config = transport_.get({.url = origin_ + "/api/config", .maximum_body = 16384,
        .deadline_ms = std::min(deadline, now_ms_() + 1500)}, cancelled);
    if (response_error(config) == PrusaLinkError::none) {
      const auto document = parse(config.body);
      if (document) settings_ = print_settings(document.get());
    }
  }
  const auto response = get("/api/status");
  if (const auto error = response_error(response); error != PrusaLinkError::none) return {.error = error};
  if (stopped()) return {.error = cancelled && cancelled() ? PrusaLinkError::cancelled : PrusaLinkError::timeout};
  auto doc = parse(response.body);
  const auto* root = doc.get();
  const auto state = integer(root, "stateCode", 255);
  const auto current = integer(root, "currentLayer", 65535), total = integer(root, "totalLayers", 65535);
  const auto version = text(root, "firmwareVersion", 64), model = text(root, "model", 256);
  if (!doc || !flag(root, "ok") || !state || !current || !total || *current > *total ||
      !version || version->empty() || !model || !cJSON_IsBool(field(root, "busy")) ||
      !cJSON_IsBool(field(root, "paused"))) return {.error = PrusaLinkError::unsupported_response};
  for (const char* key : {"pausing", "resuming", "stopping", "vatLow", "dryRun", "zKnown", "receiving"})
    if (field(root, key) && !cJSON_IsBool(field(root, key))) return {.error = PrusaLinkError::unsupported_response};
  for (const char* key : {"sdJob", "state"})
    if (field(root, key) && !text(root, key)) return {.error = PrusaLinkError::unsupported_response};
  const auto now = now_ms_();
  const auto uptime = integer(root, "uptimeSecs");
  if ((uptime_ && uptime && *uptime < *uptime_) || (previous_ && now - last_sample_ms_ > 20000)) {
    previous_.reset(); settings_.reset(); next_settings_attempt_ms_ = 0;
  }
  uptime_ = uptime; last_sample_ms_ = now;
  identity_.firmware_version = *version;
  PrusaLinkSample sample;
  auto& snapshot = sample.snapshot;
  snapshot.profile_id = profile_id_; snapshot.link = core::LinkState::online; snapshot.updated_at_ms = now;
  auto& job = snapshot.job;
  job.reachable = true;
  job.phase = core::JobPhase::idle; job.resin_stage = core::ResinStage::standby;
  const bool sd_job = !text(root, "sdJob", 32).value_or("").empty();
  const bool active = flag(root, "busy") && !sd_job && *total > 0 && !model->empty();
  if (!active || !previous_ || previous_->snapshot.job.gcode_file != *model || previous_->snapshot.job.total_layers != *total) {
    settings_.reset(); next_settings_attempt_ms_ = 0;
  }
  if (active) {
    if (settings_ && settings_->layer_height_mm && number(root, "layerHeight", 1) &&
        std::abs(*settings_->layer_height_mm - *number(root, "layerHeight", 1)) < 0.0001)
      job.resin_settings = *settings_;
    job.name = *model; job.gcode_file = *model;
    job.current_layer = *current; job.total_layers = *total;
    job.completion = 100.0F * static_cast<float>(*current) / *total; job.completion_known = true;
    if (const auto value = integer(root, "runSecs")) { job.elapsed_seconds = *value; job.elapsed_known = true; }
    if (const auto value = integer(root, "remainingSecs")) { job.remaining_seconds = *value; job.remaining_known = true; }
    job.phase = core::JobPhase::printing;
    switch (*state) {
      case 0: job.phase = core::JobPhase::preparing; job.resin_stage = core::ResinStage::homing; break;
      case 1: job.resin_stage = core::ResinStage::exposing; break;
      case 2: job.resin_stage = core::ResinStage::lifting; break;
      case 3: job.resin_stage = core::ResinStage::lowering; break;
      case 4: job.resin_stage = core::ResinStage::stopping; break;
      case 5: job.resin_stage = core::ResinStage::pausing; break;
      case 6: case 10: job.phase = core::JobPhase::paused; job.resin_stage = core::ResinStage::paused; break;
      case 7: job.phase = core::JobPhase::preparing; job.resin_stage = core::ResinStage::lowering; break;
      case 8: job.resin_stage = core::ResinStage::finishing; break;
      default: job.phase = core::JobPhase::unknown; job.resin_stage = core::ResinStage::unknown; break;
    }
    if (flag(root, "stopping")) job.resin_stage = core::ResinStage::stopping;
    // TinyMaker keeps paused=true during state 7's physical resume travel.
    else if (flag(root, "paused") && *state != 7) { job.phase = core::JobPhase::paused; job.resin_stage = core::ResinStage::paused; }
    if (flag(root, "vatLow") || *state == 10) job.condition = core::PrinterCondition::attention;
    if (flag(root, "dryRun")) job.kind = core::JobKind::calibration;
    if (const auto value = number(root, "layerHeight", 1)) job.resin_settings.layer_height_mm = *value;
    if (flag(root, "zKnown")) if (const auto value = number(root, "zMm", 1000)) { job.motion.z_known = true; job.motion.z_mm = *value; }
    if (job.resin_stage == core::ResinStage::exposing) {
      const auto duration = integer(root, "phaseTotalMs", 3600000), elapsed = integer(root, "phaseElapsedMs", 3600000);
      // Anchor the remaining interval locally, also when joining an exposure
      // that started before PrintDeck booted. A late stage transition stays at
      // zero until the printer reports lifting instead of dropping the timer.
      if (duration && elapsed && *duration > 0)
        job.resin_exposure = core::ResinExposureTiming{
            now, *duration - std::min(*duration, *elapsed), true};
    }
  } else if (sd_job || flag(root, "receiving")) {
    job.condition = core::PrinterCondition::busy;
    job.resin_stage = core::ResinStage::transferring_file;
  } else if (previous_ && !flag(root, "busy")) {
    const auto& before = previous_->snapshot.job;
    // Idle clears all job fields. Preserve a terminal outcome only with observed
    // evidence; a disconnect, reboot or missed cancellation is not completion.
    const bool stopped_before = before.resin_stage == core::ResinStage::stopping || before.phase == core::JobPhase::cancelled;
    const bool finished_before = before.resin_stage == core::ResinStage::finishing || before.phase == core::JobPhase::completed;
    if (stopped_before || finished_before) {
      job = before;
      job.phase = stopped_before ? core::JobPhase::cancelled : core::JobPhase::completed;
      job.resin_stage = stopped_before ? core::ResinStage::stopped : core::ResinStage::completed;
      job.remaining_seconds = 0; job.remaining_known = true; job.resin_exposure.reset();
      if (finished_before) { job.completion = 100; job.completion_known = true; job.current_layer = job.total_layers; }
    }
  }
  if (const auto value = number(root, "vatRemainingMl", 10000)) job.resin_telemetry.vat_remaining_ml = *value;
  if (field(root, "vatLow")) job.resin_telemetry.vat_low = flag(root, "vatLow");
  if (active) if (const auto value = number(root, "resinUsedMl", 10000)) job.resin_telemetry.used_ml = *value;
  job.normalize();
  previous_ = std::make_unique<PrusaLinkSample>(sample);
  return {.sample = std::move(sample)};
}
}  // namespace printdeck::platform
