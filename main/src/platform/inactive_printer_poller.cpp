#include "printdeck/platform/inactive_printer_poller.hpp"
#include "printdeck/platform/printer_address_recovery.hpp"
#include "printdeck/platform/prusalink_service.hpp"
#include "printdeck/platform/tinymaker_service.hpp"
#include "printdeck/platform/octoprint_service.hpp"
#include "printdeck/platform/elegoo_sdcp_service.hpp"
#include "printdeck/platform/uniformation_sdcp_parser.hpp"
#include "printdeck/platform/elegoo_cc2_service.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "printdeck/platform/bambu_trust.hpp"
#include "printdeck/platform/moonraker_status_parser.hpp"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {
namespace {

constexpr char kTag[] = "inactive_printers";
constexpr std::size_t kMaximumResponseBytes = 16 * 1024;
constexpr int kRequestTimeoutMs = 3000;
constexpr std::size_t kMaximumBambuReportBytes = 96 * 1024;
constexpr char kBambuRequestVersion[] =
    "{\"info\":{\"sequence_id\":\"1\",\"command\":\"get_version\"}}";
constexpr char kBambuStartReports[] =
    "{\"pushing\":{\"sequence_id\":\"2\",\"command\":\"start\"}}";
constexpr char kBambuRequestAll[] =
    "{\"pushing\":{\"sequence_id\":\"3\",\"command\":\"pushall\"}}";

struct JsonDeleter {
  void operator()(cJSON* value) const { cJSON_Delete(value); }
};
using JsonDocument = std::unique_ptr<cJSON, JsonDeleter>;

struct ResponseBuffer {
  std::string body;
  bool overflow = false;
};

esp_err_t response_event(esp_http_client_event_t* event) {
  if (event == nullptr || event->event_id != HTTP_EVENT_ON_DATA ||
      event->user_data == nullptr || event->data == nullptr || event->data_len <= 0) {
    return ESP_OK;
  }
  auto* response = static_cast<ResponseBuffer*>(event->user_data);
  const std::size_t bytes = static_cast<std::size_t>(event->data_len);
  if (response->body.size() + bytes > kMaximumResponseBytes) {
    response->overflow = true;
    return ESP_FAIL;
  }
  response->body.append(static_cast<const char*>(event->data), bytes);
  return ESP_OK;
}

std::string base_url(std::string endpoint) {
  while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
  if (endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) {
    endpoint.insert(0, "http://");
  }
  return endpoint;
}

const cJSON* member(const cJSON* object, const char* key) {
  return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

std::string string_member(const cJSON* object, const char* key) {
  const cJSON* value = member(object, key);
  return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : "";
}

double number_member(const cJSON* object, const char* key, double fallback = 0.0) {
  const cJSON* value = member(object, key);
  return cJSON_IsNumber(value) && std::isfinite(value->valuedouble)
             ? value->valuedouble
             : fallback;
}

std::string display_job_name(std::string path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash != std::string::npos) path.erase(0, slash + 1);
  constexpr char extension[] = ".gcode";
  if (path.size() >= sizeof(extension) - 1 &&
      path.compare(path.size() - (sizeof(extension) - 1), sizeof(extension) - 1,
                   extension) == 0) {
    path.resize(path.size() - (sizeof(extension) - 1));
  }
  return path;
}

bool is_bambu_calibration_job(std::string_view name, std::string_view gcode_file) {
  constexpr std::string_view prefix = "auto_cali_for_";
  const auto matches = [prefix](std::string_view value) {
    const std::size_t slash = value.find_last_of("/\\");
    if (slash != std::string_view::npos) value.remove_prefix(slash + 1);
    return value.rfind(prefix, 0) == 0;
  };
  return matches(name) || matches(gcode_file);
}

core::JobPhase bambu_phase(const std::string& state) {
  if (state == "RUNNING") return core::JobPhase::printing;
  if (state == "PAUSE" || state == "PAUSED") return core::JobPhase::paused;
  if (state == "FINISH") return core::JobPhase::completed;
  if (state == "FAILED" || state == "OFFLINE") return core::JobPhase::failed;
  if (state == "PREPARE" || state == "SLICING" || state == "INIT") {
    return core::JobPhase::preparing;
  }
  if (state == "IDLE") return core::JobPhase::idle;
  return core::JobPhase::unknown;
}

struct BambuProbeContext {
  TaskHandle_t owner = nullptr;
  esp_mqtt_client_handle_t client = nullptr;
  std::string report_topic;
  std::string request_topic;
  std::string incoming_topic;
  std::vector<char> incoming_payload;
  std::vector<char> complete_payload;
  std::mutex payload_mutex;
  bool failed = false;
};

void bambu_probe_event(void* context, esp_event_base_t, std::int32_t,
                       void* event_data) {
  auto* probe = static_cast<BambuProbeContext*>(context);
  auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
  if (probe == nullptr || event == nullptr) return;
  switch (static_cast<esp_mqtt_event_id_t>(event->event_id)) {
    case MQTT_EVENT_CONNECTED:
      esp_mqtt_client_subscribe(probe->client, probe->report_topic.c_str(), 0);
      break;
    case MQTT_EVENT_SUBSCRIBED:
      esp_mqtt_client_publish(probe->client, probe->request_topic.c_str(),
                              kBambuRequestVersion, 0, 0, 0);
      esp_mqtt_client_publish(probe->client, probe->request_topic.c_str(),
                              kBambuStartReports, 0, 0, 0);
      esp_mqtt_client_publish(probe->client, probe->request_topic.c_str(),
                              kBambuRequestAll, 0, 0, 0);
      break;
    case MQTT_EVENT_ERROR:
      {
        const std::lock_guard<std::mutex> lock(probe->payload_mutex);
        probe->failed = true;
      }
      xTaskNotifyGive(probe->owner);
      break;
    case MQTT_EVENT_DATA: {
      if (event->data_len <= 0 || event->current_data_offset < 0 || !event->data ||
          (event->current_data_offset == 0 && (!event->topic || event->topic_len <= 0)) ||
          event->total_data_len <= 0 ||
          static_cast<std::size_t>(event->total_data_len) > kMaximumBambuReportBytes) {
        break;
      }
      const std::lock_guard<std::mutex> lock(probe->payload_mutex);
      if (event->current_data_offset == 0) {
        probe->incoming_topic.assign(event->topic, event->topic_len);
        probe->incoming_payload.assign(static_cast<std::size_t>(event->total_data_len), '\0');
      }
      const std::size_t offset = static_cast<std::size_t>(event->current_data_offset);
      const std::size_t length = static_cast<std::size_t>(event->data_len);
      if (offset + length > probe->incoming_payload.size()) {
        probe->incoming_payload.clear();
        probe->incoming_topic.clear();
        break;
      }
      std::memcpy(probe->incoming_payload.data() + offset, event->data, length);
      if (offset + length == probe->incoming_payload.size() &&
          probe->incoming_topic == probe->report_topic) {
        probe->complete_payload.swap(probe->incoming_payload);
        probe->incoming_topic.clear();
        xTaskNotifyGive(probe->owner);
      }
      break;
    }
    default: break;
  }
}

}  // namespace

