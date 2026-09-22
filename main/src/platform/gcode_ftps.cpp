#include "printdeck/platform/gcode_ftps.hpp"
#include "printdeck/platform/bambu_trust.hpp"
#include "printdeck/platform/prusalink_http_transport.hpp"
#include "printdeck/core/gcode_source.hpp"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>

namespace printdeck::platform {
namespace {
using Tls = std::unique_ptr<esp_tls_t, decltype(&esp_tls_conn_destroy)>;
bool retryable(ssize_t value) {
  return value == ESP_TLS_ERR_SSL_WANT_READ || value == ESP_TLS_ERR_SSL_WANT_WRITE ||
      (value < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
}
class Transfer {
 public:
  Transfer(const core::PrinterProfile& profile, std::uint64_t deadline,
           const std::function<bool()>& cancelled)
      : profile_(profile), deadline_(deadline), cancelled_(cancelled) {}
  bool stopped() const { return prusalink_now_ms() >= deadline_ || (cancelled_ && cancelled_()); }
  Tls connect(const std::string& address, unsigned port) {
    Tls tls(esp_tls_init(), esp_tls_conn_destroy);
    if (!tls) return tls;
    esp_tls_cfg_t config{};
    config.non_block = true;
    config.timeout_ms = 1000;
    config.addr_family = ESP_TLS_AF_INET;
    config.common_name = profile_.serial.c_str();
    config.cacert_buf = reinterpret_cast<const unsigned char*>(bambu_trust_anchors());
    config.cacert_bytes = std::strlen(bambu_trust_anchors()) + 1;
    config.tls_version = ESP_TLS_VER_TLS_1_2;
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
    config.esp_tls_dyn_buf_strategy = ESP_TLS_DYN_BUF_RX_STATIC;
#endif
    int result = 0;
    while (!stopped() && result == 0) {
      result = esp_tls_conn_new_async(address.c_str(), address.size(), port, &config, tls.get());
      if (!result) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (result != 1) tls.reset();
    return tls;
  }
  bool read(esp_tls_t* tls, char* bytes, std::size_t length) {
    std::size_t done = 0;
    while (done < length && !stopped()) {
      errno = 0;
      const auto count = esp_tls_conn_read(tls, bytes + done, length - done);
      if (count > 0) done += count;
      else if (!retryable(count)) return false;
      else vTaskDelay(pdMS_TO_TICKS(10));
    }
    return done == length;
  }
  bool response(esp_tls_t* tls, unsigned& code, std::string& final) {
    std::string first;
    for (unsigned lines = 0; lines < 32 && !stopped(); ++lines) {
      final.clear();
      while (final.size() < 512) {
        char c = 0;
        if (!read(tls, &c, 1)) return false;
        final += c;
        if (c == '\n') break;
      }
      if (!final.ends_with("\r\n")) return false;
      final.resize(final.size()-2);
      if (lines == 0) {
        if (final.size() < 4 || final[0]<'1'||final[0]>'5'||final[1]<'0'||final[1]>'9'||final[2]<'0'||final[2]>'9') return false;
        first = final.substr(0,3);
        if (final[3] != ' ' && final[3] != '-') return false;
        code = (final[0]-'0')*100+(final[1]-'0')*10+final[2]-'0';
      }
      if (final.starts_with(first+" ")) return true;
    }
    return false;
  }
  bool command(esp_tls_t* tls, const std::string& command, unsigned expected,
               std::string& reply, unsigned alternate = 0) {
    if (command.size() > 768 || command.find_first_of("\r\n") != command.npos) return false;
    const auto wire = command + "\r\n";
    std::size_t sent = 0;
    while (sent < wire.size() && !stopped()) {
      errno = 0;
      const auto count = esp_tls_conn_write(tls, wire.data()+sent, wire.size()-sent);
      if (count > 0) sent += count;
      else if (!retryable(count)) return false;
      else vTaskDelay(pdMS_TO_TICKS(10));
    }
    unsigned code = 0;
    return sent == wire.size() && response(tls,code,reply) && (code == expected || code == alternate);
  }
 private:
  const core::PrinterProfile& profile_;
  std::uint64_t deadline_;
  const std::function<bool()>& cancelled_;
};
}

PrusaLinkHttpResponse gcode_ftps_range(const core::PrinterProfile& profile,
    const std::string& path, std::uint64_t offset, std::uint32_t length,
    std::uint64_t deadline, const std::function<bool()>& cancelled) {
  const PrusaLinkHttpResponse failed{.error=PrusaLinkError::unavailable};
  if (profile.protocol != core::PrinterProtocol::bambu_lan ||
      !prusalink_origin(profile.endpoint) || profile.endpoint.find(':') != profile.endpoint.npos ||
      !prusalink_credential_valid(profile.serial,64) || !prusalink_credential_valid(profile.access_code,128) ||
      !core::gcode_bambu_path(path,{}) || !length || length > core::kGcodeChunkLimit ||
      offset > core::kGcodeFileLimit-length) return failed;
  const auto address = prusalink_resolved_ipv4(profile.endpoint,deadline,cancelled);
  if (address.empty()) return failed;
  Transfer transfer(profile,deadline,cancelled);
  auto control = transfer.connect(address,990);
  std::string reply;
  unsigned code = 0;
  if (!control || !transfer.response(control.get(),code,reply) || code != 220 ||
      !transfer.command(control.get(),"USER bblp",331,reply) ||
      !transfer.command(control.get(),"PASS "+profile.access_code,230,reply) ||
      !transfer.command(control.get(),"PBSZ 0",200,reply) ||
      !transfer.command(control.get(),"PROT P",200,reply) ||
      !transfer.command(control.get(),"TYPE I",200,reply) ||
      !transfer.command(control.get(),"SIZE "+path,213,reply)) return failed;
  std::uint64_t size = 0;
  if (!core::gcode_uint(std::string_view(reply).substr(4),size) || size > core::kGcodeFileLimit || size < offset+length) return failed;
  // A modification timestamp is required before combining independently resumed ranges.
  if (!transfer.command(control.get(),"MDTM "+path,213,reply)) return failed;
  const auto modified = reply.substr(4);
  if (modified.size() < 14 || modified.size() > 24 ||
      !std::all_of(modified.begin(),modified.end(),[](char c){return (c>='0'&&c<='9')||c=='.';})) return failed;
  if (!transfer.command(control.get(),"PASV",227,reply)) return failed;
  const auto port = core::gcode_pasv_port(reply);
  // Ignore the PASV host; both verified TLS channels go to the selected printer address.
  if (!port || !transfer.command(control.get(),"REST "+std::to_string(offset),350,reply) ||
      !transfer.command(control.get(),"RETR "+path,150,reply,125)) return failed;
  auto data = transfer.connect(address,*port);
  std::string body(length,'\0');
  if (!data || !transfer.read(data.get(),body.data(),body.size())) return failed;
  // Closing both channels stops this bounded RETR. No file is modified or queued for printing.
  return {.status=206,.body=std::move(body),.content_range="bytes "+std::to_string(offset)+"-"+
      std::to_string(offset+length-1)+"/"+std::to_string(size),.etag=modified};
}
} // namespace printdeck::platform
