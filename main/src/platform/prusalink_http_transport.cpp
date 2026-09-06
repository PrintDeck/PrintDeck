#include "printdeck/platform/prusalink_http_transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>

#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "mbedtls/md5.h"
#include "mdns.h"
#include "printdeck/platform/prusalink_http_decoder.hpp"

#if defined(CONFIG_MBEDTLS_DEBUG) && CONFIG_MBEDTLS_DEBUG
#error "PrusaLink credentials require TLS payload tracing to be disabled"
#endif

namespace printdeck::platform {
namespace {

std::timed_mutex transaction_mutex;
std::mutex dns_mutex;
struct DnsSlot {
  std::array<char, 129> name{};
  ip_addr_t address{};
  bool used = false;
  bool waiting = false;
  bool done = false;
  bool found = false;
};
// Late DNS callbacks retain a bounded static slot, never a worker-stack pointer.
// A timed-out caller leaves release to its callback; no slot is prematurely reused.
std::array<DnsSlot, 4> dns_slots;

void dns_complete(const char*, const ip_addr_t* address, void* context) {
  const std::lock_guard<std::mutex> lock(dns_mutex);
  auto& slot = *static_cast<DnsSlot*>(context);
  slot.found = address && IP_IS_V4(address);
  if (slot.found) slot.address = *address;
  slot.done = true;
  if (!slot.waiting) slot.used = false;
}

void dns_begin(void* context) {
  auto& slot = *static_cast<DnsSlot*>(context);
  ip_addr_t address{};
  const auto result = dns_gethostbyname_addrtype(slot.name.data(), &address, dns_complete,
                                                context, LWIP_DNS_ADDRTYPE_IPV4);
  if (result == ERR_OK) dns_complete(nullptr, &address, context);
  else if (result != ERR_INPROGRESS) dns_complete(nullptr, nullptr, context);
}

bool stopped(std::uint64_t deadline, const std::function<bool()>& cancelled) {
  return prusalink_now_ms() >= deadline || (cancelled && cancelled());
}

std::string resolved_ipv4(std::string host, std::uint64_t deadline,
                          const std::function<bool()>& cancelled) {
  ip_addr_t address{};
  if (!ipaddr_aton(host.c_str(), &address)) {
    if (host.ends_with(".local")) {
      host.resize(host.size() - 6);
      esp_ip4_addr_t mdns_address{};
      if (stopped(deadline, cancelled)) return {};
      const auto now = prusalink_now_ms();
      if (now >= deadline) return {};
      const auto budget = static_cast<std::uint32_t>(std::min<std::uint64_t>(1000, deadline - now));
      if (mdns_query_a(host.c_str(), std::max<std::uint32_t>(1, budget), &mdns_address) != ESP_OK) return {};
      ip_addr_set_ip4_u32(&address, mdns_address.addr);
    } else {
      DnsSlot* pending = nullptr;
      {
        const std::lock_guard<std::mutex> lock(dns_mutex);
        for (auto& slot : dns_slots) if (!slot.used) { pending = &slot; break; }
        if (!pending) return {};
        *pending = {};
        pending->used = pending->waiting = true;
        std::memcpy(pending->name.data(), host.data(), host.size());
      }
      if (tcpip_try_callback(dns_begin, pending) != ERR_OK) {
        const std::lock_guard<std::mutex> lock(dns_mutex);
        pending->used = false;
        return {};
      }
      bool found = false;
      while (!stopped(deadline, cancelled)) {
        {
          const std::lock_guard<std::mutex> lock(dns_mutex);
          if (pending->done) { found = pending->found; address = pending->address; break; }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      {
        const std::lock_guard<std::mutex> lock(dns_mutex);
        pending->waiting = false;
        if (pending->done) pending->used = false;
      }
      if (!found) return {};
    }
  }
  if (!IP_IS_V4(&address) || stopped(deadline, cancelled)) return {};
  std::array<char, 16> text{};
  if (!ipaddr_ntoa_r(&address, text.data(), text.size())) return {};
  // A local-looking DNS name must not redirect credentials to a public address.
  return prusalink_origin(text.data()) ? std::string(text.data()) : std::string{};
}

std::string hex(const unsigned char* data, std::size_t size) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) { result += digits[data[i] >> 4]; result += digits[data[i] & 15]; }
  return result;
}

bool retryable(ssize_t result) {
  return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE ||
         (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
}

}  // namespace

std::timed_mutex& prusalink_transaction_mutex() { return transaction_mutex; }
std::uint64_t prusalink_now_ms() { return static_cast<std::uint64_t>(esp_timer_get_time()) / 1000; }

std::string prusalink_md5(std::string_view input) {
  unsigned char digest[16]{};
  if (mbedtls_md5(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest) != 0) return {};
  return hex(digest, sizeof(digest));
}

std::string prusalink_random_cnonce() {
  unsigned char random[16]{};
  esp_fill_random(random, sizeof(random));
  return hex(random, sizeof(random));
}

PrusaLinkHttpResponse PrusaLinkEspTransport::get(const PrusaLinkHttpRequest& request,
                                               const std::function<bool()>& cancelled) {
  if (request.url.size() > 768 || request.maximum_body > 512 * 1024 ||
      request.header_value.size() > 2048 ||
      (!request.header_name.empty() && request.header_name != "Authorization" && request.header_name != "X-Api-Key"))
    return {.error = PrusaLinkError::invalid_configuration};
  const bool secure = request.url.starts_with("https://");
  if (!secure && !request.url.starts_with("http://")) return {.error = PrusaLinkError::invalid_configuration};
  const auto offset = secure ? 8U : 7U;
  const auto slash = request.url.find('/', offset);
  if (slash == std::string::npos) return {.error = PrusaLinkError::invalid_configuration};
  const auto origin = prusalink_origin(std::string_view(request.url).substr(0, slash));
  if (!origin || *origin != request.url.substr(0, slash)) return {.error = PrusaLinkError::invalid_configuration};
  const auto target = request.url.substr(slash);
  if (target.starts_with("//") || target.find_first_of("\r\n #\\") != std::string::npos ||
      target.find('\0') != std::string::npos ||
      (!request.header_value.empty() && !prusalink_credential_valid(request.header_value, 2048)))
    return {.error = PrusaLinkError::invalid_configuration};
  const auto authority = request.url.substr(offset, slash - offset);
  auto host = authority;
  int port = secure ? 443 : 80;
  if (const auto colon = host.find(':'); colon != std::string::npos) {
    port = std::atoi(host.c_str() + colon + 1);
    host.resize(colon);
  }
  const auto address = resolved_ipv4(host, request.deadline_ms, cancelled);
  const auto stopped_error = [&] {
    return cancelled && cancelled() ? PrusaLinkError::cancelled :
           prusalink_now_ms() >= request.deadline_ms ? PrusaLinkError::timeout : PrusaLinkError::unavailable;
  };
  if (address.empty()) return {.error = stopped_error()};
  std::unique_ptr<esp_tls_t, decltype(&esp_tls_conn_destroy)> tls(esp_tls_init(), esp_tls_conn_destroy);
  if (!tls) return {.error = PrusaLinkError::unavailable};
  esp_tls_cfg_t config{};
  config.non_block = true;
  config.is_plain_tcp = !secure;
  config.addr_family = ESP_TLS_AF_INET;
  config.timeout_ms = 1000;
  if (secure) {
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.common_name = host.c_str();
  }
  int connected = 0;
  while (!stopped(request.deadline_ms, cancelled) && connected == 0) {
    connected = esp_tls_conn_new_async(address.c_str(), address.size(), port, &config, tls.get());
    if (connected == 0) vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (connected != 1) return {.error = stopped_error()};
  if (!secure) {
    // ESP-IDF's plain-TCP async path reports success while connect() may
    // still be pending. Confirm writability and SO_ERROR before sending;
    // an immediate send can otherwise fail with ENOTCONN on real hardware.
    int socket = -1;
    if (esp_tls_get_conn_sockfd(tls.get(), &socket) != ESP_OK || socket < 0)
      return {.error = PrusaLinkError::unavailable};
    bool ready = false;
    while (!stopped(request.deadline_ms, cancelled)) {
      fd_set writes;
      FD_ZERO(&writes);
      FD_SET(socket, &writes);
      timeval timeout{.tv_sec = 0, .tv_usec = 20000};
      const int selected = select(socket + 1, nullptr, &writes, nullptr, &timeout);
      if (selected < 0 && errno == EINTR) continue;
      if (selected < 0) return {.error = stopped_error()};
      if (selected == 0) continue;
      int error = 0;
      socklen_t length = sizeof(error);
      if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
        return {.error = stopped_error()};
      ready = true;
      break;
    }
    if (!ready) return {.error = stopped_error()};
  }
  // No esp_http_client/native auth: that component traces plaintext request
  // headers in debug builds. ESP-TLS owns only the socket/TLS record layer here.
  std::string wire = "GET " + target + " HTTP/1.1\r\nHost: " + authority +
      "\r\nUser-Agent: PrintDeck-PrusaLink/1\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
  if (!request.header_name.empty()) wire += request.header_name + ": " + request.header_value + "\r\n";
  wire += "\r\n";
  std::size_t sent = 0;
  while (sent < wire.size() && !stopped(request.deadline_ms, cancelled)) {
    errno = 0;
    const auto size = esp_tls_conn_write(tls.get(), wire.data() + sent, wire.size() - sent);
    if (size > 0) sent += size;
    else if (!retryable(size)) return {.error = stopped_error()};
    else vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (sent != wire.size()) return {.error = stopped_error()};
  wire.clear();
  PrusaLinkHttpDecoder decoder(request.maximum_body);
  std::array<char, 2048> bytes{};
  while (!decoder.complete() && !stopped(request.deadline_ms, cancelled)) {
    errno = 0;
    const auto size = esp_tls_conn_read(tls.get(), bytes.data(), bytes.size());
    if (size > 0) { if (!decoder.feed(std::string_view(bytes.data(), size))) return decoder.take_response(); }
    else if (size == 0) { decoder.finish(); return decoder.take_response(); }
    else if (!retryable(size)) return {.error = stopped_error()};
    else vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!decoder.complete()) return {.error = stopped_error()};
  return decoder.take_response();
}

}  // namespace printdeck::platform
