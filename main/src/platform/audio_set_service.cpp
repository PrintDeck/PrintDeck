#include "printdeck/platform/audio_set_service.hpp"
#include "printdeck/platform/set_catalog_service.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "printdeck/platform/sd_set_storage.hpp"
#include "printdeck/platform/board.hpp"
#include "printdeck/core/audio_pack.hpp"
#include "printdeck/core/audio_sample.hpp"
#include "printdeck/core/compressed_resource.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_littlefs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "mbedtls/sha256.h"

namespace printdeck::platform {
namespace {
constexpr char root[]="/assets/audio", current[]="/assets/audio/current", previous[]="/assets/audio/previous", staging[]="/assets/audio/staging";
std::uint64_t now_ms(){return esp_timer_get_time()/1000;}
std::size_t size_of(const std::string& path) {struct stat st{};return stat(path.c_str(),&st)==0 && S_ISREG(st.st_mode) ? st.st_size : 0;}
void remove_pair(const char* directory) {
  for(const auto* name:{"/audio.pda","/manifest.json","/set.jsn"}) std::remove((std::string(directory)+name).c_str());
  rmdir(directory);
}
void* allocate(std::size_t bytes) {return heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
bool valid_pack(const std::string& path, const std::string& expected) {
  const auto bytes=size_of(path);
  std::unique_ptr<FILE,decltype(&std::fclose)> f(std::fopen(path.c_str(),"rb"),std::fclose);
  auto index=std::make_unique<core::AudioPackIndex>();
  if(!f || !core::read_audio_pack_index(f.get(),bytes,*index)) return false;
  mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);mbedtls_sha256_starts(&hash,0);
  std::array<unsigned char,1024> block{};std::rewind(f.get());std::size_t read=0;
  while(auto n=std::fread(block.data(),1,block.size(),f.get())) {mbedtls_sha256_update(&hash,block.data(),n);read+=n;}
  std::array<unsigned char,32> digest{};mbedtls_sha256_finish(&hash,digest.data());mbedtls_sha256_free(&hash);
  constexpr char hex[]="0123456789abcdef";std::string actual;
  for(auto byte:digest){actual+=hex[byte>>4];actual+=hex[byte&15];}
  if(read!=bytes || actual!=expected) return false;
  for(std::size_t i=0;i<index->count;++i) {
    const auto& entry=index->entries[i];std::vector<std::uint8_t> gzip(entry.compressed_bytes);
    std::unique_ptr<std::uint8_t,decltype(&heap_caps_free)> decoded(static_cast<std::uint8_t*>(allocate(entry.decoded_bytes)),heap_caps_free);
    core::AudioSampleDecoder decoder;
    if(!decoded || std::fseek(f.get(),entry.offset,SEEK_SET)!=0 || std::fread(gzip.data(),1,gzip.size(),f.get())!=gzip.size() ||
       !core::decompress_gzip_exact(gzip.data(),gzip.size(),decoded.get(),entry.decoded_bytes,allocate,heap_caps_free) ||
       !decoder.open(decoded.get(),entry.decoded_bytes)) return false;
    vTaskDelay(1);
  }
  return true;
}
struct Download {FILE* output;const std::atomic<bool>* cancelled;std::size_t bytes=0,expected=0;std::uint64_t deadline;std::function<void(int)> progress;};
esp_err_t receive(esp_http_client_event_t* event) {
  auto& data=*static_cast<Download*>(event->user_data);
  if(data.cancelled->load() || now_ms()>=data.deadline) return ESP_FAIL;
  if(event->event_id==HTTP_EVENT_ON_DATA) {
    if(event->data_len<0 || static_cast<std::size_t>(event->data_len)>data.expected-data.bytes) return ESP_FAIL;
    const auto n=std::fwrite(event->data,1,event->data_len,data.output);data.bytes+=n;
    if(n!=static_cast<std::size_t>(event->data_len)) return ESP_FAIL;
    data.progress(static_cast<int>(data.bytes*80/data.expected));
  }
  return ESP_OK;
}
std::string text(const cJSON* object,const char* name) {
  const auto* value=cJSON_GetObjectItemCaseSensitive(object,name);
  return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
}
AudioSetService& AudioSetService::instance(){static AudioSetService service;return service;}
std::size_t AudioSetService::storage_bytes() {
  std::size_t total=0;
  for(const auto* directory:{current,previous,staging})
    for(const auto* name:{"/audio.pda","/manifest.json"}) total+=size_of(std::string(directory)+name);
  return total;
}
std::size_t AudioSetService::storage_budget() {
  std::size_t largest=0;
  for(const auto& set:SetCatalogService::instance().sets(true))largest=std::max(largest,set.package.bytes);
  return core::audio_required_budget(largest);
}
std::size_t AudioSetService::storage_reserve() {
  const auto budget=storage_budget(),used=storage_bytes();
  return used<budget?budget-used:0;
}
void AudioSetService::start(const NetworkService& network) {
  network_=&network;mkdir(root,0755);
  if(!load_current()) {
    remove_pair(current);
    if(std::rename(previous,current)==0 && !load_current()) remove_pair(current);
  }
  remove_pair(staging);remove_pair(previous);
}
bool AudioSetService::load_current(const std::string& directory) {
  const bool on_sd = directory.rfind("/sdcard/", 0) == 0;
  const auto path=directory+(on_sd ? "/set.jsn" : "/manifest.json");const auto bytes=size_of(path);
  if(bytes==0 || bytes>2048) return false;
  std::unique_ptr<FILE,decltype(&std::fclose)> f(std::fopen(path.c_str(),"rb"),std::fclose);
  std::string body(bytes,'\0');
  if(!f || std::fread(body.data(),1,bytes,f.get())!=bytes) return false;
  std::unique_ptr<cJSON,decltype(&cJSON_Delete)> json(cJSON_ParseWithLength(body.data(),body.size()),cJSON_Delete);
  if(!json) return false;
  auto id=text(json.get(),"id"),style=text(json.get(),"style"),language=text(json.get(),"language"),version=text(json.get(),"version");
  if(!core::valid_set_id(id) || !core::valid_set_id(style) || version.empty() || version.size()>32 ||
      language!="all" ||
      !valid_pack(directory+"/audio.pda",text(json.get(),"sha256"))) return false;
  // Close metadata before loading the payload (the SD VFS has bounded handles).
  f.reset();
  core::ReactionGif pack;
  std::unique_ptr<core::AudioPackIndex> index;
  if (on_sd) {
    pack = core::ReactionGifBytes::read(directory + "/audio.pda", core::kAudioPackMaximumBytes);
    index = std::make_unique<core::AudioPackIndex>();
    std::unique_ptr<FILE,decltype(&std::fclose)> file(std::fopen((directory + "/audio.pda").c_str(), "rb"), std::fclose);
    if (!pack || !file || !core::read_audio_pack_index(file.get(), pack->size(), *index)) return false;
  }
  const std::lock_guard files(file_mutex_);
  sd_pack_ = std::move(pack); sd_pack_index_ = std::move(index);
  active_directory_ = directory;
  const std::lock_guard lock(mutex_);state_.id=std::move(id);state_.style=std::move(style);state_.language=std::move(language);
  state_.version=std::move(version);state_.available=true;++state_.generation;return true;
}
void AudioSetService::select_storage(const std::string& directory) {
  // Internal audio remains the fallback when SD is disabled, absent or invalid.
  if (!directory.empty()) {
    mkdir(directory.c_str(), 0755);
    const auto selected = directory + "/current", backup = directory + "/previous";
    struct stat info{};
    if (stat(selected.c_str(), &info) != 0) std::rename(backup.c_str(), selected.c_str());
    remove_pair((directory + "/staging").c_str());
    if (load_current(selected)) { remove_pair(backup.c_str()); return; }
    // A committed rename with invalid media must still be able to roll back.
    if (stat(backup.c_str(), &info) == 0) {
      remove_pair(selected.c_str());
      if (std::rename(backup.c_str(), selected.c_str()) == 0 && load_current(selected)) return;
    }
  }
  if (load_current()) return;
  const std::lock_guard files(file_mutex_);
  sd_pack_.reset(); sd_pack_index_.reset(); active_directory_ = current;
  const std::lock_guard lock(mutex_);
  state_.available = false; state_.id.clear(); ++state_.generation;
}
AudioSetSnapshot AudioSetService::snapshot() const {const std::lock_guard lock(mutex_);return state_;}
bool AudioSetService::request(std::string_view id,std::string_view request_id) {
  if(request_id.size()>64) return false;
  const auto catalog=SetCatalogService::instance().sets(true);
  const auto selected=std::find_if(catalog.begin(),catalog.end(),[&](const auto& set){return set.id==id;});
  const std::lock_guard lock(mutex_);
  if(state_.busy || running_.load()) return false;
  state_.error.clear();
  // An installed, validated pack is usable offline too. Keep this idempotent
  // even when an older client sends the same install command again.
  if(state_.available && state_.id==id && state_.language=="all" &&
     (selected==catalog.end() || state_.version==selected->version)) {
    state_.requested_id=id;state_.request_id=request_id;state_.progress=100;
    return true;
  }
  if(!network_ || !network_->status().station_connected) {state_.error="wifi_required";return false;}
  if(selected==catalog.end()) {state_.error="catalog_unavailable";SetCatalogService::instance().request_refresh();return false;}
  requested_=*selected;state_.requested_id=id;state_.busy=true;state_.progress=0;
  state_.request_id=request_id.empty() ? std::to_string(esp_random()) : std::string(request_id);
  SetCatalogService::instance().cancel();
  cancelled_.store(false);pending_.store(true);return true;
}
bool AudioSetService::cancel(std::string_view request_id) {
  const std::lock_guard lock(mutex_);
  if(!state_.busy || (!request_id.empty() && request_id!=state_.request_id))return false;
  cancelled_.store(true);return true;
}
void AudioSetService::poll() {
  if(finished_.load() && task_ && eTaskGetState(task_)==eSuspended) {finished_.store(false);vTaskDeleteWithCaps(task_);task_=nullptr;running_.store(false);}
  if(running_.load() || SetCatalogService::instance().busy() || !pending_.exchange(false))return;
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<32*1024 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<12*1024) {
    const std::lock_guard lock(mutex_);state_.busy=false;state_.error="memory_unavailable";return;
  }
  running_.store(true);
  if(xTaskCreatePinnedToCoreWithCaps(task_entry,"audio_sets",6144,this,2,&task_,kServiceCore,
      MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
    running_.store(false);const std::lock_guard lock(mutex_);state_.busy=false;state_.error="memory_unavailable";
  }
}
void AudioSetService::task_entry(void* context) {
  auto& self=*static_cast<AudioSetService*>(context);self.install();
  {const std::lock_guard lock(self.mutex_);self.state_.busy=false;self.state_.requested_id.clear();}
  self.finished_.store(true);vTaskSuspend(nullptr);
}
void AudioSetService::install() {
  AssetTransferLease lease;
  if (!lease) { const std::lock_guard lock(mutex_); state_.error = "transfer_busy"; return; }
  auto& storage = sd_set_storage();
  const std::lock_guard card(storage.mutex);
  const bool on_sd = !storage.root.empty();
  const std::string directory = on_sd ? storage.root + "/audio" : root;
  const std::string current_path = directory + "/current", previous_path = directory + "/previous",
                    staging_path = directory + "/staging";
  const char* current = current_path.c_str();
  const char* previous = previous_path.c_str();
  const char* staging = staging_path.c_str();
  mkdir(directory.c_str(), 0755);
  ESP_LOGI("audio_sets", "Set download destination: %s", on_sd ? "SD" : "internal");
  const auto fail=[&](const char* error){remove_pair(staging);const std::lock_guard lock(mutex_);state_.error=cancelled_.load()?"cancelled":error;};
  if(cancelled_.load()){fail("cancelled");return;}
  const auto& package=requested_.package;
  remove_pair(previous);remove_pair(staging);
  std::size_t total=0,used=0;
  std::uint64_t sd_total = 0, sd_free = 0;
  const bool room = on_sd ? (board_sd_status() == ESP_OK &&
      board_sd_space(&sd_total, &sd_free) == ESP_OK && package.bytes + kSdSetReserveBytes <= sd_free) :
      (esp_littlefs_info("assets",&total,&used)==ESP_OK && used<=total &&
       package.bytes+2048+128*1024<=total-used && storage_bytes()+package.bytes+2048<=core::kAudioStorageBudget);
  if (!room) {fail("storage_unavailable");return;}
  if(mkdir(staging,0755)!=0){fail("storage_unavailable");return;}
  const auto path=std::string(staging)+"/audio.pda";
  FILE* output=std::fopen(path.c_str(),"wb");if(!output){fail("storage_unavailable");return;}
  Download data{output,&cancelled_,0,package.bytes,now_ms()+60000,[this](int progress) {
    const std::lock_guard lock(mutex_);state_.progress=progress;}};
  const auto url=std::string(kAudioSetOrigin)+package.file;
  esp_http_client_config_t config{};config.url=url.c_str();config.crt_bundle_attach=esp_crt_bundle_attach;
  config.timeout_ms=3000;config.disable_auto_redirect=true;config.buffer_size=2048;config.buffer_size_tx=512;
  config.event_handler=receive;config.user_data=&data;
  auto client=esp_http_client_init(&config);
  bool downloaded=false;
  if(client) {
    downloaded=esp_http_client_perform(client)==ESP_OK && esp_http_client_get_status_code(client)==200 &&
      esp_http_client_is_complete_data_received(client) && data.bytes==package.bytes && now_ms()<data.deadline;
    esp_http_client_cleanup(client);
  }
  if(std::fclose(output)!=0)downloaded=false;
  if(!downloaded || cancelled_.load()){fail("download_unavailable");return;}
  {const std::lock_guard lock(mutex_);state_.progress=85;}
  if(!valid_pack(path,package.sha256)){fail("invalid_package");return;}
  std::unique_ptr<cJSON,decltype(&cJSON_Delete)> manifest(cJSON_CreateObject(),cJSON_Delete);
  if(!manifest){fail("memory_unavailable");return;}
  const std::string all_languages="all";
  for(const auto& pair:std::array<std::pair<const char*,const std::string*>,5>{{{"id",&requested_.id},{"style",&requested_.style},
      {"language",&all_languages},{"version",&requested_.version},{"sha256",&package.sha256}}}) cJSON_AddStringToObject(manifest.get(),pair.first,pair.second->c_str());
  std::unique_ptr<char,decltype(&cJSON_free)> body(cJSON_PrintUnformatted(manifest.get()),cJSON_free);
  if(!body){fail("memory_unavailable");return;}
  FILE* metadata=std::fopen((std::string(staging)+(on_sd ? "/set.jsn" : "/manifest.json")).c_str(),"wb");
  if(!metadata){fail("storage_unavailable");return;}
  const bool written=std::fputs(body.get(),metadata)>=0;const bool closed=std::fclose(metadata)==0;
  if(!written || !closed || cancelled_.load()){fail("storage_unavailable");return;}
  core::ReactionGif next_pack;
  std::unique_ptr<core::AudioPackIndex> next_index;
  if (on_sd) {
    next_pack = core::ReactionGifBytes::read(path, core::kAudioPackMaximumBytes);
    next_index = std::make_unique<core::AudioPackIndex>();
    std::unique_ptr<FILE,decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!next_pack || !file || !core::read_audio_pack_index(file.get(), next_pack->size(), *next_index)) {
      fail("memory_unavailable"); return;
    }
  }
  const std::lock_guard files(file_mutex_);
  // Cancellation and commit are serialized: cancellation accepted before this
  // point leaves the previous set intact; after commit there is nothing to cancel.
  std::unique_lock commit(mutex_);
  if(cancelled_.load()){commit.unlock();fail("cancelled");return;}
  struct stat info{};const bool had_current=stat(current,&info)==0;
  if(had_current && std::rename(current,previous)!=0){commit.unlock();fail("storage_unavailable");return;}
  if(std::rename(staging,current)!=0) {
    if(had_current)std::rename(previous,current);
    commit.unlock();fail("storage_unavailable");return;
  }
  active_directory_ = current_path;
  sd_pack_ = std::move(next_pack); sd_pack_index_ = std::move(next_index);
  {state_.id=requested_.id;state_.style=requested_.style;state_.language="all";
   state_.version=requested_.version;state_.available=true;state_.progress=100;state_.error.clear();++state_.generation;state_.busy=false;}
  commit.unlock();
  remove_pair(previous);
}
bool AudioSetService::read_sample(std::string_view style,std::string_view language,std::string_view event,
    std::vector<std::uint8_t>& gzip,std::size_t& decoded) const {
  const std::lock_guard files(file_mutex_);
  {const std::lock_guard lock(mutex_);if(!state_.available || state_.style!=style || state_.language!="all")return false;}
  const auto path=active_directory_+"/audio.pda";
  std::unique_ptr<FILE,decltype(&std::fclose)> f(nullptr,std::fclose);
  std::unique_ptr<core::AudioPackIndex> flash_index;
  const auto* index = sd_pack_index_.get();
  if (!sd_pack_) {
    f.reset(std::fopen(path.c_str(), "rb"));
    flash_index = std::make_unique<core::AudioPackIndex>();
    if (!f || !core::read_audio_pack_index(f.get(), size_of(path), *flash_index)) return false;
    index = flash_index.get();
  }
  if (!index) return false;
  auto locale=std::find(core::kSetLanguages.begin(),core::kSetLanguages.end(),language);
  if(locale==core::kSetLanguages.end())return false;
  constexpr std::array<std::string_view,6> prefixes{"en_","pl_","es_","fr_","de_","zh_cn_"};
  const auto* entry=index->find(std::string(prefixes[locale-core::kSetLanguages.begin()])+std::string(event));
  if(!entry)entry=index->find(event);
  if(!entry)return false;
  gzip.resize(entry->compressed_bytes);decoded=entry->decoded_bytes;
  if (sd_pack_) {
    if (entry->offset > sd_pack_->size() || gzip.size() > sd_pack_->size() - entry->offset) return false;
    std::copy_n(sd_pack_->data() + entry->offset, gzip.size(), gzip.data());
    return true;
  }
  return std::fseek(f.get(),entry->offset,SEEK_SET)==0 && std::fread(gzip.data(),1,gzip.size(),f.get())==gzip.size();
}
}  // namespace printdeck::platform
