#include "printdeck/platform/board.hpp"
#include "printdeck/platform/set_catalog_service.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include <array>
#include <cstdio>
#include <memory>
#include <sys/stat.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "printdeck/core/audio_pack.hpp"
#include "printdeck/platform/audio_set_service.hpp"
#include "freertos/idf_additions.h"

namespace printdeck::platform {
namespace {
std::atomic<bool> transfer_active{false};
constexpr std::size_t maximum_catalog = 64 * 1024;
const char* path(bool audio) {return audio ? "/assets/audio-catalog.json" : "/assets/reaction-catalog.json";}
std::uint64_t now_ms() {return esp_timer_get_time()/1000;}
bool read_cached(bool audio, std::string& body) {
  struct stat info{};
  if (stat(path(audio), &info) != 0 || info.st_size <= 0 || info.st_size > maximum_catalog) return false;
  std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path(audio), "rb"), std::fclose);
  if (!file) return false;
  body.resize(info.st_size);
  return std::fread(body.data(),1,body.size(),file.get()) == body.size();
}
bool save_cached(bool audio, const std::string& body) {
  std::size_t total=0,used=0;
  // Bounded catalog metadata is needed to bootstrap audio on existing devices.
  // Reserve admission applies to reaction images; metadata keeps the FS safety margin.
  constexpr std::size_t reserve=128*1024;
  if(esp_littlefs_info("assets",&total,&used)!=ESP_OK || used>total || body.size()+reserve>total-used) return false;
  const auto staging = std::string(path(audio)) + ".new";
  FILE* file=std::fopen(staging.c_str(),"wb");
  if (!file) return false;
  const bool written=std::fwrite(body.data(),1,body.size(),file)==body.size();
  const bool closed=std::fclose(file)==0;
  if (!written || !closed || std::rename(staging.c_str(),path(audio)) != 0) {
    std::remove(staging.c_str());return false;
  }
  return true;
}
struct Download {
  std::string body;
  const std::atomic<bool>* cancelled;
  std::uint64_t deadline;
};
esp_err_t receive(esp_http_client_event_t* event) {
  auto& data=*static_cast<Download*>(event->user_data);
  if (data.cancelled->load() || now_ms() >= data.deadline) return ESP_FAIL;
  if (event->event_id == HTTP_EVENT_ON_DATA) {
    if (event->data_len < 0 || static_cast<std::size_t>(event->data_len) > maximum_catalog-data.body.size()) return ESP_FAIL;
    data.body.append(static_cast<const char*>(event->data),event->data_len);
  }
  return ESP_OK;
}
bool download(bool audio, const std::atomic<bool>& cancelled, std::string& body) {
  Download data{{}, &cancelled, now_ms()+15000};
  const std::string url=std::string(audio ? kAudioSetOrigin : kReactionSetOrigin)+"catalog.json";
  esp_http_client_config_t config{};
  config.url=url.c_str();config.crt_bundle_attach=esp_crt_bundle_attach;
  config.timeout_ms=3000;config.disable_auto_redirect=true;
  config.buffer_size=2048;config.buffer_size_tx=512;
  config.event_handler=receive;config.user_data=&data;
  esp_http_client_handle_t client=esp_http_client_init(&config);
  if (!client) return false;
  const auto result=esp_http_client_perform(client);
  const bool valid=result==ESP_OK && esp_http_client_get_status_code(client)==200 &&
      esp_http_client_is_complete_data_received(client) && !cancelled.load() && now_ms()<data.deadline;
  esp_http_client_cleanup(client);
  if (valid) body=std::move(data.body);
  return valid;
}
}
AssetTransferLease::AssetTransferLease() : acquired_(!transfer_active.exchange(true)) {}
AssetTransferLease::~AssetTransferLease() {if(acquired_) transfer_active.store(false);}
SetCatalogService& SetCatalogService::instance() {static SetCatalogService service;return service;}
void SetCatalogService::start(const NetworkService& network) {
  network_=&network;load(false);if(kBoardHasAudio)load(true);requested_.store(true);next_refresh_ms_=now_ms()+15000;
}
void SetCatalogService::load(bool audio) {
  std::string body;std::vector<core::DownloadableSet> parsed;
  if (!read_cached(audio,body) || !core::parse_set_catalog(body,audio,parsed)) return;
  const std::lock_guard lock(mutex_);(audio ? audio_ : reactions_)=std::move(parsed);
}
void SetCatalogService::request_refresh() {requested_.store(true);}
void SetCatalogService::cancel() {cancelled_.store(true);requested_.store(false);}
std::vector<core::DownloadableSet> SetCatalogService::sets(bool audio) const {
  const std::lock_guard lock(mutex_);return audio ? audio_ : reactions_;
}
std::string SetCatalogService::error() const {const std::lock_guard lock(mutex_);return error_;}
void SetCatalogService::poll() {
  if(finished_.load() && task_ && eTaskGetState(task_)==eSuspended) {finished_.store(false);vTaskDeleteWithCaps(task_);task_=nullptr;running_.store(false);}
  if (!network_ || running_.load() || now_ms()<next_refresh_ms_ || !network_->status().station_connected) return;
  if (!requested_.exchange(false)) return;
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<32*1024 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<12*1024) {
    requested_.store(true);next_refresh_ms_=now_ms()+5000;return;
  }
  cancelled_.store(false);running_.store(true);
  next_refresh_ms_=now_ms()+60000;
  if (xTaskCreatePinnedToCoreWithCaps(task_entry,"set_catalog",6144,this,2,&task_,kServiceCore,
      MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
    running_.store(false);requested_.store(true);
  }
}
void SetCatalogService::task_entry(void* context) {
  auto& self=*static_cast<SetCatalogService*>(context);
  self.refresh();self.finished_.store(true);vTaskSuspend(nullptr);
}
void SetCatalogService::refresh() {
  AssetTransferLease lease;
  if (!lease) {requested_.store(true);return;}
  {const std::lock_guard lock(mutex_);error_.clear();}
  for (bool audio : {false,true}) {
    if(audio && !kBoardHasAudio)continue;
    if (cancelled_.load()) return;
    std::string body;std::vector<core::DownloadableSet> parsed;
    const bool downloaded=download(audio,cancelled_,body);
    const bool valid=downloaded && core::parse_set_catalog(body,audio,parsed);
    const bool saved=valid && save_cached(audio,body);
    if (!saved) {
      ESP_LOGW("set_catalog", "%s refresh incomplete: download=%d valid=%d bytes=%u", audio?"audio":"reaction", downloaded,valid,static_cast<unsigned>(body.size()));
      const std::lock_guard lock(mutex_);error_="catalog_unavailable";continue;
    }
    const std::lock_guard lock(mutex_);
    (audio ? audio_ : reactions_)=std::move(parsed);
  }
}
}  // namespace printdeck::platform
