#include "printdeck/platform/cloud_pairing_service.hpp"
#include <array>
#include <string_view>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <charconv>
#include "printdeck/core/printer_command.hpp"
#include "printdeck/core/device_api.hpp"
#include <memory>
#include "printdeck/platform/image_workspace.hpp"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/task_affinity.hpp"
#ifdef PRINTDECK_CLOUD_DEVELOPMENT_HEADER
#include PRINTDECK_CLOUD_DEVELOPMENT_HEADER
#else
#define PRINTDECK_LOCAL_CLOUD_API "http://:8000/api/v1/device"
#define PRINTDECK_LOCAL_CLOUD_PANEL "http://:8000"
#define PRINTDECK_LOCAL_CLOUD_STORAGE "pd_cloud_dev"
#endif

namespace printdeck::platform {
namespace {
// Production origins remain fixed; development addresses are configured separately.
constexpr char kPanelOrigin[] = "https://app.printdeck.xyz";
constexpr char kApi[] = "https://api.printdeck.xyz/api/v1/device";
constexpr char kLocalApi[] = PRINTDECK_LOCAL_CLOUD_API;
constexpr char kLocalPanel[] = PRINTDECK_LOCAL_CLOUD_PANEL;
constexpr char kLocalStorage[] = PRINTDECK_LOCAL_CLOUD_STORAGE;
constexpr bool kDeveloperAvailable = true;
static_assert(std::string_view(kLocalApi).starts_with("http://"));
static_assert(sizeof(kLocalStorage) <= 16 && std::string_view(kLocalStorage) != "pd_cloud");
std::string url_host(std::string_view url) {
  const auto start=url.find("://");
  if(start==std::string_view::npos)return {};
  const auto host=url.substr(start+3);
  return std::string(host.substr(0,host.find_first_of(":/")));
}
unsigned url_port(std::string_view url) {
  const auto start=url.find("://");
  if(start==std::string_view::npos)return 8000;
  const auto authority=url.substr(start+3,url.find('/',start+3)-(start+3));
  const auto colon=authority.find(':');
  if(colon==std::string_view::npos)return url.starts_with("https:")?443:80;
  const auto text=authority.substr(colon+1);
  unsigned port=0;
  const auto result=std::from_chars(text.data(),text.data()+text.size(),port);
  return result.ec==std::errc{} && result.ptr==text.data()+text.size() && port>0 && port<=65535?port:8000;
}
std::string with_host(std::string_view url, const std::string& host, unsigned port = 0) {
  const auto start=url.find("://");
  if(start==std::string_view::npos)return {};
  const auto end=url.find_first_of(":/",start+3);
  const auto path=url.find('/',start+3);
  return std::string(url.substr(0,start+3))+host+(port?":"+std::to_string(port)+
      (path==std::string_view::npos?"":std::string(url.substr(path))):
      (end==std::string_view::npos?"":std::string(url.substr(end))));
}
bool local_host_valid(std::string_view host) {
  std::array<unsigned,4> parts{};
  for(unsigned i=0;i<4;++i) {
    const auto end=host.find('.'); const auto part=host.substr(0,end);
    if(part.empty() || part.size()>3 || (part.size()>1 && part.front()=='0'))return false;
    const auto result=std::from_chars(part.data(),part.data()+part.size(),parts[i]);
    if(result.ec!=std::errc{} || result.ptr!=part.data()+part.size() || parts[i]>255)return false;
    if(i==3) { if(end!=std::string_view::npos)return false; }
    else { if(end==std::string_view::npos)return false; host.remove_prefix(end+1); }
  }
  return parts[0]==10 || (parts[0]==172 && parts[1]>=16 && parts[1]<=31) || (parts[0]==192 && parts[1]==168);
}
const char* storage(bool local) { return local ? kLocalStorage : "pd_cloud"; }
void configure_tls(esp_http_client_config_t& config, bool local) {
  // HTTP is explicit only for the user-selected local development environment.
  if(!local)config.crt_bundle_attach=esp_crt_bundle_attach;
}
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::int64_t millis() { return esp_timer_get_time()/1000; }
std::string field(cJSON* object, const char* name) {
  const auto* item=cJSON_GetObjectItemCaseSensitive(object,name);
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
std::string encode(cJSON* object) {
  char* value=cJSON_PrintUnformatted(object);
  std::string result=value?value:"{}";
  cJSON_free(value);
  return result;
}
bool pairing_code_valid(const std::string& token) {
  return token.size()==60 && token.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")==std::string::npos;
}
bool token_valid(const std::string& token) {
  const auto separator=token.find('|');
  if(separator==std::string::npos || separator==0 || separator>20 || token.front()=='0')return false;
  const auto secret_size=token.size()-separator-1;
  if(secret_size<40 || secret_size>128)return false;
  return token.find_first_not_of("0123456789",0)>=separator &&
         token.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ",separator+1)==std::string::npos;
}
struct Reply { int status=0; std::string body; bool overflow=false; };
void ensure_dns_backup() {
  // lwIP DNS is shared. Keep DHCP's primary/backup choices; add a backup only
  // when Cloud is used and the network has supplied no secondary resolver.
  auto* station=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if(!station)return;
  esp_netif_dns_info_t backup{};
  if(esp_netif_get_dns_info(station,ESP_NETIF_DNS_BACKUP,&backup)!=ESP_OK ||
     !ESP_IP_IS_ANY(backup.ip))return;
  backup.ip.type=ESP_IPADDR_TYPE_V4;
  backup.ip.u_addr.ip4.addr=ESP_IP4TOADDR(1,1,1,1);
  esp_netif_set_dns_info(station,ESP_NETIF_DNS_BACKUP,&backup);
}
esp_err_t on_http(esp_http_client_event_t* event) {
  if(event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0 && event->user_data) {
    auto& reply=*static_cast<Reply*>(event->user_data);
    if(reply.body.size()+static_cast<std::size_t>(event->data_len)>8192) {
      reply.overflow=true; return ESP_FAIL;
    }
    reply.body.append(static_cast<const char*>(event->data),event->data_len);
  }
  return ESP_OK;
}
Reply exchange(esp_http_client_handle_t& client, bool local, const std::string& base, const char* path, const std::string& body, const std::string& token={}, bool remove=false) {
  if(!local)ensure_dns_backup();
  Reply reply;
  const std::string url=base+path;
  esp_http_client_config_t config{};
  config.url=url.c_str(); config.timeout_ms=8000;
  configure_tls(config,local);
  config.disable_auto_redirect=true; config.event_handler=on_http; config.user_data=&reply;
  config.buffer_size=1024; config.buffer_size_tx=1024;
  if (!client) client=esp_http_client_init(&config);
  else {
    esp_http_client_set_url(client, url.c_str());
    esp_http_client_set_user_data(client, &reply);
  }
  if(!client)return reply;
  esp_http_client_set_method(client,remove?HTTP_METHOD_DELETE:HTTP_METHOD_POST);
  esp_http_client_set_header(client,"Content-Type","application/json");
  esp_http_client_set_header(client,"Accept","application/json");
  const std::string bearer="Bearer "+token;
  if(!token.empty())esp_http_client_set_header(client,"Authorization",bearer.c_str());
  esp_http_client_set_post_field(client,body.data(),body.size());
  const auto result=esp_http_client_perform(client);
  const auto status=esp_http_client_get_status_code(client);
  // ESP-IDF returns NOT_SUPPORTED for a completed 401 header without a Basic/Digest
  // challenge. Bearer-token rejection is still authoritative, even without a body.
  if((result==ESP_OK && !reply.overflow) || (result==ESP_ERR_NOT_SUPPORTED && status==401))reply.status=status;
  // The request owns these pointers only until perform returns.
  esp_http_client_set_user_data(client, nullptr);
  esp_http_client_set_post_field(client, nullptr, 0);
  if (reply.status != 200) { esp_http_client_cleanup(client); client=nullptr; }
  return reply;
}
}

CloudPairingService::~CloudPairingService() { close_http(); }
void CloudPairingService::close_http() {
  if (http_client_) { esp_http_client_cleanup(http_client_); http_client_=nullptr; }
}

void CloudPairingService::set_command_sink(CommandSink sink) {
  std::lock_guard lock(mutex_);
  command_sink_ = sink;
}

void CloudPairingService::set_feed_source(FeedSource source, void* context) {
  std::lock_guard lock(mutex_);
  feed_source_ = source;
  feed_context_ = context;
}

void CloudPairingService::set_screen_source(ScreenSource source, ScreenInput input, ScreenBusy busy) {
  std::lock_guard lock(mutex_);
  screen_source_ = source; screen_input_ = input; screen_busy_ = busy;
}

// Called under mutex_. A closed session cannot be revived by a delayed response.
void CloudPairingService::stop_screen() {
  if (!screen_session_.empty()) screen_closed_ = screen_session_;
  screen_session_.clear(); screen_input_id_.clear(); screen_result_.clear();
  screen_expires_ = 0; screen_waiting_ = false;
}

void CloudPairingService::screen_directive(const std::string& payload, std::int64_t started, std::uint32_t input_frame) {
  Json root(cJSON_Parse(payload.c_str()), cJSON_Delete);
  const auto integer = [](cJSON* object, const char* name, double min, double max) -> std::int64_t {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
        std::floor(value->valuedouble) == value->valuedouble && value->valuedouble >= min && value->valuedouble <= max
        ? static_cast<std::int64_t>(value->valuedouble) : -1;
  };
  const auto hex_id = [](const std::string& value) {
    return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
  };
  const auto session = field(root.get(), "session");
  const auto ttl = integer(root.get(), "valid_for_ms", 1, 20000);
  if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), "active")) || !hex_id(session) ||
      ttl < 0 || started + ttl <= millis() || !screen_source_ || paused_ || !online_ || disconnect_) {
    stop_screen(); return;
  }
  if (!screen_session_.empty() && millis() >= screen_expires_) stop_screen();
  if (session == screen_closed_) return;
  if (session != screen_session_) {
    stop_screen(); screen_session_ = session; screen_seq_ = 0; screen_due_ = millis();
  }
  screen_expires_ = started + ttl;
  auto* input = cJSON_GetObjectItemCaseSensitive(root.get(), "input");
  const auto id = field(input, "id");
  const auto input_ttl = integer(input, "valid_for_ms", 1, 8000);
  if (!hex_id(id) || id == screen_input_id_ || screen_waiting_ || input_ttl < 0 || started + input_ttl <= millis()) return;
  screen_input_id_ = id; screen_result_ = "rejected";
  const auto action = field(input, "action");
  const auto frame = integer(input, "frame_seq", 1, UINT32_MAX);
  const auto x = integer(input, "x", 0, 465), y = integer(input, "y", 0, 465);
  const auto end_x = integer(input, "end_x", 0, 465), end_y = integer(input, "end_y", 0, 465);
  if (frame == (input_frame == UINT32_MAX ? screen_seq_ : input_frame) && x >= 0 && y >= 0 && end_x >= 0 && end_y >= 0 &&
      (action == "tap" || action == "long_press" || action == "swipe") && screen_input_ && screen_busy_ &&
      screen_input_(feed_context_, action, x, y, end_x, end_y)) {
    screen_waiting_ = true; screen_result_.clear();
    // Wait for the release to reach LVGL before reporting an applied gesture.
    screen_settle_ = millis() + (action == "long_press" ? 1400 : 600);
  }
}

