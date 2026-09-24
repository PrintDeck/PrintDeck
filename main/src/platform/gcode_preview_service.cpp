#include "printdeck/platform/gcode_preview_service.hpp"
#include "printdeck/platform/prusalink_http_transport.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "printdeck/core/settings.hpp"
#include "printdeck/platform/elegoo_sdcp_parser.hpp"
#include "printdeck/platform/elegoo_cc2_parser.hpp"
#include "printdeck/platform/gcode_ftps.hpp"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/idf_additions.h"
#include "mbedtls/sha256.h"
#include <array>
#include <algorithm>
#include <chrono>

namespace printdeck::platform {
namespace {
std::string digest(std::string_view value){
 std::array<unsigned char,32> hash{};
 if(mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.data()),value.size(),hash.data(),0))return {};
 constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(64);
 for(auto c:hash){out+=hex[c>>4];out+=hex[c&15];}return out;
}
}
void GcodePreviewService::update(const core::PrinterProfile* profile,const core::PrinterSnapshot& state,bool suspended){
 const std::lock_guard lock(mutex_);
 const bool valid=profile&&profile->id==state.profile_id&&core::gcode_fdm(profile->protocol);
 const auto identity=valid?std::to_string(profile->id)+"\n"+profile->endpoint+"\n"+state.job.gcode_file+"\n"+state.job.preview_hint+"\n"+state.job.gcode_download:"";
 const bool changed=(state.job.current_layer&&last_layer_&&state.job.current_layer<last_layer_)||state.job.elapsed_seconds+5<last_elapsed_||identity!=identity_||(valid&&!core::same_printer_connection(profile_,*profile));
 if(changed){identity_=identity;profile_=valid?*profile:core::PrinterProfile{};generation_=esp_random()|1U;
   path_=valid?core::gcode_http_path(profile->protocol,state.job.gcode_file).value_or(""):"";
   if(valid&&profile->protocol==core::PrinterProtocol::prusalink){
     const auto origin=prusalink_origin(profile->endpoint);
     path_=origin&&!state.job.gcode_download.empty()?core::gcode_prusa_download(state.job.gcode_download,*origin).value_or(""):"";
   }
   if(valid&&profile->protocol==core::PrinterProtocol::bambu_lan)path_=core::gcode_bambu_path(state.job.gcode_file,state.job.preview_hint).value_or("");
   unavailable_=!valid?"no_job":!core::gcode_plain(state.job.gcode_file)?"format_unavailable":path_.empty()?"source_unavailable":"";
   work_={};result_={};size_=0;key_.clear();validator_.clear();lease_=0;next_=0;metadata_at_=0;
 }
 last_elapsed_=state.job.elapsed_seconds;last_layer_=state.job.current_layer;
 online_=valid&&state.link==core::LinkState::online&&(state.job.phase==core::JobPhase::printing||state.job.phase==core::JobPhase::paused||state.job.phase==core::JobPhase::preparing||state.job.phase==core::JobPhase::completed);
 status_at_=state.updated_at_ms;suspended_=suspended;
 if(!online_||suspended_){work_.pending=false;result_.bytes.reset();lease_=0;}
}
GcodePreviewService::Result GcodePreviewService::request(std::uint32_t profile,bool metadata,std::uint32_t generation,std::uint64_t offset,std::uint32_t length){
 const std::lock_guard lock(mutex_);const auto now=prusalink_now_ms();
 if(profile!=profile_.id||!online_||now<status_at_||now-status_at_>10000)return {.status=404,.reason="no_job"};
 if(!unavailable_.empty())return {.status=422,.reason=unavailable_};
 if(suspended_)return {.status=429,.reason="busy"};
 if(!metadata&&(generation!=generation_||!size_))return {.status=409,.reason="changed"};
 if(!metadata&&(!length||length>core::kGcodeChunkLimit||offset>=size_||length>size_-offset))return {.status=400,.reason="range"};
 lease_=now+6000;
 if(metadata&&size_&&now-metadata_at_<5000)return {.status=200,.reason="ready",.key=key_,.generation=generation_,.size=size_};
 if(!task_&&xTaskCreatePinnedToCoreWithCaps(entry,"gcode_preview",12288,this,1,&task_,kServiceCore,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS)return {.status=503,.reason="busy"};
 const bool same=work_.metadata==metadata&&(metadata||(work_.offset==offset&&work_.length==length));
 if(same&&!work_.pending&&result_.status==200&&result_.bytes)return result_;
 if(same&&work_.attempts>=3)return {.status=422,.reason="source_unavailable"};
 if(work_.pending)return {.status=202,.reason="pending"};
 if(now<next_)return {.status=429,.reason="busy"};
 const auto attempts=same?work_.attempts:0;work_={true,metadata,offset,length,attempts};result_={};
 xTaskNotifyGive(task_);return {.status=202,.reason="pending"};
}
void GcodePreviewService::entry(void* value){static_cast<GcodePreviewService*>(value)->run();}
void GcodePreviewService::run(){
 while(true){
  ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(250));
  core::PrinterProfile profile;Work work;std::string path,identity,validator;std::uint32_t generation;std::uint64_t size;
  {const std::lock_guard lock(mutex_);if(prusalink_now_ms()>=lease_){result_.bytes.reset();work_.pending=false;}
   if(!work_.pending||suspended_||!online_)continue;
   profile=profile_;work=work_;path=path_;identity=identity_;generation=generation_;validator=validator_;size=size_;}
  const auto cancelled=[&]{const std::lock_guard lock(mutex_);const auto now=prusalink_now_ms();return generation!=generation_||!online_||suspended_||now>=lease_||now<status_at_||now-status_at_>10000;};
  if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<32*1024||heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)<256*1024){
   const std::lock_guard lock(mutex_);if(generation==generation_){work_.pending=false;next_=prusalink_now_ms()+1500;}continue;
  }
  // Share the HTTP transaction gate with selected telemetry. No network in the web server or LVGL task.
  PrinterTransactionLock transaction(profile.endpoint);
  if(!transaction.try_lock_for(std::chrono::milliseconds(20)))continue;
  PrusaLinkEspTransport transport;
  PrusaLinkClient client(transport,prusalink_md5,prusalink_now_ms,prusalink_random_cnonce);
  PrusaLinkCredentials credentials{.mode=profile.protocol==core::PrinterProtocol::prusalink?profile.http_auth_mode:core::HttpAuthMode::api_key,
    .username=profile.http_username,.password=profile.http_password,.api_key=profile.api_key};
  PrusaLinkHttpResponse response{.error=PrusaLinkError::invalid_configuration};
  const auto offset=work.metadata?0:work.offset;const auto length=work.metadata?1:work.length;
  const auto deadline=prusalink_now_ms()+3500;
  if(profile.protocol==core::PrinterProtocol::bambu_lan){
    response=gcode_ftps_range(profile,path,offset,length,deadline,cancelled);
  }else if(profile.protocol==core::PrinterProtocol::prusalink){
    if(client.configure(profile.endpoint,std::move(credentials),profile.id))response=client.read_range(path,offset,length,deadline,cancelled);
  }else{
    auto origin=prusalink_origin(profile.endpoint);
    if(profile.protocol==core::PrinterProtocol::elegoo_sdcp){
      const auto endpoint=elegoo_sdcp_endpoint(profile.endpoint);
      origin=endpoint?prusalink_origin(endpoint->host):std::nullopt;
    }else if(profile.protocol==core::PrinterProtocol::elegoo_cc2){
      const auto host=elegoo_cc2_host(profile.endpoint);
      origin=host?prusalink_origin(*host):std::nullopt;
      const auto token=core::gcode_query_token(profile.access_code);
      if(token)path+="?X-Token="+*token;else origin.reset();
    }
    if(origin){
      PrusaLinkHttpRequest request{.url=*origin+path,.maximum_body=std::max<std::size_t>(length,4096),
        .deadline_ms=deadline,.range_offset=offset,.range_length=length};
      if((profile.protocol==core::PrinterProtocol::moonraker||profile.protocol==core::PrinterProtocol::octoprint)&&!profile.api_key.empty()){
        request.header_name="X-Api-Key";request.header_value=profile.api_key;
      }
      response=transport.get(request,cancelled);
    }
  }
  transaction.unlock();
  if(cancelled())continue;
  std::uint64_t total=0;
  const auto next_validator=!response.etag.empty()?"E:"+response.etag:!response.last_modified.empty()?"M:"+response.last_modified:"";
  const bool framing=response.error==PrusaLinkError::none&&(response.content_encoding.empty()||response.content_encoding=="identity")&&response.status==206&&response.body.size()==length&&core::gcode_range(response.content_range,offset,length,total);
  const bool changed=framing&&size&&(total!=size||validator!=next_validator);
  const std::lock_guard lock(mutex_);if(generation!=generation_)continue;
  work_.pending=false;next_=prusalink_now_ms()+250;
  if(changed){generation_=esp_random()|1U;size_=0;key_.clear();validator_.clear();work_={};result_={.status=409,.reason="changed"};continue;}
  if(!framing){++work_.attempts;next_=prusalink_now_ms()+2000;result_={.status=503,.reason="unavailable"};continue;}
  if(work.metadata){
   size_=total;validator_=next_validator;metadata_at_=prusalink_now_ms();
   // Without a remote validator, never reuse persisted chunks across device sessions/jobs.
   key_=digest(identity+"\n"+std::to_string(total)+"\n"+(validator_.empty()?std::to_string(generation_):validator_));
   result_={.status=200,.reason="ready",.key=key_,.generation=generation_,.size=size_};
  }else result_={.status=200,.reason="ready",.key=key_,.generation=generation_,.size=size_,.offset=offset,.bytes=std::make_shared<std::string>(std::move(response.body))};
 }
}
} // namespace printdeck::platform