esp_err_t InactivePrinterPoller::start(const core::DeviceSettings& settings,
                                       const NetworkService& network) {
  if (task_ != nullptr) return ESP_ERR_INVALID_STATE;
  network_ = &network;
  configure(settings);
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "inactive_printers", 49152, this, 2,
                                      &task_, kServiceCore,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    task_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void InactivePrinterPoller::configure(const core::DeviceSettings& settings) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto previous_profile = [this](std::uint32_t profile_id) {
      return std::find_if(
          profiles_.begin(), profiles_.end(),
          [profile_id](const core::PrinterProfile& profile) {
            return profile.id == profile_id;
          });
    };
    const auto next_profile = [&settings](std::uint32_t profile_id) {
      return std::find_if(
          settings.profiles.begin(), settings.profiles.end(),
          [profile_id](const core::PrinterProfile& profile) {
            return profile.id == profile_id;
          });
    };
    const auto invalidated = [&](std::uint32_t profile_id) {
      const auto previous = previous_profile(profile_id);
      const auto next = next_profile(profile_id);
      return previous == profiles_.end() || next == settings.profiles.end() ||
             !core::same_printer_connection(*previous, *next);
    };
    check_attempts_.erase(
        std::remove_if(check_attempts_.begin(), check_attempts_.end(),
                       [&invalidated](const CheckAttempt& attempt) {
                         return invalidated(attempt.profile_id);
                       }),
        check_attempts_.end());
    for (auto& attempt : check_attempts_) attempt.in_progress = false;
    snapshot_.printers.erase(
        std::remove_if(snapshot_.printers.begin(), snapshot_.printers.end(),
                       [&invalidated](const InactivePrinterStatus& status) {
                         return invalidated(status.profile_id);
                       }),
        snapshot_.printers.end());
    profiles_ = settings.profiles;
    if (active_profile_ != 0 &&
        std::none_of(profiles_.begin(), profiles_.end(), [this](const auto& profile) {
          return profile.id == active_profile_;
        })) {
      active_profile_ = 0;
    }
    interval_s_ = settings.inactive_printer_poll_interval_s;
    ++config_generation_;
    ++snapshot_.revision;
  }
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