void CloudPairingService::upload_screen() {
  ScreenSource source;
  void* context;
  std::string session, token, base, result_id, result;
  std::uint32_t generation, seq;
  bool local;
  {
    std::lock_guard lock(mutex_);
    if (screen_session_.empty() || millis() >= screen_expires_ || paused_ || !online_ || disconnect_) {
      stop_screen(); return;
    }
    if (screen_waiting_) {
      if (millis() < screen_settle_ || screen_busy_(feed_context_)) return;
      screen_waiting_ = false; screen_result_ = "applied";
    }
    source = screen_source_; context = feed_context_; session = screen_session_; token = token_;
    generation = generation_; seq = screen_seq_ + 1; local = developer_mode_; base = local ? local_api_ : kApi;
    result_id = screen_input_id_; result = screen_result_;
    screen_due_ = millis() + 750; // Retry a busy shared workspace without queueing captures.
  }
#ifdef ESP_PLATFORM
#if !CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC || CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL != 0
  return; // PNG, HTTP and TLS buffers must use PSRAM, preserving internal memory for Wi-Fi.
#endif
#endif
  ImageWorkspaceLock workspace(0);
  if (!workspace) return;
  const auto internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const auto internal_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (internal_free < 8 * 1024 || internal_block < 3 * 1024 ||
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < 2 * 1024 * 1024 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < 1024 * 1024) {
    ESP_LOGW("cloud_screen", "Capture deferred: internal free=%u, block=%u", unsigned(internal_free), unsigned(internal_block));
    return;
  }
  { std::lock_guard lock(mutex_); screen_due_ = millis() + 5000; }
  const auto started = millis(), deadline = started + 8000;
  const auto current = [&]() {
    std::lock_guard lock(mutex_);
    return !paused_ && online_ && !disconnect_ && generation_ == generation && screen_session_ == session &&
        millis() < screen_expires_ && millis() < deadline;
  };
  std::vector<std::uint8_t> bytes;
  if (!source || !current()) return;
  if (!source(context, bytes)) {
    // Local Web Config may own the shared capture cooldown. Retry between its
    // refreshes instead of repeatedly colliding at exact five-second multiples.
    std::lock_guard lock(mutex_);
    if (generation_ == generation && screen_session_ == session) screen_due_ = millis() + 750;
    return;
  }
  if (bytes.empty() || bytes.size() > 1024 * 1024 || !current()) return;
  const auto url = base + "/live-view/" + session + "/" + std::to_string(seq);
  esp_http_client_config_t config{};
  config.url = url.c_str(); config.timeout_ms = 1500;
  configure_tls(config, local); config.disable_auto_redirect = true;
  config.buffer_size = 1024; config.buffer_size_tx = 1024;
  auto* client = esp_http_client_init(&config);
  if (!client) return;
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "image/png");
  esp_http_client_set_header(client, "Accept", "application/json");
  const auto bearer = std::string("Bearer ") + token;
  esp_http_client_set_header(client, "Authorization", bearer.c_str());
  if (!result.empty()) {
    esp_http_client_set_header(client, "X-Live-Input-Id", result_id.c_str());
    esp_http_client_set_header(client, "X-Live-Input-Result", result.c_str());
  }
  std::size_t sent = 0;
  if (current() && esp_http_client_open(client, static_cast<int>(bytes.size())) == ESP_OK) {
    while (sent < bytes.size() && current()) {
      const auto count = esp_http_client_write(client, reinterpret_cast<const char*>(bytes.data() + sent),
          static_cast<int>(std::min<std::size_t>(512, bytes.size() - sent)));
      if (count <= 0) break;
      sent += static_cast<std::size_t>(count);
      // Leave Wi-Fi time to reclaim its small internal packet buffers between
      // PSRAM-backed writes instead of bursting an entire image into TCP.
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  int status = 0;
  std::string response;
  if (sent == bytes.size() && current() && esp_http_client_fetch_headers(client) >= 0) {
    status = esp_http_client_get_status_code(client);
    std::array<char, 1024> buffer{};
    // fetch_headers may already have parsed the whole response into its raw buffer.
    // Drain that buffer even when is_complete_data_received is already true.
    while (status == 200 && current()) {
      const auto count = esp_http_client_read(client, buffer.data(), buffer.size());
      if (count == 0 && esp_http_client_is_complete_data_received(client)) break;
      if (count <= 0 || response.size() + count > 8192) { status = 0; break; }
      response.append(buffer.data(), count);
    }
  }
  esp_http_client_cleanup(client);
  std::lock_guard lock(mutex_);
  if (session != screen_session_) return;
  if (seq == 1 || status != 200) ESP_LOGI("cloud_screen", "Screen upload: HTTP %d, bytes=%u, elapsed=%u ms", status, unsigned(sent), unsigned(millis() - started));
  if (generation_ != generation || status != 200 || paused_ || !online_ || millis() >= screen_expires_ || millis() >= deadline) {
    stop_screen(); return;
  }
  // A gesture queued while this upload was in flight refers to the frame the
  // viewer saw before it. Validate that exact predecessor, then acknowledge
  // execution only in a later image. Poll replies use the current frame instead.
  const auto input_frame = screen_seq_;
  screen_seq_ = seq;
  Json root(cJSON_Parse(response.c_str()), cJSON_Delete);
  auto* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
  screen_directive(encode(cJSON_GetObjectItemCaseSensitive(data, "live_view")), started, input_frame);
}

void CloudPairingService::set_thumbnail_source(ThumbnailSource source) {
  std::lock_guard lock(mutex_);
  thumbnail_source_ = source;
}

bool CloudPairingService::preview_enabled() const {
  std::lock_guard lock(mutex_);
  return confirmed_ && online_ && !paused_ && !disconnect_ && command_.empty() && !token_.empty();
}

void CloudPairingService::upload_thumbnail(std::uint32_t printer, const std::string& key,
                                           std::uint32_t generation) {
  if (!printer || key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string::npos) return;
  ThumbnailSource source;
  void* context;
  std::string token, base;
  bool local;
  {
    std::lock_guard lock(mutex_);
    if (paused_ || !online_ || disconnect_ || generation_ != generation || result_pending_ || !thumbnail_source_) return;
    if (thumbnail_key_ != key) { thumbnail_key_ = key; thumbnail_attempts_ = 0; }
    if (thumbnail_attempts_ >= 3 || millis() < thumbnail_due_) return;
    source = thumbnail_source_; context = feed_context_; token = token_;
    local = developer_mode_; base=local?local_api_:kApi;
  }
  auto thumbnail = source(context);
  if (thumbnail.printer_id != printer || thumbnail.key != key || !thumbnail.image ||
      thumbnail.image->empty() || thumbnail.image->size() > 512 * 1024) return;
  // No copy, encoder, second connection worker or per-printer pending image queue.
#ifdef ESP_PLATFORM
  // The shipping configuration puts TLS allocations and ordinary HTTP buffers
  // in PSRAM. Do not enable this optional transfer under an internal-only build.
#if !CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC || CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL != 0
  return;
#endif
#endif
  ImageWorkspaceLock workspace(0);
  if (!workspace || heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24 * 1024 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 8 * 1024 ||
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < 384 * 1024 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < 64 * 1024) return;
  {
    std::lock_guard lock(mutex_);
    if (paused_ || !online_ || generation_ != generation || result_pending_) return;
    ++thumbnail_attempts_;
    thumbnail_due_ = millis() + 60000 * thumbnail_attempts_;
  }
  const auto deadline = millis() + 8000;
  const auto current = [&]() {
    { std::lock_guard lock(mutex_);
      if (paused_ || !online_ || generation_ != generation || millis() >= deadline) return false;
    }
    const auto latest = source(context);
    return latest.printer_id == printer && latest.key == key;
  };
  const auto url = base + "/thumbnail/" + std::to_string(printer) + "/" + key;
  esp_http_client_config_t config{};
  config.url = url.c_str(); config.timeout_ms = 1500;
  configure_tls(config,local);
  config.disable_auto_redirect = true;
  config.buffer_size = 1024; config.buffer_size_tx = 1024;
  auto* client = esp_http_client_init(&config);
  if (!client) return;
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
  esp_http_client_set_header(client, "Accept", "application/json");
  const auto bearer = std::string("Bearer ") + token;
  esp_http_client_set_header(client, "Authorization", bearer.c_str());
  std::size_t sent = 0;
  const auto& bytes = *thumbnail.image;
  if (current() && esp_http_client_open(client, static_cast<int>(bytes.size())) == ESP_OK) {
    while (sent < bytes.size() && current()) {
      const auto count = esp_http_client_write(client, reinterpret_cast<const char*>(bytes.data() + sent),
          static_cast<int>(std::min<std::size_t>(2048, bytes.size() - sent)));
      if (count <= 0) break;
      sent += static_cast<std::size_t>(count);
      vTaskDelay(1);
    }
  }
  int status = 0;
  if (sent == bytes.size() && current() && esp_http_client_fetch_headers(client) >= 0)
    status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (status == 204) {
    std::lock_guard lock(mutex_);
    if (generation_ == generation && thumbnail_key_ == key) thumbnail_attempts_ = 3;
  }
  ESP_LOGI("cloud_thumbnail", "Upload %s (%u bytes, HTTP %d)", status == 204 ? "complete" : "deferred",
           static_cast<unsigned>(sent), status);
}

void CloudPairingService::initialize() {
  std::lock_guard lock(mutex_);
  if(initialized_)return;
  initialized_=true;
  local_host_=url_host(kLocalApi); local_port_=url_port(kLocalApi); local_api_=kLocalApi; local_panel_=kLocalPanel;
  nvs_handle_t handle;
  if(kDeveloperAvailable && nvs_open("pd_cloud",NVS_READONLY,&handle)==ESP_OK) {
    std::array<char,256> value{}; std::size_t size=value.size();
    if(nvs_get_str(handle,"environment",value.data(),&size)==ESP_OK) {
      developer_mode_=!local_host_.empty() && std::string_view(value.data())==kLocalStorage;
      Json config(cJSON_Parse(value.data()),cJSON_Delete);
      if(field(config.get(),"scope")==kLocalStorage) {
        const auto host=field(config.get(),"host");
        const auto* port=cJSON_GetObjectItemCaseSensitive(config.get(),"port");
        const bool port_valid=!port || (cJSON_IsNumber(port) && std::isfinite(port->valuedouble) &&
            std::floor(port->valuedouble)==port->valuedouble && port->valuedouble>=1 && port->valuedouble<=65535);
        if(port_valid && !host.empty() && (host==local_host_ || local_host_valid(host))) {
          local_host_=host;
          if(port)local_port_=static_cast<unsigned>(port->valuedouble);
          local_api_=with_host(kLocalApi,host,local_port_);
          local_panel_=port?"http://"+host+":"+std::to_string(local_port_):with_host(kLocalPanel,host);
          developer_mode_=cJSON_IsTrue(cJSON_GetObjectItem(config.get(),"enabled"));
        }
      }
    }
    nvs_close(handle);
  }
  load_credential();
}

void CloudPairingService::load_credential() {
  nvs_handle_t handle;
  if(nvs_open(storage(developer_mode_),NVS_READONLY,&handle)!=ESP_OK)return;
  std::array<char,1024> blob{}; std::size_t size=blob.size();
  const auto result=nvs_get_str(handle,"credential",blob.data(),&size);
  nvs_close(handle);
  if(result!=ESP_OK)return;
  Json root(cJSON_Parse(blob.data()),cJSON_Delete);
  if(developer_mode_) {
    const auto endpoint=field(root.get(),"endpoint");
    if(endpoint.empty()?local_api_!=kLocalApi:endpoint!=local_api_)return;
  }
  const auto token=field(root.get(),"token");
  if(!token_valid(token)) { state_="error"; return; }
  token_=token; account_=field(root.get(),"account");
  disconnect_=cJSON_IsTrue(cJSON_GetObjectItem(root.get(),"disconnect"));
  state_=disconnect_?"disconnecting":"checking";
}

bool CloudPairingService::set_developer_mode(bool enabled, bool& changed, const std::string& host, unsigned port) {
  std::lock_guard lock(mutex_);
  changed=false;
  if(paused_ || !kDeveloperAvailable || port>65535)return false;
  const auto next_host=host.empty()?local_host_:host;
  const auto next_port=port?port:local_port_;
  if(next_host.empty() || (next_host!=url_host(kLocalApi) && !local_host_valid(next_host)))return false;
  if(enabled==developer_mode_ && next_host==local_host_ && next_port==local_port_ &&
     (!port || local_panel_=="http://"+next_host+":"+std::to_string(next_port)))return true;
  Json config(cJSON_CreateObject(),cJSON_Delete);
  if(!config)return false;
  cJSON_AddStringToObject(config.get(),"scope",kLocalStorage);
  cJSON_AddStringToObject(config.get(),"host",next_host.c_str());
  // Missing port preserves legacy split-port configurations until explicitly updated.
  const bool shared_port=port || local_panel_=="http://"+local_host_+":"+std::to_string(local_port_);
  if(shared_port)cJSON_AddNumberToObject(config.get(),"port",next_port);
  cJSON_AddBoolToObject(config.get(),"enabled",enabled);
  const auto serialized=encode(config.get());
  nvs_handle_t handle;
  if(nvs_open("pd_cloud",NVS_READWRITE,&handle)!=ESP_OK)return false;
  auto result=nvs_set_str(handle,"environment",serialized.c_str());
  if(result==ESP_OK)result=nvs_commit(handle);
  nvs_close(handle);
  if(result!=ESP_OK)return false;
  // The worker owns HTTP handles. Its in-flight result is invalidated before
  // loading the other environment; credentials never cross environments.
  ++generation_; developer_mode_=enabled; reset_transport_=true;
  local_host_=next_host;local_port_=next_port;local_api_=with_host(kLocalApi,next_host,next_port);
  local_panel_=shared_port?"http://"+next_host+":"+std::to_string(next_port):with_host(kLocalPanel,next_host);
  command_.clear(); token_.clear(); account_.clear(); secret_.clear(); code_.clear(); link_.clear();
  last_command_id_.clear(); last_command_status_.clear(); thumbnail_key_.clear();
  result_pending_=false; confirmed_=false; disconnect_=false;
  feed_failures_=0; poll_interval_ms_=0; thumbnail_attempts_=0;
  due_=0; expires_=0; feed_due_=0; thumbnail_due_=0; state_="disconnected";
  load_credential(); changed=true;
  return true;
}

bool CloudPairingService::save(const std::string& token, const std::string& account, bool disconnect) {
  nvs_handle_t handle;
  if(nvs_open(storage(developer_mode_),NVS_READWRITE,&handle)!=ESP_OK)return false;
  esp_err_t result;
  if(token.empty()) {
    result=nvs_erase_key(handle,"credential");
    if(result==ESP_ERR_NVS_NOT_FOUND)result=ESP_OK;
  } else {
    Json root(cJSON_CreateObject(),cJSON_Delete);
    if(!root){nvs_close(handle);return false;}
    if(developer_mode_)cJSON_AddStringToObject(root.get(),"endpoint",local_api_.c_str());
    cJSON_AddStringToObject(root.get(),"token",token.c_str());
    cJSON_AddStringToObject(root.get(),"account",account.c_str());
    cJSON_AddBoolToObject(root.get(),"disconnect",disconnect);
    const auto blob=encode(root.get());
    result=nvs_set_str(handle,"credential",blob.c_str());
  }
  if(result==ESP_OK)result=nvs_commit(handle);
  nvs_close(handle);return result==ESP_OK;
}

void CloudPairingService::tick(bool online) {
  std::lock_guard lock(mutex_);
  online_=online;
  if(!paused_ && !task_ && (!token_.empty() || !command_.empty())) {
    // TLS/JSON and persistence stay on core 0; no display callbacks or NVS reads in tick.
    // NVS writes disable the flash cache, so this worker's stack must stay in internal RAM.
    if(xTaskCreatePinnedToCoreWithCaps(entry,"cloud_pair",8192,this,1,&task_,kServiceCore,
                                     MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
      task_=nullptr;state_="error";
    }
  }
}

bool CloudPairingService::request(const std::string& action,const std::string& id,
                                 const std::string& name,const std::string& locale) {
  std::lock_guard lock(mutex_);
  if(paused_ || !command_.empty())return false;
  if(action=="start") {
    if(!token_.empty() || !secret_.empty() || state_=="starting")return false;
    id_=id;name_=name;locale_=locale;state_="starting";
  } else if(action=="cancel") {
    if(!token_.empty())return false;
  } else if(action=="disconnect") {
    if(token_.empty())return false;
  } else return false;
  ++generation_; command_=action; due_=0;
  return true;
}

void CloudPairingService::pause(bool paused) {
  std::lock_guard lock(mutex_);paused_=paused;++generation_;
}

std::string CloudPairingService::status_json() const {
  std::lock_guard lock(mutex_);
  Json root(cJSON_CreateObject(),cJSON_Delete);
  if(!root)return "{}";
  cJSON_AddStringToObject(root.get(),"state",state_.c_str());
  cJSON_AddBoolToObject(root.get(),"paired",!token_.empty());
  cJSON_AddBoolToObject(root.get(),"pairing",command_=="start" || !secret_.empty());
  cJSON_AddStringToObject(root.get(),"account",account_.c_str());
  cJSON_AddStringToObject(root.get(),"user_code",code_.c_str());
  cJSON_AddStringToObject(root.get(),"verification_uri",link_.c_str());
  cJSON_AddStringToObject(root.get(),"panel_uri",developer_mode_?local_panel_.c_str():kPanelOrigin);
  cJSON_AddStringToObject(root.get(),"local_host",local_host_.c_str());
  cJSON_AddNumberToObject(root.get(),"local_port",local_port_);
  cJSON_AddStringToObject(root.get(),"local_default_host",url_host(kLocalApi).c_str());
  cJSON_AddBoolToObject(root.get(),"developer_available",kDeveloperAvailable);
  cJSON_AddBoolToObject(root.get(),"developer_mode",developer_mode_);
  cJSON_AddStringToObject(root.get(),"local_api_uri",local_api_.c_str());
  cJSON_AddStringToObject(root.get(),"local_panel_uri",local_panel_.c_str());
  return encode(root.get());
}

void CloudPairingService::entry(void* context) { static_cast<CloudPairingService*>(context)->run(); }

void CloudPairingService::run() {
  for(;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    step();
  }
}

void CloudPairingService::step() {
    std::unique_lock lock(mutex_);
    if(reset_transport_) { close_http(); stop_screen(); reset_transport_=false; }
    if (paused_ || !online_ || disconnect_ || !command_.empty() || token_.empty() || millis() >= screen_expires_) stop_screen();
    if(paused_) { close_http(); return; }
    if(command_=="cancel") {
      command_.clear();secret_.clear();code_.clear();link_.clear();state_="disconnected";return;
    }
    if(command_=="disconnect") {
      if(!save(token_,account_,true)){state_="error";command_.clear();return;}
      disconnect_=true;command_.clear();state_="disconnecting";
    }
    if(!secret_.empty() && millis()>=expires_) {
      secret_.clear();code_.clear();link_.clear();state_="expired";
    }
    if(command_.empty() && token_.empty() && secret_.empty()) { close_http(); return; }
    if(!online_) { close_http(); state_=disconnect_?"disconnecting":"offline";return;}
    if(millis()<due_) {
      if (!screen_session_.empty() && millis() >= screen_due_) {
        close_http(); lock.unlock(); upload_screen();
      }
      return;
    }
    const auto generation=generation_;
    const auto token=token_;
    const auto local=developer_mode_;
    const std::string base=local?local_api_:kApi, panel_origin=local?local_panel_:kPanelOrigin;
    const bool starting=command_=="start", removing=disconnect_;
    Json payload(cJSON_CreateObject(),cJSON_Delete);
    if(!payload){state_="error";due_=millis()+30000;return;}
    if(starting) {
      cJSON_AddStringToObject(payload.get(),"device_id",id_.c_str());
      const auto label=name_.empty()?std::string(kBoardVariant)+" "+id_.substr(id_.size()>6?id_.size()-6:0):name_;
      cJSON_AddStringToObject(payload.get(),"device_name",label.c_str());
      cJSON_AddStringToObject(payload.get(),"hardware",kBoardVariant);
      cJSON_AddStringToObject(payload.get(),"locale",locale_.c_str());
    } else if(token.empty())cJSON_AddStringToObject(payload.get(),"device_code",secret_.c_str());
    auto body=encode(payload.get());
    const bool feeding=!starting && !removing && !token.empty() && confirmed_ && feed_source_;
    const bool polling=feeding && poll_interval_ms_ && millis()<feed_due_;
    const auto source=feed_source_;
    auto* context=feed_context_;
    const auto* path=feeding?(polling?"/poll":"/feed"):starting?"/pairings":token.empty()?"/pairings/poll":"/connection";
    lock.unlock();
    // Reuse the pairing worker and cached state. No printer I/O, extra worker,
    // pending snapshot queue or NVS writes; fresh state replaces failed uploads.
    if(feeding && !polling)body=source(context);
    lock.lock();
    if(paused_ || generation!=generation_ || !online_)return;
    if(feeding && result_pending_ && !body.empty() && body.back()=='}') {
      body.pop_back();
      if(body.size()>1)body+=',';
      body+="\"command_results\":[{\"id\":\""+last_command_id_+"\",\"status\":\""+last_command_status_+"\"}]}";
    }
    if(feeding && (body.empty() || body.size()>65536)) {
      close_http();
      due_=millis()+300000;
      return;
    }
    lock.unlock();
    const auto exchange_started=millis();
    const auto reply=exchange(http_client_,local,base,path,body,token,removing);
    Json root(cJSON_ParseWithLength(reply.body.data(),reply.body.size()),cJSON_Delete);
    auto* data=cJSON_GetObjectItemCaseSensitive(root.get(),"data");
    lock.lock();
    if(paused_ || generation!=generation_) { close_http(); return; }
    due_=millis()+(token.empty()?5000:60000);
    if(!token.empty() && (reply.status==401 || (removing && reply.status==200 && field(data,"state")=="disconnected"))) {
      if(save("","",false)) {
        stop_screen();
        token_.clear();account_.clear();disconnect_=false;confirmed_=false;feed_failures_=0;
        last_command_id_.clear();last_command_status_.clear();result_pending_=false;
        poll_interval_ms_=0;feed_due_=0;close_http();
        state_=removing?"disconnected":"revoked";
      } else state_="error";
      return;
    }
    if(feeding) {
      if(reply.status==200 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data,"accepted"))) {
        feed_failures_=0;
        screen_directive(encode(cJSON_GetObjectItemCaseSensitive(data,"live_view")), exchange_started);
        const auto interval=[&](const char* name, unsigned fallback, unsigned minimum, unsigned maximum) {
          const auto* value=cJSON_GetObjectItemCaseSensitive(data,name);
          return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
              std::floor(value->valuedouble)==value->valuedouble && value->valuedouble>=minimum && value->valuedouble<=maximum
              ? static_cast<unsigned>(value->valuedouble)*1000U : fallback;
        };
        // Older servers omit next_poll_seconds: retain their feed-only behavior.
        poll_interval_ms_=interval("next_poll_seconds",0,3,30);
        if(!polling) feed_due_=millis()+interval("next_feed_seconds",30000,3,60);
        due_=poll_interval_ms_?std::min(feed_due_,millis()+poll_interval_ms_):feed_due_;
        state_="connected";
        result_pending_=false;
        const auto* commands=cJSON_GetObjectItemCaseSensitive(data,"commands");
        if(command_sink_ && cJSON_IsArray(commands) && cJSON_GetArraySize(commands)==1) {
          auto* item=cJSON_GetArrayItem(commands,0);
          const auto id=field(item,"id");
          const auto* ttl=cJSON_GetObjectItemCaseSensitive(item,"valid_for_ms");
          auto* envelope=cJSON_GetObjectItemCaseSensitive(item,"command");
          const auto payload=encode(envelope);
          core::PrinterCommand parsed;
          core::DeviceCommand device_command;
          const bool valid=!id.empty() && id.size()<=20 && id.front()!='0' &&
              id.find_first_not_of("0123456789")==std::string::npos &&
              cJSON_IsNumber(ttl) && ttl->valuedouble>millis()-exchange_started && ttl->valuedouble<=120000 &&
              std::floor(ttl->valuedouble)==ttl->valuedouble && (core::parse_printer_command(payload,parsed) || (core::parse_device_command(payload,device_command) && ((device_command.action=="device.name.set" || device_command.action=="device.voice.set") || (device_command.action=="device.timezone.set" || device_command.action=="device.unified_api.set" || device_command.action=="device.mqtt.patch" || device_command.action=="device.appearance.patch" || device_command.action=="audio.test" || device_command.action=="firmware.check" || device_command.action=="firmware.install" || device_command.action=="device.reactions.patch" || device_command.action=="reactions.event.set" || device_command.action=="reactions.storage.set"))));
          if(valid) {
            if(id!=last_command_id_) {
              // Shared execution gates validate capabilities and persist device
              // settings before acknowledging; no printer network I/O runs here.
              // Keep the pairing lock so disconnect/OTA cannot race dispatch.
              const bool accepted=command_sink_(feed_context_,payload);
              last_command_id_=id;last_command_status_=accepted?"accepted":"rejected";
            }
            result_pending_=true;
            // Report the receipt and observed state promptly, without another
            // printer read or a parallel Cloud request. Only new transport opts in.
            if(poll_interval_ms_) { feed_due_=millis()+1000; due_=feed_due_; }
          }
        }
        if(!poll_interval_ms_)close_http();
      } else if(polling && (reply.status==404 || reply.status==405)) {
        // A server rollback must not strand a paired device on the newer route.
        poll_interval_ms_=0;feed_due_=0;due_=millis()+30000;close_http();
      } else {
        if(feed_failures_<5)++feed_failures_;
        const auto delay=std::min(300000U,30000U << (feed_failures_-1));
        due_=millis()+delay;
        state_="offline";close_http();
      }
      if (!polling && reply.status == 200 && !result_pending_ && screen_session_.empty()) {
        auto* thumbnail = cJSON_GetObjectItemCaseSensitive(data, "thumbnail");
        auto* printer = cJSON_GetObjectItemCaseSensitive(thumbnail, "printer_id");
        const auto key = field(thumbnail, "key");
        if (cJSON_IsNumber(printer) && printer->valuedouble >= 1 && printer->valuedouble <= UINT32_MAX &&
            std::floor(printer->valuedouble) == printer->valuedouble) {
          const auto id = static_cast<std::uint32_t>(printer->valuedouble);
          close_http(); // Never retain a second TLS connection beside a thumbnail upload.
          lock.unlock();
          std::string{}.swap(body); // Release the feed buffer before opening another TLS connection.
          upload_thumbnail(id, key, generation);
        }
      }
      // Regular snapshots remain independent of the bounded command poll.
      return;
    }
    close_http(); // Pairing/connection checks do not retain idle TLS state.
    if(reply.status!=200 || !cJSON_IsObject(data)) {
      due_=millis()+30000;state_=removing?"disconnecting":"offline";
      if(starting && reply.status>=400 && reply.status<500 && reply.status!=429) {
        command_.clear();state_="error";
      }
      return;
    }
    if(starting) {
      const auto secret=field(data,"device_code"),code=field(data,"user_code"),link=field(data,"verification_uri");
      const auto expected=panel_origin+"/cloud/pair/"+code;
      command_.clear();
      if(!pairing_code_valid(secret) || code.size()!=16 || code.find_first_not_of("0123456789ABCDEF")!=std::string::npos || link!=expected) {
        state_="error";return;
      }
      secret_=secret;code_=code;link_=link;expires_=millis()+600000;state_="pending";
    } else if(!token.empty()) {
      if(field(data,"state")!="connected") {state_="error";return;}
      const auto account=field(data,"account");
      if(account.size()>254){state_="error";return;}
      if(account!=account_ && !save(token_,account,false)){state_="error";return;}
      account_=account;state_="connected";confirmed_=true;feed_failures_=0;
      if(feed_source_)due_=millis()+1000;
    } else {
      const auto result=field(data,"state");
      if(result=="authorized") {
        const auto issued=field(data,"token");
        if(!token_valid(issued) || !save(issued,"",false)){state_="error";return;}
        token_=issued;secret_.clear();code_.clear();link_.clear();state_="checking";due_=0;
      } else if(result=="expired" || result=="denied") {
        secret_.clear();code_.clear();link_.clear();state_=result;
      } else state_=result=="pending" || result=="slow_down"?"pending":"error";
    }
}
} // namespace printdeck::platform
