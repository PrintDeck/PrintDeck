#include "printdeck/platform/mqtt_export_service.hpp"
#include <algorithm>
#include <limits>
#include <cstdio>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include "printdeck/core/mqtt_discovery.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "printdeck/platform/web_config.hpp"

namespace printdeck::platform {
namespace {
constexpr auto kEmpty = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kPayloadLimit = 16384;
std::int64_t now_ms() { return esp_timer_get_time()/1000; }
bool contains(const auto& values, std::uint32_t id) {
  return std::find(values.begin(),values.end(),id)!=values.end();
}
// JSON escaping can expand a byte to six. Bound strings and cardinalities
// before shared serialization, not only after allocation.
bool bounded(const core::UnifiedPrinterView& view) {
  const auto& job=view.snapshot.job;
  std::size_t strings=view.display_name.size()+view.endpoint.size()+view.manufacturer.size()+view.model.size()+job.name.size();
  if(job.materials.slots.size()>32 || job.materials.external_spools.size()>12) return false;
  for(const auto& tool:job.toolheads) strings+=tool.state.size()+tool.material.size();
  for(const auto& slot:job.materials.slots) strings+=slot.material.size();
  for(const auto& slot:job.materials.external_spools) strings+=slot.material.size();
  strings+=job.materials.external_spool.material.size();
  return strings<=1024;
}
}

void MqttExportService::initialize(WebConfig& source, PersistenceWorker& persistence) {
  source_=&source; persistence_=&persistence;
  registry_.fill(kEmpty);
  nvs_handle_t handle=0;
  const auto opened=nvs_open("pd_mqtt",NVS_READONLY,&handle);
  if(opened==ESP_OK) {
    std::size_t length=sizeof(registry_);
    const auto result=nvs_get_blob(handle,"registry",registry_.data(),&length);
    if(result==ESP_ERR_NVS_NOT_FOUND) registry_.fill(kEmpty);
    else if(result!=ESP_OK || length!=sizeof(registry_)) error_=4;
    nvs_close(handle);
  } else if(opened!=ESP_ERR_NVS_NOT_FOUND) error_=4;
  for(std::size_t i=0;i<registry_.size();++i) {
    const auto id=registry_[i];
    if(id==kEmpty) continue;
    if(id>std::numeric_limits<std::uint32_t>::max() ||
       std::find(registry_.begin(),registry_.begin()+i,id)!=registry_.begin()+i) error_=4;
  }
  registration_count_=std::count_if(registry_.begin(),registry_.end(),[](auto id){return id!=kEmpty;});
  const auto config=source.mqtt_settings();
  cleanup_pending_=has_registrations() && (!config.enabled || !config.discovery);
  persistence.bind(PersistenceWorker::Slot::mqtt,persist_entry,this);
  source.set_mqtt_export(this);
  initialized_=error_.load()!=4;
  if(!initialized_) registration_count_=core::kMaximumProfiles+1;
}

const char* MqttExportService::error() const {
  switch(error_.load()) {
    case 1:return "connection_failed";
    case 2:return "memory_low";
    case 3:return "cleanup_pending";
    case 4:return "configuration_failed";
    default:return "";
  }
}

void MqttExportService::tick(bool network_ready, bool enabled) {
  network_ready_=network_ready; enabled_=enabled;
  if(reset_requested_ || !initialized_ || !persistence_->running() || (!enabled && !cleanup_pending_) || !network_ready || running_.exchange(true)) return;
  TaskHandle_t task=nullptr;
  if(xTaskCreatePinnedToCoreWithCaps(task_entry,"mqtt_export",6144,this,2,&task,
      kServiceCore,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
    running_=false; error_=2;
  }
}

void MqttExportService::task_entry(void* context) {
  auto* service=static_cast<MqttExportService*>(context);
  service->run();
  service->running_=false;
  vTaskDeleteWithCaps(nullptr);
}

void MqttExportService::event_entry(void* context, esp_event_base_t, int32_t id, void* data) {
  auto* service=static_cast<MqttExportService*>(context);
  const auto* event=static_cast<esp_mqtt_event_t*>(data);
  if(id==MQTT_EVENT_CONNECTED) {
    service->connected_=true; service->announce_requested_=true;
  } else if(id==MQTT_EVENT_DISCONNECTED || id==MQTT_EVENT_ERROR) {
    service->connected_=false; service->error_=1;
  } else if(id==MQTT_EVENT_PUBLISHED) {
    service->ack_=event->msg_id;
  } else if(id==MQTT_EVENT_DATA && event->current_data_offset==0 &&
      event->data_len==6 && event->total_data_len==6 && event->topic_len==20 &&
      std::string_view(event->topic,20)=="homeassistant/status" &&
      std::string_view(event->data,6)=="online") {
    service->announce_requested_=true;
  }
}

auto MqttExportService::registry_copy() -> std::array<std::uint64_t,core::kMaximumProfiles+1> {
  const std::lock_guard<std::mutex> lock(registry_mutex_);
  return registry_;
}

std::uint32_t MqttExportService::persist_entry(void* context) {
  auto* service=static_cast<MqttExportService*>(context);
  service->persist_active_=true;
  if(service->reset_requested_) { service->persist_active_=false; return PersistenceWorker::kNoRetry; }
  const auto revision=service->persist_revision_.load();
  const auto registry=service->registry_copy();
  nvs_handle_t handle=0;
  auto result=nvs_open("pd_mqtt",NVS_READWRITE,&handle);
  if(result==ESP_OK) {
    result=nvs_set_blob(handle,"registry",registry.data(),sizeof(registry));
    if(result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
  }
  if(result!=ESP_OK) { service->error_=4; service->persist_active_=false; return 1000; }
  service->saved_revision_=revision;
  service->persist_active_=false;
  return PersistenceWorker::kNoRetry;
}

bool MqttExportService::save_registry() {
  const auto revision=persist_revision_.load();
  if(reset_requested_) return false;
  if(saved_revision_.load()==revision) return true;
  if(!persistence_->request(PersistenceWorker::Slot::mqtt)) return false;
  const auto deadline=now_ms()+3000;
  while(!reset_requested_ && saved_revision_.load()<revision && now_ms()<deadline) vTaskDelay(pdMS_TO_TICKS(20));
  return saved_revision_.load()>=revision;
}

bool MqttExportService::register_id(std::uint32_t id) {
  auto configuration_guard=configuration_lock();
  if(reset_requested_ || !source_->mqtt_settings_equal(active_)) return false;
  {
    const std::lock_guard<std::mutex> lock(registry_mutex_);
    if(!contains(registry_,id)) {
      auto free=std::find(registry_.begin(),registry_.end(),kEmpty);
      if(free==registry_.end()) { error_=3; return false; }
      *free=id; ++registration_count_; ++persist_revision_;
    }

  }
  configuration_guard.unlock();
  return save_registry();
}

bool MqttExportService::publish(const std::string& topic,const std::string& payload,
                                bool retained,bool confirmed) {
  if(!client_ || !connected_ || payload.size()>kPayloadLimit || topic.size()>256) return false;
  if(confirmed) ack_=-1;
  const int message=esp_mqtt_client_publish(client_,topic.c_str(),payload.data(),
      payload.size(),confirmed?1:0,retained);
  if(message<0) return false;
  if(!confirmed) return true;
  const auto deadline=now_ms()+2000;
  while(connected_ && ack_.load()!=message && now_ms()<deadline) vTaskDelay(pdMS_TO_TICKS(10));
  return ack_.load()==message;
}

bool MqttExportService::cleanup(std::uint32_t id) {
  for(std::size_t i=0;i<core::mqtt_discovery_entity_count(id);++i) {
    const auto* sensor=core::mqtt_discovery_sensor_at(id,i);
    if(!sensor || !publish(core::mqtt_discovery_topic(device_id_,id,*sensor),"",true,true)) return false;
  }
  auto configuration_guard=configuration_lock();
  {
    const std::lock_guard<std::mutex> lock(registry_mutex_);
    const auto found=std::find(registry_.begin(),registry_.end(),id);
    if(found!=registry_.end()) { *found=kEmpty; --registration_count_; ++persist_revision_; }
  }
  configuration_guard.unlock();
  return save_registry();
}

bool MqttExportService::announce(bool refresh_discovery) {
  const bool discovery=active_.enabled && active_.discovery;
  cleanup_pending_=!discovery && has_registrations();
  auto info=source_->unified_info_json();
  info.pop_back();
  info+=",\"mqtt\":{\"home_assistant_discovery\":"+std::string(discovery?"true":"false")+
      ",\"discovery_cleanup_pending\":"+(cleanup_pending_?"true":"false")+"}}";
  if(!publish(root_+"/info",info,true,true)) return false;
  const auto ids=source_->unified_printer_ids();
  std::string catalog="{\"api_version\":\"v1\",\"printers\":[";
  bool first=true;
  for(auto id:ids) {
    if(id==0) continue;
    const auto view=source_->unified_printer_views(id,true);
    if(view.empty()) continue;
    if(!bounded(view.front())) { error_=4; return false; }
    const auto record=core::unified_api_printers_json(view);
    const auto begin=record.find('[')+1;
    const auto length=record.size()-begin-2;
    if(catalog.size()+length+3>kPayloadLimit) { error_=4; return false; }
    if(!first) catalog+=',';
    catalog.append(record,begin,length); first=false;
  }
  catalog+="]}";
  std::uint32_t hash=2166136261U;
  for(unsigned char ch:catalog) hash=(hash^ch)*16777619U;
  for(unsigned char ch:source_->export_language()) hash=(hash^ch)*16777619U;
  refresh_discovery=refresh_discovery || hash!=catalog_hash_;
  if(hash!=catalog_hash_ || generation_.empty()) {
    char value[18]{};
    std::snprintf(value,sizeof(value),"%08lx-%08lx",static_cast<unsigned long>(session_nonce_),
                  static_cast<unsigned long>(++generation_counter_));
    generation_=value;
  }
  catalog_hash_=hash;
  catalog.pop_back(); catalog+=",\"_mqtt_generation\":\""+generation_+"\"}";
  if(!publish(root_+"/printers",catalog,true,true)) return false;
  for(auto id:registry_copy()) {
    if(id==kEmpty) continue;
    const bool present=id==0 || contains(ids,id);
    if(!discovery || !present) {
      cleanup_pending_=true;
      if(!cleanup(id)) return false;
    }
  }
  if(persist_revision_.load()!=saved_revision_.load() && !save_registry()) {
    cleanup_pending_=true;
    return false;
  }
  cleanup_pending_=false;
  if(discovery && refresh_discovery) {
    const auto configure=[&](const core::UnifiedPrinterView* view) {
      const auto id=view?view->id:0;
      if(!register_id(id)) return false;
      for(std::size_t i=0;i<core::mqtt_discovery_entity_count(id);++i) {
        if(reset_requested_ || !source_->mqtt_settings_equal(active_)) return false;
        const auto* sensor=core::mqtt_discovery_sensor_at(id,i);
        if(!sensor) return false;
        const auto payload=core::mqtt_discovery_json(device_id_,root_,view,*sensor,source_->export_language());
        if(payload.empty() || !publish(core::mqtt_discovery_topic(device_id_,id,*sensor),payload,true,true)) return false;
      }
      return true;
    };
    if(!configure(nullptr)) return false;
    for(auto id:ids) {
      if(!id) continue;
      const auto views=source_->unified_printer_views(id,true);
      if(!views.empty() && !configure(&views.front())) return false;
    }
  }
  info=source_->unified_info_json(); info.pop_back();
  info+=",\"mqtt\":{\"home_assistant_discovery\":"+std::string(discovery?"true":"false")+
      ",\"discovery_cleanup_pending\":false}}";
  return publish(root_+"/info",info,true,true);
}

bool MqttExportService::publish_cycle() {
  const auto envelope=[&](std::string payload) {
    payload.pop_back(); payload+=",\"_mqtt_generation\":\""+generation_+"\"}"; return payload;
  };
  const auto power=source_->unified_power();
  if(!publish(root_+"/device",envelope(core::unified_api_snapshot_json({},power)),false,false)) return false;
  const auto ids=source_->unified_printer_ids();
  for(auto it=event_cursors_.begin();it!=event_cursors_.end();) {
    if(!contains(ids,it->first)) it=event_cursors_.erase(it); else ++it;
  }
  for(auto id:ids) {
    if(!id) continue;
    if(reset_requested_ || !source_->mqtt_settings_equal(active_)) return false;
    auto views=source_->unified_printer_views(id);
    if(views.empty()) continue;
    const auto& view=views.front();
    if(!bounded(view)) { error_=4; return false; }
    const auto path=root_+"/printers/"+std::to_string(view.id);
    if(!publish(path+"/info",core::unified_api_printer_json(view),false,false) ||
       !publish(path+"/status",envelope(core::unified_api_status_json(view)),false,false) ||
       !publish(path+"/nozzles",core::unified_api_nozzles_json(view),false,false) ||
       !publish(path+"/materials",core::unified_api_materials_json(view),false,false)) return false;
    const auto& history=view.print_events;
    auto& cursor=event_cursors_[view.id];
    if(cursor.first==history.stream && cursor.first) {
      for(std::size_t index=0;index<history.count;++index) {
        const auto& event=history.events[index];
        if(event.sequence<=cursor.second) continue;
        // Old journal entries are for explicit consumers, never delayed announcements.
        if(now_ms()>=static_cast<std::int64_t>(event.observed_at_ms) &&
           now_ms()-static_cast<std::int64_t>(event.observed_at_ms)<=30'000) {
          auto payload=core::unified_api_print_event_json(history,event);
          payload.pop_back();
          payload+=",\"printdeck_id\":\""+device_id_+"\",\"printer_id\":\""+std::to_string(view.id)+"\",\"routing_key\":\""+device_id_+":"+std::to_string(view.id)+"\",\"event_id\":\""+device_id_+":"+std::to_string(view.id)+":"+core::print_stream_id(history.stream)+":"+std::to_string(event.sequence)+"\"}";
          if(!publish(path+"/events",payload,false,false)) return false;
        }
        cursor.second=event.sequence;
      }
    }
    cursor={history.stream,history.sequence};
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return publish(root_+"/availability","online",true,false);
}

void MqttExportService::destroy_client() {
  if(client_) {
    if(connected_) publish(root_+"/availability","offline",true,false);
    // ESP-MQTT stop waits for its task. Only this disposable owner can wait.
    esp_mqtt_client_stop(client_);
    esp_mqtt_client_destroy(client_);
    client_=nullptr;
  }
  connected_=false;
  event_cursors_.clear();
}

void MqttExportService::run() {
  auto identity=source_->unified_info_json();
  auto* parsed=cJSON_Parse(identity.c_str());
  const auto* id=parsed?cJSON_GetObjectItemCaseSensitive(parsed,"device_id"):nullptr;
  if(cJSON_IsString(id)) device_id_=id->valuestring;
  cJSON_Delete(parsed);
  if(device_id_.empty()) { error_=4; return; }
  root_="printdeck/"+device_id_+"/v1";
  std::int64_t last_cycle=0,last_announce=0,retry=0;
  unsigned failures=0;
  session_nonce_=esp_random(); generation_counter_=0; catalog_hash_=0; generation_.clear();
  while(!reset_requested_ && (enabled_ || cleanup_pending_)) {
    if(!source_->mqtt_settings_equal(active_)) {
      auto requested=source_->mqtt_settings();
      if(client_) {
      if(active_.discovery && !requested.discovery) {
        cleanup_pending_=has_registrations();
        for(auto registered:registry_copy()) if(registered!=kEmpty && !cleanup(registered)) break;
      }
        destroy_client();
      }
      active_=std::move(requested);
    }
    if(!active_.enabled && !has_registrations()) break;
    if(!network_ready_) { destroy_client(); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if(!client_ && now_ms()>=retry) {
      if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<4096 ||
         heap_caps_get_free_size(MALLOC_CAP_SPIRAM)<128*1024) {
        error_=2; retry=now_ms()+5000; vTaskDelay(pdMS_TO_TICKS(100)); continue;
      }
      const std::string uri=std::string(active_.tls?"mqtts://":"mqtt://")+active_.host+":"+std::to_string(active_.port);
      const std::string client_id=device_id_+"-export";
      const std::string will=root_+"/availability";
      esp_mqtt_client_config_t config{};
      config.broker.address.uri=uri.c_str();
      if(active_.tls) {
        if(active_.ca_certificate.empty()) config.broker.verification.crt_bundle_attach=esp_crt_bundle_attach;
        else config.broker.verification.certificate=active_.ca_certificate.c_str();
      }
      config.credentials.client_id=client_id.c_str();
      config.credentials.username=active_.username.empty()?nullptr:active_.username.c_str();
      config.credentials.authentication.password=active_.password.empty()?nullptr:active_.password.c_str();
      config.session.protocol_ver=MQTT_PROTOCOL_V_3_1_1;
      config.session.keepalive=30;
      config.session.last_will.topic=will.c_str(); config.session.last_will.msg="offline";
      config.session.last_will.qos=1; config.session.last_will.retain=true;
      config.network.timeout_ms=2000; config.network.disable_auto_reconnect=true;
      config.task.priority=3; config.task.stack_size=4096;
      config.buffer.size=1024; config.buffer.out_size=1024; config.outbox.limit=20*1024;
      client_=esp_mqtt_client_init(&config);
      if(!client_ || esp_mqtt_client_register_event(client_,MQTT_EVENT_ANY,event_entry,this)!=ESP_OK ||
         esp_mqtt_client_start(client_)!=ESP_OK) { destroy_client(); error_=1; retry=now_ms()+5000; }
      else {
        const auto deadline=now_ms()+6000;
        while((enabled_ || cleanup_pending_) && !connected_ && now_ms()<deadline) vTaskDelay(pdMS_TO_TICKS(50));
        if(!connected_) {
          destroy_client(); error_=1;
          failures=std::min(failures+1,5U);
          retry=now_ms()+std::min(30000U,1000U<<failures)+(esp_random()%1000);
        } else {
          esp_mqtt_client_subscribe(client_,"homeassistant/status",0);
          announce_requested_=true; failures=0;
        }
      }
    }
    if(client_ && !connected_) { destroy_client(); retry=now_ms()+5000; }
    if(connected_) {
      bool okay=true;
      const bool refresh=announce_requested_.exchange(false);
      if(refresh || now_ms()-last_announce>=30000) {
        okay=announce(refresh); last_announce=now_ms(); last_cycle=0;
      }
      if(!active_.enabled) break;
      if(okay && now_ms()-last_cycle>=5000) { okay=publish_cycle(); last_cycle=now_ms(); }
      if(!okay) { destroy_client(); retry=now_ms()+5000; }
      else error_=0;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if(!reset_requested_ && has_registrations() && connected_) {
    cleanup_pending_=has_registrations();
    for(auto registered:registry_copy()) if(registered!=kEmpty && !cleanup(registered)) break;
  }
  cleanup_pending_=has_registrations();
  destroy_client();
  active_={}; device_id_.clear(); root_.clear();
}
}  // namespace printdeck::platform