void InactivePrinterPoller::set_active_profile(std::uint32_t profile_id) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (profile_id != 0 &&
        std::none_of(profiles_.begin(), profiles_.end(), [profile_id](const auto& profile) {
          return profile.id == profile_id;
        })) {
      profile_id = 0;
    }
    if (active_profile_ == profile_id) return;
    active_profile_ = profile_id;
    ++config_generation_;
    ++snapshot_.revision;
  }
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

void InactivePrinterPoller::mark_offline(std::uint32_t profile_id) {
  if (profile_id == 0) return;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto found = std::find_if(snapshot_.printers.begin(), snapshot_.printers.end(),
                              [profile_id](const InactivePrinterStatus& status) {
                                return status.profile_id == profile_id;
                              });
    InactivePrinterStatus offline;
    offline.profile_id = profile_id;
    offline.available = true;
    offline.connected = false;
    offline.updated_at_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    auto attempt = std::find_if(check_attempts_.begin(), check_attempts_.end(),
        [profile_id](const auto& value) { return value.profile_id == profile_id; });
    if (attempt == check_attempts_.end()) {
      check_attempts_.push_back({.profile_id = profile_id});
      attempt = std::prev(check_attempts_.end());
    }
    attempt->recovery.failures = 2;
    if (found == snapshot_.printers.end()) snapshot_.printers.push_back(offline);
    else *found = offline;
    ++snapshot_.revision;
  }
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

InactivePrinterSnapshot InactivePrinterPoller::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

bool InactivePrinterPoller::check_in_progress(std::uint32_t profile_id) const {
  if (probing_profile_.load() == profile_id) return true;
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto attempt = std::find_if(
      check_attempts_.begin(), check_attempts_.end(),
      [profile_id](const CheckAttempt& candidate) {
        return candidate.profile_id == profile_id;
      });
  return attempt != check_attempts_.end() && attempt->in_progress;
}

bool InactivePrinterPoller::begin_automatic_check(
    std::uint32_t profile_id, std::uint32_t generation, std::uint64_t now_ms) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (generation != config_generation_ || active_profile_ == profile_id) {
    return false;
  }
  auto attempt = std::find_if(
      check_attempts_.begin(), check_attempts_.end(),
      [profile_id](const CheckAttempt& candidate) {
        return candidate.profile_id == profile_id;
      });
  if (attempt == check_attempts_.end()) {
    check_attempts_.push_back({.profile_id = profile_id});
    attempt = std::prev(check_attempts_.end());
  }
  if (attempt->in_progress ||
      !core::printer_check_allowed(
          now_ms, attempt->started_at_ms, std::max<std::uint64_t>(kMinimumCheckSpacingMs, interval_s_ * 1000ULL))) {
    return false;
  }
  attempt->started_at_ms = now_ms;
  attempt->in_progress = true;
  return true;
}

void InactivePrinterPoller::finish_automatic_check(
    std::uint32_t profile_id, std::uint64_t started_at_ms) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto attempt = std::find_if(
      check_attempts_.begin(), check_attempts_.end(),
      [profile_id](const CheckAttempt& candidate) {
        return candidate.profile_id == profile_id;
      });
  if (attempt != check_attempts_.end() &&
      attempt->started_at_ms == started_at_ms) {
    attempt->in_progress = false;
  }
}

