#include "printdeck/platform/reaction_cover.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <array>
#include <algorithm>
#include <memory>
#include <mutex>
#include <new>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_littlefs.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "printdeck/platform/set_catalog_service.hpp"

namespace printdeck::platform {
namespace {
struct CoverSource { const char* id; const std::uint8_t* data; std::size_t size; };
#include "../../generated/reaction-catalog/reaction_device_previews.inc"

// Immutable indexed pixels live in flash. Selecting a cover neither inflates
// data nor waits for camera/capture workspaces or the application worker.
constexpr auto kDescriptors = [] {
  std::array<lv_image_dsc_t, std::size(kReactionCovers)> images{};
  for (std::size_t i = 0; i < images.size(); ++i) {
    auto& image = images[i];
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_I4;
    image.header.w = image.header.h = kReactionCoverSize;
    image.header.stride = kReactionCoverSize / 2;
    image.data_size = kReactionCovers[i].size;
    image.data = kReactionCovers[i].data;
  }
  return images;
}();

struct DownloadedCover {
  std::string hash;
  lv_image_dsc_t image{};
  ~DownloadedCover() {heap_caps_free(const_cast<std::uint8_t*>(image.data));}
};
struct CachedCover {std::string id; std::shared_ptr<DownloadedCover> data;};
std::mutex covers_mutex;
std::vector<CachedCover> covers;
std::mutex disk_mutex;
constexpr char cache_directory[]="/assets/reaction-covers";
constexpr std::size_t cache_budget=256*1024;
// The on-device filename must fit LittleFS's 64-byte name limit.
// Its 128-bit key is only a lookup; contents still require the full SHA-256.
bool owned_cover(std::string_view name) {
  return (name.size()==36 && name.ends_with(".bin")) &&
         name.substr(0,32).find_first_not_of("0123456789abcdef")==std::string_view::npos;
}
bool valid_pixels(const std::uint8_t* pixels, std::size_t bytes, const std::string& hash) {
  std::array<unsigned char,32> digest{};
  if(mbedtls_sha256(pixels,bytes,digest.data(),0)!=0)return false;
  constexpr char hex[]="0123456789abcdef";
  for(std::size_t i=0;i<digest.size();++i)
    if(hash[2*i]!=hex[digest[i]>>4] || hash[2*i+1]!=hex[digest[i]&15])return false;
  for(std::size_t i=3;i<64;i+=4)if(pixels[i]!=255)return false;
  return true;
}
bool read_cover(const std::string& hash, std::uint8_t* pixels, std::size_t bytes) {
  const std::lock_guard lock(disk_mutex);
  const auto path=std::string(cache_directory)+"/"+hash.substr(0,32)+".bin";
  FILE* file=std::fopen(path.c_str(),"rb");
  if(!file)return false;
  const bool read=std::fread(pixels,1,bytes,file)==bytes && std::fgetc(file)==EOF;
  std::fclose(file);
  if(read && valid_pixels(pixels,bytes,hash))return true;
  std::remove(path.c_str());return false;
}
void save_cover(const std::string& hash, const std::uint8_t* pixels, std::size_t bytes) {
  const std::lock_guard lock(disk_mutex);
  mkdir(cache_directory,0700);
  std::size_t used=0;
  if(auto* directory=opendir(cache_directory)) {
    while(auto* entry=readdir(directory))if(owned_cover(entry->d_name)) {
      struct stat st{};const auto path=std::string(cache_directory)+"/"+entry->d_name;
      if(stat(path.c_str(),&st)==0)used+=st.st_size;
    }
    closedir(directory);
  }
  std::size_t total=0,allocated=0;
  if(used+bytes>cache_budget || esp_littlefs_info("assets",&total,&allocated)!=ESP_OK ||
     allocated>total || total-allocated<bytes+128*1024)return;
  const auto path=std::string(cache_directory)+"/"+hash.substr(0,32)+".bin";
  const auto staging=std::string(cache_directory)+"/download.part";
  FILE* file=std::fopen(staging.c_str(),"wb");
  if(!file)return;
  bool valid=true;
  for(std::size_t offset=0;offset<bytes;) {
    const auto count=std::min<std::size_t>(1024,bytes-offset);
    const auto started=esp_timer_get_time();
    if(std::fwrite(pixels+offset,1,count,file)!=count){valid=false;break;}
    offset+=count;
    vTaskDelay(pdMS_TO_TICKS(std::clamp<std::int64_t>((esp_timer_get_time()-started)*2/1000,10,250)));
  }
  valid=std::fclose(file)==0 && valid;
  if(!valid || std::rename(staging.c_str(),path.c_str())!=0)std::remove(staging.c_str());
}

std::shared_ptr<DownloadedCover> download_cover(const std::string& hash,
                                               const std::atomic<bool>& cancelled, bool online) {
  constexpr std::size_t bytes = 64 + kReactionCoverSize * kReactionCoverSize / 2;
  // At most 32 covers (592 KiB on AMOLED), shared by variants and live images.
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < bytes + 1024 * 1024 || cancelled.load()) return {};
  auto result = std::make_shared<DownloadedCover>();
  auto* pixels = static_cast<std::uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!pixels) return {};
  result->image.data = pixels;
  const bool cached=read_cover(hash,pixels,bytes);
  if(!cached) {
    if(!online)return {};
    const std::string url = std::string(kReactionSetOrigin) + "previews/" + hash + "-" +
                            std::to_string(kReactionCoverSize) + ".bin";
    esp_http_client_config_t config{};
    config.url=url.c_str();config.crt_bundle_attach=esp_crt_bundle_attach;
    config.timeout_ms=2000;config.disable_auto_redirect=true;
    config.buffer_size=1024;config.buffer_size_tx=512;
    const auto client=esp_http_client_init(&config);
    if(!client)return {};
    const auto deadline=esp_timer_get_time()+10000000;
    bool valid=esp_http_client_open(client,0)==ESP_OK;
    if(valid)valid=esp_http_client_fetch_headers(client)==static_cast<std::int64_t>(bytes) &&
                   esp_http_client_get_status_code(client)==200;
    std::size_t received=0;
    while(valid && received<bytes && !cancelled.load() && esp_timer_get_time()<deadline) {
      const int count=esp_http_client_read(client,reinterpret_cast<char*>(pixels+received),
                                          std::min<std::size_t>(1024,bytes-received));
      if(count<=0){valid=false;break;}
      received+=count;
      vTaskDelay(1);
    }
    valid=valid && received==bytes && !cancelled.load() && esp_timer_get_time()<deadline &&
          esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    if(!valid)return {};
    if(!valid_pixels(pixels,bytes,hash))return {};
    save_cover(hash,pixels,bytes);
  }
  result->hash=hash;
  auto& image=result->image;
  image.header.magic=LV_IMAGE_HEADER_MAGIC;image.header.cf=LV_COLOR_FORMAT_I4;
  image.header.w=image.header.h=kReactionCoverSize;image.header.stride=kReactionCoverSize/2;
  image.data_size=bytes;
  ESP_LOGI("reaction_cover","%s verified cover (%u bytes)",cached?"Cached":"Downloaded",unsigned(bytes));
  return result;
}
}

