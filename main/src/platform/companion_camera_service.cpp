#include "printdeck/platform/companion_camera_service.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "lwip/inet.h"
#include "printdeck/platform/task_affinity.hpp"
#include "printdeck/platform/board.hpp"
namespace printdeck::platform {
namespace {
struct Body { unsigned char* bytes; std::size_t size=0, limit; };
std::int64_t now_ms(){return esp_timer_get_time()/1000;}
bool get(const std::string& host,const char* path,Body& body,int timeout) {
  const std::string url="http://"+host+path;
  esp_http_client_config_t config{};
  config.url=url.c_str();config.timeout_ms=timeout;
  config.buffer_size=1024;config.buffer_size_tx=384;
  config.disable_auto_redirect=true;
  auto client=esp_http_client_init(&config);if(!client)return false;
  const auto deadline=now_ms()+timeout*3;
  bool ok=esp_http_client_open(client,0)==ESP_OK;
  if(ok) {
    const auto length=esp_http_client_fetch_headers(client);
    ok=length>=0 && esp_http_client_get_status_code(client)==200 &&
       length<=static_cast<std::int64_t>(body.limit);
  }
  // Read explicitly: an event callback error does not reliably stop a peer
  // sending an oversized or indefinitely chunked response.
  while(ok && !esp_http_client_is_complete_data_received(client)) {
    const auto remaining=deadline-now_ms();
    if(remaining<=0 || body.size==body.limit){ok=false;break;}
    esp_http_client_set_timeout_ms(client,std::min<std::int64_t>(timeout,remaining));
    const int count=esp_http_client_read(client,reinterpret_cast<char*>(body.bytes+body.size),
                                        std::min<std::size_t>(1024,body.limit-body.size));
    if(count<=0){ok=count==0 && esp_http_client_is_complete_data_received(client);break;}
    body.size+=count;
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok && body.size>0;
}
}
esp_err_t CompanionCameraService::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(task_) return ESP_OK;
  return xTaskCreatePinnedToCoreWithCaps(entry,"pd_camera",8192,this,2,&task_,kServiceCore,
          MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
void CompanionCameraService::entry(void* context){static_cast<CompanionCameraService*>(context)->run();}
CompanionCameraStatus CompanionCameraService::snapshot() const { std::lock_guard<std::mutex> lock(mutex_);return status_; }
void CompanionCameraService::message(const char* text){std::lock_guard<std::mutex> lock(mutex_);status_.message=text;}
void CompanionCameraService::configure(const std::vector<core::CompanionCamera>& known) {
  std::lock_guard<std::mutex> lock(mutex_);
  for(auto& camera:status_.cameras) camera.printers.clear();
  for(const auto& camera:known) {
    auto it=std::find_if(status_.cameras.begin(),status_.cameras.end(),[&](const auto& c){return c.id==camera.id;});
    if(it==status_.cameras.end()) { if(status_.cameras.size()<core::kMaximumCompanionCameras)status_.cameras.push_back(camera); }
    else it->printers=camera.printers;
  }
}
void CompanionCameraService::search(const NetworkStatus& network, bool web) {
  if(!network.station_connected){message("Connect to Wi-Fi to search");return;}
  {std::lock_guard<std::mutex> lock(mutex_);if(status_.scanning)return;network_=network;
   status_.scanning=true;status_.progress=0;status_.message="Searching for PrintDeck Camera";}
  web_search_deadline_=web?now_ms()+15000:0;
  cancel_requested_=false;scan_requested_=true;
  if(task_)xTaskNotifyGive(task_);
}
void CompanionCameraService::cancel(){cancel_requested_=true;web_search_deadline_=0;}
void CompanionCameraService::maintain_search(bool device_page) {
  const auto deadline=web_search_deadline_.load();
  if(deadline>0 ? now_ms()>deadline : !device_page)cancel();
}
void CompanionCameraService::keep_web_search_alive() {
  if(web_search_deadline_.load()>0)web_search_deadline_=now_ms()+15000;
}
void CompanionCameraService::forget(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_.cameras.erase(std::remove_if(status_.cameras.begin(),status_.cameras.end(),
      [&](const auto& camera){return camera.id==id;}),status_.cameras.end());
}
void CompanionCameraService::view(const core::CompanionCamera* camera,int fps) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string id=camera?camera->id:std::string{};
  core::CompanionCamera effective=camera?*camera:core::CompanionCamera{};
  const auto found=std::find_if(status_.cameras.begin(),status_.cameras.end(),[&](const auto& c){return c.id==id;});
  if(camera && found!=status_.cameras.end())effective.host=found->host;
  const auto& host=effective.host;
  if(viewed_.id==id && viewed_.host==host){fps_=std::clamp(fps,1,5);return;}
  generation_.fetch_add(1);viewed_=std::move(effective);
  view_started_ms_=now_ms();first_frame_pending_=!id.empty();
  if(first_frame_pending_)ESP_LOGI("pd_camera", "Companion view requested");
  fps_=std::clamp(fps,1,5);status_.frame.reset();status_.frame_camera_id.clear();status_.refreshing=false;
  if(task_)xTaskNotifyGive(task_);
}
bool CompanionCameraService::identify(const std::string& host,core::CompanionCamera& camera) {
  unsigned char bytes[1536]{};Body body{bytes,0,sizeof(bytes)-1};
  if(!get(host,"/api/identity",body,400))return false;
  cJSON* json=cJSON_ParseWithLength(reinterpret_cast<char*>(bytes),body.size);
  if(!json)return false;
  const auto* product=cJSON_GetObjectItemCaseSensitive(json,"product");
  const auto* version=cJSON_GetObjectItemCaseSensitive(json,"api_version");
  const auto* id=cJSON_GetObjectItemCaseSensitive(json,"id");
  const auto* name=cJSON_GetObjectItemCaseSensitive(json,"name");
  bool ok=cJSON_IsString(product) && std::strcmp(product->valuestring,"PrintDeck Camera")==0 &&
          cJSON_IsNumber(version) && version->valuedouble==1 && cJSON_IsString(id) && cJSON_IsString(name);
  if(ok){camera.id=id->valuestring;camera.name=name->valuestring;camera.host=host;ok=core::valid_companion_camera(camera);}
  cJSON_Delete(json);return ok;
}
void CompanionCameraService::add(core::CompanionCamera camera) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it=std::find_if(status_.cameras.begin(),status_.cameras.end(),[&](const auto& c){return c.id==camera.id;});
  if(it!=status_.cameras.end()){it->host=camera.host;it->name=camera.name;}
  else if(status_.cameras.size()<core::kMaximumCompanionCameras)status_.cameras.push_back(std::move(camera));
}
void CompanionCameraService::scan() {
  NetworkStatus network;std::vector<std::string> hosts;
  {std::lock_guard<std::mutex> lock(mutex_);network=network_;for(const auto& c:status_.cameras)hosts.push_back(c.host);}
  hosts.push_back("printdeckcamera.local");
  in_addr ip{},mask{};
  if(inet_pton(AF_INET,network.ipv4.c_str(),&ip)==1 && inet_pton(AF_INET,network.netmask.c_str(),&mask)==1){
    const auto local=ntohl(ip.s_addr), netmask=ntohl(mask.s_addr);
    const auto base=local & (netmask | 0xffffff00U);
    const auto end=base | ~(netmask | 0xffffff00U);
    for(auto address=base+1;address<end;++address) {
      if(address==local)continue;
      in_addr value{};value.s_addr=htonl(address);char text[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET,&value,text,sizeof(text));
      if(std::find(hosts.begin(),hosts.end(),text)==hosts.end())hosts.emplace_back(text);
    }
  }
  const auto deadline=now_ms()+120000;
  std::size_t completed=0;
  for(const auto& host:hosts) {
    if(cancel_requested_ || now_ms()>=deadline)break;
    core::CompanionCamera camera;if(identify(host,camera))add(std::move(camera));
    {std::lock_guard<std::mutex> lock(mutex_);status_.progress=static_cast<int>(++completed*100/hosts.size());}
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  std::lock_guard<std::mutex> lock(mutex_);status_.scanning=false;
  status_.message=cancel_requested_?"Search stopped":completed==hosts.size()?"Search complete":"Search time limit reached";
  if(completed==hosts.size())status_.progress=100;
}
void CompanionCameraService::capture(const core::CompanionCamera& camera,std::uint32_t generation) {
  constexpr bool large=kDisplayUsesLargeLayout;
  constexpr int edge=large?466:240;
  constexpr std::size_t limit=large?256*1024:64*1024;
  auto* bytes=static_cast<unsigned char*>(heap_caps_malloc(limit,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(!bytes)return;
  Body body{bytes,0,limit};
  const auto fetch_started=now_ms();
  const bool fetched=get(camera.host,large?"/snapshot.jpg?preset=pd466":"/snapshot.jpg?preset=pd240",body,1000);
  const auto fetch_ms=now_ms()-fetch_started;
  core::CameraFrame frame;
  if(fetched && generation==generation_.load()) {
    jpeg_dec_config_t config=DEFAULT_JPEG_DEC_CONFIG();config.output_type=JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t decoder=nullptr;
    if(jpeg_dec_open(&config,&decoder)==JPEG_ERR_OK){
      jpeg_dec_io_t io{};jpeg_dec_header_info_t header{};
      io.inbuf=bytes;io.inbuf_len=body.size;
      if(jpeg_dec_parse_header(decoder,&io,&header)==JPEG_ERR_OK && header.width==edge && header.height==edge) {
        int size=0;
        if(jpeg_dec_get_outbuf_len(decoder,&size)==JPEG_ERR_OK && size==edge*edge*2) {
          void* output=jpeg_calloc_align(size,16);
          if(output){io.outbuf=static_cast<unsigned char*>(output);
            if(jpeg_dec_process(decoder,&io)==JPEG_ERR_OK)frame=core::CameraFrame::adopt(static_cast<unsigned char*>(output),size,jpeg_free_align);
            else jpeg_free_align(output);
          }
        }
      }
      jpeg_dec_close(decoder);
    }
  }
  heap_caps_free(bytes);
  std::lock_guard<std::mutex> lock(mutex_);
  if(generation!=generation_.load())return;
  status_.refreshing=false;
  if(frame && first_frame_pending_){
    ESP_LOGI("pd_camera", "First frame: %lld ms from request, fetch %lld ms, decode %lld ms", now_ms()-view_started_ms_, fetch_ms, now_ms()-fetch_started-fetch_ms);
    first_frame_pending_=false;
  }
  if(frame){status_.frame=std::move(frame);status_.width=edge;status_.height=edge;status_.frame_camera_id=camera.id;}
  else {status_.frame.reset();status_.frame_camera_id.clear();}
}
void CompanionCameraService::run() {
  std::string verified_id,verified_host;
  for(;;){
    if(scan_requested_.exchange(false)){scan();verified_id.clear();}
    core::CompanionCamera camera;int fps;std::uint32_t generation;
    {std::lock_guard<std::mutex> lock(mutex_);camera=viewed_;fps=fps_;generation=generation_.load();}
    if(camera.id.empty()){verified_id.clear();ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(250));continue;}
    receiving_=true; const auto start=now_ms();
    if(verified_id!=camera.id || verified_host!=camera.host){
      core::CompanionCamera identity;
      if(!identify(camera.host,identity) || identity.id!=camera.id){receiving_=false;ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1000));continue;}
      verified_id=camera.id;verified_host=camera.host;
    }
    {std::lock_guard<std::mutex> lock(mutex_);if(generation==generation_.load())status_.refreshing=true;}
    capture(camera,generation);receiving_=false;
    if(!snapshot().frame)verified_id.clear();
    ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(std::max<std::int64_t>(20,1000/fps-(now_ms()-start))));
  }
}
}