void InactivePrinterPoller::publish_automatic_result(
    std::uint32_t generation, InactivePrinterStatus result) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (generation != config_generation_ || result.profile_id == active_profile_) return;
  const bool profile_still_exists = std::any_of(
      profiles_.begin(), profiles_.end(),
      [&result](const core::PrinterProfile& profile) {
        return profile.id == result.profile_id;
      });
  if (!profile_still_exists) return;
  const auto attempt = std::find_if(
      check_attempts_.begin(), check_attempts_.end(),
      [&result](const CheckAttempt& candidate) {
        return candidate.profile_id == result.profile_id;
      });
  if (attempt == check_attempts_.end()) return;
  const core::AutomaticProbeDecision decision = core::automatic_probe_decision(
      result.connected, attempt->consecutive_failures);
  attempt->consecutive_failures = decision.consecutive_failures;
  if (!decision.publish) {
    ESP_LOGW(kTag,
             "Printer status probe failed once; retaining the previous reachability");
    return;
  }
  auto status = std::find_if(
      snapshot_.printers.begin(), snapshot_.printers.end(),
      [&result](const InactivePrinterStatus& candidate) {
        return candidate.profile_id == result.profile_id;
      });
  if (status == snapshot_.printers.end()) snapshot_.printers.push_back(std::move(result));
  else *status = std::move(result);
  ++snapshot_.revision;
}

void InactivePrinterPoller::task_entry(void* context) {
  static_cast<InactivePrinterPoller*>(context)->task_loop();
}

void InactivePrinterPoller::finish_recovery() {
  if (!recovery_run_ || !discovery_) return;
  const auto run = *recovery_run_;
  const auto current = network_->status();
  bool stale = !recovery_allowed_.load() || !current.station_connected || current.recovery_ap_active ||
      current.station_name != run.network.station_name || current.ipv4 != run.network.ipv4 ||
      current.netmask != run.network.netmask;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stale |= run.generation != config_generation_ || active_profile_ == run.profile.id;
  }
  const auto result = discovery_->snapshot(true);
  if (stale || result.scan_id != run.scan_id) {
    discovery_->cancel(run.scan_id);
    recovery_run_.reset();
    return;
  }
  if (discovery_->running()) return;
  recovery_run_.reset();
  next_discovery_ms_ = esp_timer_get_time() / 1000 + 30000;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& attempt : check_attempts_) {
      if (attempt.profile_id != run.profile.id) continue;
      if (result.state == PrinterDiscoveryState::complete) attempt.recovery_offset += 32;
      // Backoff starts after the scan, so a slow pass cannot immediately trigger the next one.
      const auto delay = std::min<std::uint64_t>(30000ULL << std::min(attempt.recovery.attempts ? attempt.recovery.attempts - 1 : 0, 5U), 900000);
      attempt.recovery.retry_at_ms = esp_timer_get_time() / 1000 + delay;
    }
  }
  if (result.state != PrinterDiscoveryState::complete) return;
  core::UniquePrinterAddress matches;
  const auto address = core::printer_address(run.profile);
  if (!address) return;
  for (const auto& printer : result.printers) {
    if (!printer.seen_in_current_scan || printer.protocol != run.profile.protocol || printer.port != address->port) continue;
    const bool same = run.profile.protocol == core::PrinterProtocol::moonraker
        ? (printer.network_identity == run.profile.network_identity ||
           core::moonraker_host_evidence_matches(printer.network_identity, run.profile.network_identity))
        : printer.serial == run.profile.serial;
    if (same) matches.observe(printer.host);
  }
  const auto host = matches.result();
  if (!host || *host == address->host) return;
  const auto candidate = core::printer_at_address(run.profile, *host);
  if (!candidate) return;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (run.generation != config_generation_ || active_profile_ == run.profile.id) return;
    for (auto& attempt : check_attempts_)
      if (attempt.profile_id == run.profile.id) attempt.in_progress = true;
  }
  const bool verified = probe(*candidate).connected;
  bool current_generation;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    current_generation = run.generation == config_generation_ && active_profile_ != run.profile.id;
  }
  if (verified && current_generation && recovery_allowed_.load())
    recovery_callback_(recovery_context_, run.profile, *candidate, run.network);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (run.generation == config_generation_)
      for (auto& attempt : check_attempts_)
        if (attempt.profile_id == run.profile.id) attempt.in_progress = false;
  }
}