void refresh_reaction_covers(const std::vector<core::DownloadableSet>& sets,
                             const std::atomic<bool>& cancelled, bool online) {
  // Remove obsolete versions, keeping every cover still named by the catalog.
  {const std::lock_guard lock(disk_mutex);
    if(auto* directory=opendir(cache_directory)) {
      while(auto* entry=readdir(directory)) {
        const std::string_view name=entry->d_name;
        const bool keep=owned_cover(name) && std::any_of(sets.begin(),sets.end(),[&](const auto& set){
          return set.available && set.device_previews[kReactionCoverSize==192?0:1].substr(0,32)==name.substr(0,32);
        });
        if(!keep && (owned_cover(name) || name=="download.part"))
          std::remove((std::string(cache_directory)+"/"+entry->d_name).c_str());
      }
      closedir(directory);
    }
  }
  std::vector<CachedCover> previous;
  {const std::lock_guard lock(covers_mutex);previous=covers;}
  std::vector<CachedCover> next;
  const auto deadline=esp_timer_get_time()+60000000;
  for(const auto& set:sets) {
    if(cancelled.load())return;
    if(!set.available)continue;
    const auto& hash=set.device_previews[kReactionCoverSize==192?0:1];
    if(hash.empty() || next.size()>=32)continue;
    std::shared_ptr<DownloadedCover> data;
    for(const auto& item:next)if(item.data->hash==hash){data=item.data;break;}
    if(!data)for(const auto& item:previous)if(item.data->hash==hash){data=item.data;break;}
    if(!data && esp_timer_get_time()<deadline)data=download_cover(hash,cancelled,online);
    if(!data)for(const auto& item:previous)if(item.id==set.id){data=item.data;break;}
    if(data)next.push_back({set.id,std::move(data)});
  }
  if(!cancelled.load()){const std::lock_guard lock(covers_mutex);covers=std::move(next);}
}

std::size_t reclaim_reaction_cover_cache() {
  const std::lock_guard lock(disk_mutex);
  std::size_t removed=0;
  if(auto* directory=opendir(cache_directory)) {
    while(auto* entry=readdir(directory))if(owned_cover(entry->d_name) || std::string_view(entry->d_name)=="download.part") {
      const auto path=std::string(cache_directory)+"/"+entry->d_name;struct stat st{};
      if(stat(path.c_str(),&st)==0 && std::remove(path.c_str())==0)removed+=st.st_size;
    }
    closedir(directory);
  }
  return removed;
}

lv_obj_t* create_reaction_cover(lv_obj_t* parent, std::string_view id) {
  if (id == "family:iris_palette" || id.starts_with("alloy_iris_")) id = "alloy_iris_green";
  std::string representative;
  if(id.starts_with("family:")) {
    for(const auto& set:SetCatalogService::instance().sets(false))if(set.family_id==id.substr(7)) {
      representative=set.id;id=representative;break;
    }
  }
  std::shared_ptr<DownloadedCover> data;
  {const std::lock_guard lock(covers_mutex);
    for(const auto& item:covers)if(item.id==id){data=item.data;break;}
  }
  if(data) {
    auto* owner=new(std::nothrow) std::shared_ptr<DownloadedCover>(std::move(data));
    if(owner) {
      auto* image=lv_image_create(parent);
      lv_image_set_src(image,&(*owner)->image);
      lv_obj_add_event_cb(image,[](lv_event_t* event){
        auto* held=static_cast<std::shared_ptr<DownloadedCover>*>(lv_event_get_user_data(event));
        lv_image_cache_drop(&(*held)->image);
        delete held;
      },LV_EVENT_DELETE,owner);
      return image;
    }
  }
  for (std::size_t i = 0; i < std::size(kReactionCovers); ++i) {
    if (id != kReactionCovers[i].id) continue;
    auto* image = lv_image_create(parent);
    lv_image_set_src(image, &kDescriptors[i]);
    return image;
  }
  return nullptr;
}
}