void InactivePrinterPoller::recover_or_learn(const core::PrinterProfile& profile,
    std::uint32_t generation, bool connected, const NetworkStatus& network, bool active) {
  if (!recovery_callback_ || !discovery_ || !recovery_allowed_.load() || network.recovery_ap_active) return;
  const auto cancelled = [&] {
    if (!recovery_allowed_.load()) return true;
    const auto current = network_->status();
    if (!current.station_connected || current.recovery_ap_active ||
        current.station_name != network.station_name || current.ipv4 != network.ipv4 ||
        current.netmask != network.netmask) return true;
    const std::lock_guard<std::mutex> lock(mutex_);
    return generation != config_generation_ || (!active && active_profile_ == profile.id) ||
        (active && online_profile_.load() != profile.id);
  };
  const auto now = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  bool learn = false, discover = false;
  std::size_t offset = 0;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (generation != config_generation_) return;
    auto attempt = std::find_if(check_attempts_.begin(), check_attempts_.end(),
        [&](const auto& value) { return value.profile_id == profile.id; });
    if (attempt == check_attempts_.end()) {
      check_attempts_.push_back({.profile_id = profile.id});
      attempt = std::prev(check_attempts_.end());
    }
    if (connected) { attempt->recovery.observe(true); attempt->recovery_offset = 0; }
    if (connected && profile.protocol == core::PrinterProtocol::moonraker &&
        (profile.network_identity.empty() || profile.network_identity.starts_with("mr:") ||
         core::valid_moonraker_uuid(profile.network_identity)) &&
        now >= attempt->learn_after_ms && !discovery_->running()) {
      learn = true;
      attempt->learn_after_ms = now + 300000;
    } else if (!active && core::printer_address_recoverable(profile) && attempt->recovery.due(now) &&
               now >= next_discovery_ms_ && !recovery_run_ && !discovery_->running()) {
      discover = true;
      offset = attempt->recovery_offset;

      attempt->recovery.attempted(now);
      next_discovery_ms_ = now + 10000;
    }
  }
  if (learn) {
    const auto learned = learn_moonraker_identity(profile, cancelled);
    const bool interrupted = cancelled();
    const bool queued = learned && !interrupted && recovery_callback_(recovery_context_, profile, *learned, network);
    if (!queued) {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (auto& attempt : check_attempts_)
        if (attempt.profile_id == profile.id && (interrupted || learned))
          attempt.learn_after_ms = esp_timer_get_time() / 1000 + 5000;
      if (!interrupted && !learned)
        ESP_LOGI(kTag, "Printer identity not available yet; learning will retry later");
    }
  } else if (discover && !cancelled()) {
    core::DeviceSettings settings;
    settings.profiles = {profile};
    if (discovery_->start(network, settings, {}, 0, profile, 0, offset) == ESP_OK)
      recovery_run_ = RecoveryRun{profile, network, generation, discovery_->snapshot(true).scan_id};
  }
}

void InactivePrinterPoller::reconcile_manual_search(const std::vector<core::PrinterProfile>& profiles,
    std::uint32_t generation, const NetworkStatus& network) {
  if (!discovery_ || discovery_->running() || recovery_run_ || !recovery_allowed_.load()) return;
  const auto result = discovery_->snapshot();
  if (!result.scan_id || result.state != PrinterDiscoveryState::complete ||
      result.network_name != network.station_name || result.network_ipv4 != network.ipv4 ||
      result.network_netmask != network.netmask) return;
  if (manual_scan_id_ != result.scan_id) {
    manual_scan_id_ = result.scan_id;
    reconciled_profiles_.clear();
  }
  for (const auto& profile : profiles) {
    if (!core::printer_address_recoverable(profile) ||
        std::find(reconciled_profiles_.begin(), reconciled_profiles_.end(), profile.id) != reconciled_profiles_.end()) continue;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (generation != config_generation_) return;
      if (profile.id == active_profile_) continue;
    }
    const auto address = core::printer_address(profile);
    if (!address) continue;
    core::UniquePrinterAddress matches;
    for (const auto& printer : result.printers) {
      if (!printer.seen_in_current_scan || printer.protocol != profile.protocol || printer.port != address->port) continue;
      const bool same = profile.protocol == core::PrinterProtocol::moonraker
          ? (printer.network_identity == profile.network_identity ||
             core::moonraker_host_evidence_matches(printer.network_identity, profile.network_identity))
          : printer.serial == profile.serial;
      if (same) matches.observe(printer.host);
    }
    const auto host = matches.result();
    reconciled_profiles_.push_back(profile.id);
    if (!host || *host == address->host) continue;
    const auto candidate = core::printer_at_address(profile, *host);
    if (!candidate) continue;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (generation != config_generation_ || active_profile_ == profile.id) return;
      auto attempt = std::find_if(check_attempts_.begin(), check_attempts_.end(),
          [&](const auto& value) { return value.profile_id == profile.id; });
      if (attempt == check_attempts_.end()) {
        check_attempts_.push_back({.profile_id = profile.id});
        attempt = std::prev(check_attempts_.end());
      }
      attempt->in_progress = true;
    }
    if (probe(*candidate).connected) {
      bool valid;
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        valid = generation == config_generation_ && active_profile_ != profile.id;
      }
      if (valid && recovery_allowed_.load()) recovery_callback_(recovery_context_, profile, *candidate, network);
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (generation == config_generation_)
        for (auto& attempt : check_attempts_)
          if (attempt.profile_id == profile.id) attempt.in_progress = false;
    }
    // At most one connection verification in a worker turn.
    return;
  }
}

void InactivePrinterPoller::task_loop() {
  std::size_t turn = 0;
  while (true) {
    finish_recovery();
    std::vector<core::PrinterProfile> profiles;
    std::uint32_t selected, interval, generation;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      profiles = profiles_;
      selected = active_profile_;
      interval = interval_s_;
      generation = config_generation_;
    }
    const auto network = network_->status();
    if (network.station_connected && !profiles.empty()) {
      reconcile_manual_search(profiles, generation, network);
      const auto offset = turn++ % profiles.size();
      std::rotate(profiles.begin(), profiles.begin() + offset, profiles.end());
      for (const auto& profile : profiles) {
        if (profile.id == selected) {
          if (online_profile_.load() == profile.id)
            recover_or_learn(profile, generation, true, network, true);
          continue;
        }
        const auto now = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
        bool connected = false;
        if (interval != 0 && begin_automatic_check(profile.id, generation, now)) {
          auto result = probe(profile);
          result.updated_at_ms = esp_timer_get_time() / 1000;
          connected = result.connected;
          {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (generation == config_generation_ && result.available)
              for (auto& attempt : check_attempts_)
                if (attempt.profile_id == profile.id) attempt.recovery.observe(connected);
          }
          finish_automatic_check(profile.id, now);
          publish_automatic_result(generation, std::move(result));
        }
        recover_or_learn(profile, generation, connected, network);
      }
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
}

InactivePrinterStatus InactivePrinterPoller::probe(
    const core::PrinterProfile& profile) const {
  struct ProbeGuard {
    std::atomic<std::uint32_t>& current;
    ~ProbeGuard() { current.store(0); }
  } guard{probing_profile_};
  probing_profile_.store(profile.id);
  InactivePrinterStatus summary;
  summary.profile_id = profile.id;
  summary.available = true;
  if (profile.protocol == core::PrinterProtocol::elegoo_sdcp ||
      profile.protocol == core::PrinterProtocol::elegoo_cc2 || profile.protocol == core::PrinterProtocol::uniformation_sdcp) {
    const auto cancelled = [&] {
      if (!network_ || !network_->status().station_connected) return true;
      const std::lock_guard<std::mutex> lock(mutex_);
      return active_profile_ == profile.id ||
          std::none_of(profiles_.begin(), profiles_.end(),
              [&](const auto& value) { auto expected = profile; expected.endpoint = value.endpoint; return core::same_printer_connection(value, expected); });
    };
    const auto deadline = static_cast<std::uint64_t>(esp_timer_get_time() / 1000) + 8000;
    const auto result = profile.protocol == core::PrinterProtocol::uniformation_sdcp
        ? uniformation_sdcp_probe(profile, deadline, cancelled)
        : profile.protocol == core::PrinterProtocol::elegoo_sdcp
        ? elegoo_sdcp_probe(profile, deadline, cancelled)
        : elegoo_cc2_probe(profile, deadline, cancelled);
    if (cancelled()) { summary.available = false; return summary; }
    if (result.snapshot && result.snapshot->link == core::LinkState::online) {
      const auto& job = result.snapshot->job;
      summary.connected = true;
      summary.job_name = job.name;
      summary.phase = job.phase;
      summary.kind = job.kind;
      summary.condition = job.condition;
      summary.completion = job.completion;
      summary.completion_known = job.completion_known;
      summary.elapsed_seconds = job.elapsed_seconds;
      summary.elapsed_known = job.elapsed_known;
      summary.remaining_seconds = job.remaining_seconds;
      summary.remaining_known = job.remaining_known;
    } else if (result.error == ElegooError::capacity || result.error == ElegooError::service_not_ready) {
      summary.available = false;
    }
    return summary;
  }
  if (profile.protocol == core::PrinterProtocol::prusalink || profile.protocol == core::PrinterProtocol::tinymaker ||
      profile.protocol == core::PrinterProtocol::octoprint) {
    const auto cancelled = [&] {
      if (network_ == nullptr || !network_->status().station_connected) return true;
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto found = std::find_if(profiles_.begin(), profiles_.end(),
          [&](const auto& value) { return core::same_printer_connection(value, profile); });
      return active_profile_ == profile.id || interval_s_ == 0 || found == profiles_.end();
    };
    const auto result = profile.protocol == core::PrinterProtocol::octoprint
        ? octoprint_probe(profile, prusalink_now_ms() + 5000, cancelled)
        : profile.protocol == core::PrinterProtocol::tinymaker
        ? tinymaker_probe(profile, prusalink_now_ms() + 8000, cancelled)
        : prusalink_probe(profile, prusalink_now_ms() + 5000, cancelled);
    // One resource observation per boot, never an endpoint or credential.
    static bool reported_prusa_stack = false;
    if (result.sample && !reported_prusa_stack) {
      ESP_LOGI(kTag, "Prusa status probe stack high-water=%u",
               static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
      reported_prusa_stack = true;
    }
    if (result.sample && !cancelled()) {
      const auto& job = result.sample->snapshot.job;
      summary.connected = true;
      summary.phase = job.phase;
      summary.kind = job.kind;
      summary.condition = job.condition;
      summary.completion = job.completion;
      summary.completion_known = job.completion_known;
      summary.elapsed_seconds = job.elapsed_seconds;
      summary.elapsed_known = job.elapsed_known;
      summary.remaining_seconds = job.remaining_seconds;
      summary.remaining_known = job.remaining_known;
    }
    return summary;
  }
  if (profile.protocol == core::PrinterProtocol::bambu_lan) {
    BambuProbeContext context;
    context.owner = xTaskGetCurrentTaskHandle();
    context.report_topic = "device/" + profile.serial + "/report";
    context.request_topic = "device/" + profile.serial + "/request";
    char client_id[40]{};
    std::snprintf(client_id, sizeof(client_id), "printdeck-list-%08lx",
                  static_cast<unsigned long>(esp_random()));
    const std::string stable_client_id = client_id;
    esp_mqtt_client_config_t config{};
    config.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    config.broker.address.hostname = profile.endpoint.c_str();
    config.broker.address.port = 8883;
    config.broker.verification.certificate = bambu_trust_anchors();
    config.broker.verification.common_name = profile.serial.c_str();
    config.broker.verification.skip_cert_common_name_check = false;
    config.credentials.client_id = stable_client_id.c_str();
    config.credentials.username = "bblp";
    config.credentials.authentication.password = profile.access_code.c_str();
    config.session.keepalive = 10;
    config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    config.buffer.size = 16384;
    config.buffer.out_size = 2048;
    config.task.stack_size = 8192;
    config.network.timeout_ms = 6000;
    config.network.reconnect_timeout_ms = 6000;
    context.client = esp_mqtt_client_init(&config);
    if (context.client == nullptr) {
      ESP_LOGW(kTag,
               "Bambu status probe allocation failed; internal heap=%u, largest=%u",
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
      return summary;
    }
    esp_mqtt_client_register_event(context.client, MQTT_EVENT_ANY,
                                   bambu_probe_event, &context);
    std::vector<char> report;
    const esp_err_t start_result = esp_mqtt_client_start(context.client);
    if (start_result == ESP_OK) {
      const std::int64_t deadline_us = esp_timer_get_time() + 8'000'000;
      while (esp_timer_get_time() < deadline_us) {
        const std::int64_t remaining_ms =
            std::max<std::int64_t>(1, (deadline_us - esp_timer_get_time()) / 1000);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(static_cast<std::uint32_t>(std::min<std::int64_t>(remaining_ms, 250))));
        {
          // Device selection reserves this profile immediately. Stop the
          // lightweight probe before handing ownership to the full adapter,
          // so Bambu never sees two PrintDeck MQTT sessions from one device.
          const std::lock_guard<std::mutex> lock(mutex_);
          const bool exists = std::any_of(profiles_.begin(), profiles_.end(), [&](const auto& value) {
            auto expected = profile; expected.endpoint = value.endpoint;
            return core::same_printer_connection(value, expected);
          });
          if (active_profile_ == profile.id || !exists || !network_->status().station_connected) {
            report.clear(); break;
          }
        }
        {
          const std::lock_guard<std::mutex> lock(context.payload_mutex);
          if (context.failed) break;
          if (!context.complete_payload.empty()) report.swap(context.complete_payload);
        }
        if (report.empty()) continue;
        JsonDocument candidate(cJSON_ParseWithLength(report.data(), report.size()));
        if (candidate && cJSON_IsObject(member(candidate.get(), "print"))) break;
        report.clear();
      }
      esp_mqtt_client_stop(context.client);
    } else {
      ESP_LOGW(kTag,
               "Bambu status probe could not start: %s; internal heap=%u, largest=%u",
               esp_err_to_name(start_result),
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    }
    esp_mqtt_client_destroy(context.client);
    context.client = nullptr;
    if (context.failed || report.empty()) return summary;

    JsonDocument document(cJSON_ParseWithLength(report.data(), report.size()));
    const cJSON* print = member(document.get(), "print");
    if (!document || !cJSON_IsObject(print)) return summary;
    summary.connected = true;
    summary.phase = bambu_phase(string_member(print, "gcode_state"));
    summary.job_name = string_member(print, "subtask_name");
    const std::string gcode_file = string_member(print, "gcode_file");
    if (is_bambu_calibration_job(summary.job_name, gcode_file)) {
      summary.kind = core::JobKind::calibration;
    }
    if (summary.job_name.empty()) {
      summary.job_name = display_job_name(gcode_file);
    }
    summary.completion_known = cJSON_IsNumber(member(print, "mc_percent"));
    summary.completion = static_cast<float>(std::clamp(number_member(print, "mc_percent"), 0.0, 100.0));
    summary.remaining_seconds = static_cast<std::uint32_t>(std::max(
        0.0, number_member(print, "mc_remaining_time") * 60.0));
    summary.remaining_known = cJSON_IsNumber(member(print, "mc_remaining_time"));
    return summary;
  }

  if (profile.protocol != core::PrinterProtocol::moonraker) return summary;
  ResponseBuffer response;
  if (!moonraker_endpoint_identity_matches(profile)) return summary;
  const std::string url = base_url(profile.endpoint) +
      "/printer/objects/query?webhooks&virtual_sdcard&print_stats&display_status";
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = kRequestTimeoutMs;
  config.event_handler = response_event;
  config.user_data = &response;
  config.buffer_size = 1024;
  config.buffer_size_tx = 512;
  if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return summary;
  if (!profile.api_key.empty()) {
    esp_http_client_set_header(client, "X-Api-Key", profile.api_key.c_str());
  }
  esp_http_client_set_header(client, "Accept", "application/json");
  const esp_err_t result = esp_http_client_perform(client);
  const int status_code = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
  esp_http_client_cleanup(client);

  if (result != ESP_OK || response.overflow || status_code < 200 || status_code >= 300) {
    return summary;
  }
  JsonDocument document(cJSON_ParseWithLength(response.body.data(), response.body.size()));
  const cJSON* status = member(member(document.get(), "result"), "status");
  const cJSON* stats = member(status, "print_stats");
  if (!document || !cJSON_IsObject(status) || !cJSON_IsObject(stats)) return summary;

  summary.connected = true;
  summary.phase = moonraker_phase(string_member(stats, "state"));
  summary.job_name = display_job_name(string_member(stats, "filename"));
  const auto timing = moonraker_job_timing(status);
  summary.completion = timing.completion;
  summary.completion_known = timing.completion_known;
  summary.elapsed_seconds = timing.elapsed_seconds;
  summary.elapsed_known = timing.elapsed_known;
  summary.remaining_seconds = timing.remaining_seconds;
  summary.remaining_known = timing.remaining_known;
  return summary;
}

}  // namespace printdeck::platform
