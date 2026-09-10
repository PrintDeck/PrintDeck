#include "printdeck/platform/elegoo_sdcp_service.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <ctime>
#include "printdeck/platform/uniformation_sdcp_parser.hpp"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "mdns.h"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {
namespace {

using Cancel = std::function<bool()>;
std::uint64_t now_ms() { return static_cast<std::uint64_t>(esp_timer_get_time()) / 1000; }
bool done(std::uint64_t deadline, const Cancel& cancelled) {
  return now_ms() >= deadline || (cancelled && cancelled());
}
bool transient() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

// Fixed, callback-owned DNS slots survive cancelled workers. No late callback
// can reference stack storage or a slot reused by a different request.
struct DnsQuery {
  std::array<char, 129> host{};
  ip_addr_t address{};
  bool busy = false, waiting = false, finished = false, found = false;
};
std::mutex dns_mutex;
std::array<DnsQuery, 2> dns_queries;
void dns_reply(const char*, const ip_addr_t* address, void* context) {
  const std::lock_guard<std::mutex> lock(dns_mutex);
  auto& query = *static_cast<DnsQuery*>(context);
  query.found = address && IP_IS_V4(address);
  if (query.found) query.address = *address;
  query.finished = true;
  if (!query.waiting) query.busy = false;
}
void dns_request(void* context) {
  auto* query = static_cast<DnsQuery*>(context);
  ip_addr_t address{};
  const auto result = dns_gethostbyname_addrtype(query->host.data(), &address, dns_reply, query, LWIP_DNS_ADDRTYPE_IPV4);
  if (result == ERR_OK) dns_reply(nullptr, &address, query);
  else if (result != ERR_INPROGRESS) dns_reply(nullptr, nullptr, query);
}
std::string resolve(std::string host, std::uint64_t deadline, const Cancel& cancelled) {
  ip_addr_t address{};
  if (!ipaddr_aton(host.c_str(), &address)) {
    if (host.ends_with(".local")) {
      host.resize(host.size() - 6);
      esp_ip4_addr_t result{};
      if (done(deadline, cancelled)) return {};
      const auto remaining = std::min<std::uint64_t>(1000, deadline - now_ms());
      if (mdns_query_a(host.c_str(), static_cast<std::uint32_t>(std::max<std::uint64_t>(1, remaining)), &result) != ESP_OK) return {};
      ip_addr_set_ip4_u32(&address, result.addr);
    } else {
      DnsQuery* query = nullptr;
      {
        const std::lock_guard<std::mutex> lock(dns_mutex);
        for (auto& slot : dns_queries) if (!slot.busy) { query = &slot; break; }
        if (!query) return {};
        *query = {}; query->busy = query->waiting = true;
        std::copy(host.begin(), host.end(), query->host.begin());
      }
      if (tcpip_try_callback(dns_request, query) != ERR_OK) {
        const std::lock_guard<std::mutex> lock(dns_mutex); query->busy = false; return {};
      }
      bool found = false;
      while (!done(deadline, cancelled)) {
        { const std::lock_guard<std::mutex> lock(dns_mutex);
          if (query->finished) { found = query->found; address = query->address; break; } }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      { const std::lock_guard<std::mutex> lock(dns_mutex);
        query->waiting = false; if (query->finished) query->busy = false; }
      if (!found) return {};
    }
  }
  if (done(deadline, cancelled) || !IP_IS_V4(&address)) return {};
  std::array<char, 16> output{};
  if (!ipaddr_ntoa_r(&address, output.data(), output.size())) return {};
  // Revalidate the result: a local-looking name must not resolve outside LAN.
  return elegoo_sdcp_endpoint(output.data()) ? std::string(output.data()) : std::string{};
}

class Socket {
 public:
  ~Socket() { close(); }
  void close() { if (value >= 0) { ::shutdown(value, SHUT_RDWR); ::close(value); value = -1; } }
  int value = -1;
};
bool wait_socket(int socket, bool write, std::uint64_t deadline, const Cancel& cancelled) {
  while (!done(deadline, cancelled)) {
    fd_set set; FD_ZERO(&set); FD_SET(socket, &set);
    timeval delay{0, 20000};
    const int result = select(socket + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &delay);
    if (result > 0) return !done(deadline, cancelled);
    if (result < 0 && errno != EINTR) return false;
  }
  return false;
}
bool read_exact(int socket, char* output, std::size_t size, std::uint64_t deadline, const Cancel& cancelled) {
  while (size && !done(deadline, cancelled)) {
    const auto count = recv(socket, output, size, 0);
    if (count > 0) { output += count; size -= static_cast<std::size_t>(count); }
    else if (count == 0 || !transient() || !wait_socket(socket, false, deadline, cancelled)) return false;
  }
  return !size && !done(deadline, cancelled);
}
bool write_exact(int socket, std::string_view input, std::uint64_t deadline, const Cancel& cancelled) {
  while (!input.empty() && !done(deadline, cancelled)) {
    const auto count = send(socket, input.data(), input.size(), 0);
    if (count > 0) input.remove_prefix(static_cast<std::size_t>(count));
    else if (count == 0 || !transient() || !wait_socket(socket, true, deadline, cancelled)) return false;
  }
  return input.empty() && !done(deadline, cancelled);
}
sockaddr_in address_of(const std::string& address, std::uint16_t port) {
  sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(port);
  inet_pton(AF_INET, address.c_str(), &peer.sin_addr);
  return peer;
}
bool discover_one(const std::string& address, ElegooIdentity& identity, std::string& outer,
                  std::uint64_t deadline, const Cancel& cancelled) {
  Socket socket;
  socket.value = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket.value < 0 || fcntl(socket.value, F_SETFL, O_NONBLOCK) < 0) return false;
  const auto peer = address_of(address, 3000);
  std::uint64_t next_send = 0;
  unsigned attempts = 0;
  std::array<char, 2048> input{};
  while (!done(deadline, cancelled)) {
    if (attempts < 2 && now_ms() >= next_send) {
      constexpr std::string_view query = "M99999";
      if (sendto(socket.value, query.data(), query.size(), 0, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) != static_cast<ssize_t>(query.size())) return false;
      ++attempts; next_send = now_ms() + 800;
    }
    sockaddr_in sender{}; socklen_t length = sizeof(sender);
    const auto count = recvfrom(socket.value, input.data(), input.size(), 0, reinterpret_cast<sockaddr*>(&sender), &length);
    if (count > 0 && count < static_cast<ssize_t>(input.size()) && sender.sin_family == AF_INET &&
        sender.sin_addr.s_addr == peer.sin_addr.s_addr && sender.sin_port == peer.sin_port &&
        elegoo_sdcp_decode_discovery(std::string_view(input.data(), count), identity, &outer)) return true;
    if (count < 0 && !transient()) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return false;
}

std::string base64(const unsigned char* input, std::size_t size) {
  std::array<unsigned char, 64> output{}; std::size_t length = 0;
  if (mbedtls_base64_encode(output.data(), output.size(), &length, input, size) != 0) return {};
  return std::string(reinterpret_cast<char*>(output.data()), length);
}

std::mutex leases_mutex;
std::array<std::string, 4> leased_hosts;
class HostLease {
 public:
  explicit HostLease(const std::string& address) {
    const std::lock_guard<std::mutex> lock(leases_mutex);
    for (const auto& host : leased_hosts) if (host == address) return;
    for (auto& host : leased_hosts) if (host.empty()) { host = address; slot_ = &host; break; }
  }
  ~HostLease() {
    const std::lock_guard<std::mutex> lock(leases_mutex);
    if (slot_) slot_->clear();
  }
  bool held() const { return slot_ != nullptr; }
 private:
  std::string* slot_ = nullptr;
};

// The history preview path is derived solely from the current task UUID and
// the already verified local peer. No redirects, arbitrary paths or credentials.
std::shared_ptr<std::vector<std::uint8_t>> fetch_resin_preview(
    const std::string& address, std::uint16_t port, const std::string& task,
    const Cancel& cancelled) {
  if (!uniformation_valid_task_id(task) || cancelled()) return {};
  const std::string url = "http://" + address + ":" + std::to_string(port) +
      "/media/emmc0/history_image/" + task + ".bmp";
  esp_http_client_config_t config{};
  config.url = url.c_str(); config.timeout_ms = 500;
  config.disable_auto_redirect = true; config.buffer_size = 1024;
  const auto client = esp_http_client_init(&config);
  if (!client) return {};
  std::unique_ptr<std::remove_pointer_t<esp_http_client_handle_t>, decltype(&esp_http_client_cleanup)>
      guard(client, esp_http_client_cleanup);
  esp_http_client_set_header(client, "Connection", "close");
  if (esp_http_client_open(client, 0) != ESP_OK) return {};
  const auto length = esp_http_client_fetch_headers(client);
  if (esp_http_client_get_status_code(client) != 200 || length < 0 || length > 1048576) return {};
  const auto deadline = now_ms() + 3000;
  auto bytes = std::make_shared<std::vector<std::uint8_t>>();
  if (length > 0) bytes->reserve(static_cast<std::size_t>(length));
  std::array<char, 2048> buffer{};
  while (!done(deadline, cancelled)) {
    const int count = esp_http_client_read(client, buffer.data(), buffer.size());
    if (count < 0) return {};
    if (count == 0) break;
    if (bytes->size() + count > 1048576) return {};
    bytes->insert(bytes->end(), buffer.data(), buffer.data() + count);
  }
  if (done(deadline, cancelled) || !esp_http_client_is_complete_data_received(client)) return {};
  std::vector<std::uint8_t> decoded; std::uint16_t width = 0, height = 0;
  if (!uniformation_decode_preview_bmp(*bytes, decoded, width, height)) return {};
  return bytes;
}

std::optional<UniformationLayerExecution> fetch_resin_execution(
    const std::string& address, std::uint16_t port, const Cancel& cancelled) {
  if (cancelled()) return {};
  const std::string url = "http://" + address + ":" + std::to_string(port) + "/customer/resources/log";
  esp_http_client_config_t config{};
  config.url = url.c_str(); config.timeout_ms = 300; config.buffer_size = 1024;
  config.disable_auto_redirect = true; config.method = HTTP_METHOD_HEAD;
  auto client = esp_http_client_init(&config);
  if (!client) return {};
  std::unique_ptr<std::remove_pointer_t<esp_http_client_handle_t>, decltype(&esp_http_client_cleanup)>
      guard(client, esp_http_client_cleanup);
  const auto deadline = now_ms() + 1200;
  esp_http_client_set_header(client, "Connection", "close");
  if (esp_http_client_open(client, 0) != ESP_OK) return {};
  const auto size = esp_http_client_fetch_headers(client);
  if (esp_http_client_get_status_code(client) != 200 || size <= 0 || size > 536870912 || done(deadline, cancelled)) return {};
  esp_http_client_close(client);
  // This firmware ignores suffix ranges. Use explicit offsets and reject a
  // full-file response or rotation, keeping every read at most 32 KiB.
  const auto offset = std::max<std::int64_t>(0, size - 32768);
  const auto range = "bytes=" + std::to_string(offset) + "-" + std::to_string(size - 1);
  esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_http_client_set_header(client, "Range", range.c_str());
  if (esp_http_client_open(client, 0) != ESP_OK) return {};
  const auto length = esp_http_client_fetch_headers(client);
  if (esp_http_client_get_status_code(client) != 206 || length != size - offset ||
      length <= 0 || length > 32768) return {};
  std::string tail; tail.reserve(static_cast<std::size_t>(length));
  std::array<char, 1024> bytes{};
  while (!done(deadline, cancelled)) {
    const int count = esp_http_client_read(client, bytes.data(), bytes.size());
    if (count < 0) return {};
    if (count == 0) break;
    if (tail.size() + count > 32768) return {};
    tail.append(bytes.data(), count);
  }
  if (done(deadline, cancelled) || !esp_http_client_is_complete_data_received(client)) return {};
  return uniformation_layer_execution(tail);
}

// This intentionally narrow RFC6455 transport never follows HTTP redirects,
// negotiates compression, or accepts an unsolicited protocol extension. Keeping
// it on the service worker also avoids a second full WebSocket task/queue.
class Session {
 public:
  bool open(const core::PrinterProfile& profile, bool allow_discovery, std::uint64_t deadline, const Cancel& cancelled) {
    const auto endpoint = elegoo_sdcp_endpoint(profile.endpoint);
    uniformation_ = profile.protocol == core::PrinterProtocol::uniformation_sdcp;
    if ((!uniformation_ && profile.protocol != core::PrinterProtocol::elegoo_sdcp) || !endpoint ||
        (!profile.serial.empty() && !elegoo_sdcp_valid_mainboard_id(profile.serial)) ||
        (profile.serial.empty() && !allow_discovery)) { error_ = ElegooError::invalid_configuration; return false; }
    const auto address = resolve(endpoint->host, deadline, cancelled);
    address_ = address; port_ = endpoint->port; live_ = !allow_discovery;
    if (address.empty()) { error_ = ElegooError::unavailable; return false; }
    lease_ = std::make_unique<HostLease>(address);
    if (!lease_->held()) { error_ = ElegooError::local_busy; return false; }
    ElegooIdentity discovered;
    std::string serial = profile.serial;
    // Restore the independent outer routing ID on reconnect as well as setup.
    // This is one bounded unicast identity request to the configured address,
    // never subnet discovery. Firmware without UDP may still supply Cmd1 attrs.
    const bool found = !uniformation_ && discover_one(address, discovered, outer_id_,
        std::min(deadline, now_ms() + (serial.empty() ? 2500 : 1200)), cancelled);
    if (serial.empty() && !uniformation_) {
      if (!found) {
        error_ = done(deadline, cancelled) ? ElegooError::timeout : ElegooError::unsupported_response; return false;
      }
      serial = discovered.serial;
    }
    serial_ = serial;
    if (uniformation_) {
      uniformation_parser_ = std::make_unique<UniformationSdcpParser>(profile.id, serial);
      char client_id[33];
      std::snprintf(client_id, sizeof(client_id), "%08x%08x%08x%08x",
          static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()),
          static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()));
      outer_id_ = client_id;
    } else {
      parser_ = std::make_unique<ElegooSdcpParser>(profile.id, serial);
      if (!discovered.serial.empty() && !parser_->seed_identity(discovered)) { error_ = parser_->error(); return false; }
    }
    socket_.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_.value < 0 || fcntl(socket_.value, F_SETFL, O_NONBLOCK) < 0) { error_ = ElegooError::unavailable; return false; }
    const auto peer = address_of(address, endpoint->port);
    const int result = connect(socket_.value, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    if (result < 0 && (errno != EINPROGRESS || !wait_socket(socket_.value, true, deadline, cancelled))) { error_ = ElegooError::unavailable; return false; }
    int code = 0; socklen_t size = sizeof(code);
    if (getsockopt(socket_.value, SOL_SOCKET, SO_ERROR, &code, &size) != 0 || code) { error_ = ElegooError::unavailable; return false; }
    std::array<unsigned char, 16> nonce{}; esp_fill_random(nonce.data(), nonce.size());
    const auto key = base64(nonce.data(), nonce.size());
    const auto challenge = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char, 20> digest{};
    if (key.empty() || mbedtls_sha1(reinterpret_cast<const unsigned char*>(challenge.data()), challenge.size(), digest.data()) != 0) {
      error_ = ElegooError::unavailable; return false;
    }
    const std::string handshake_request = "GET /websocket HTTP/1.1\r\nHost: " + endpoint->host + ":" + std::to_string(endpoint->port) +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
        "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: PrintDeck\r\n\r\n";
    const auto handshake_deadline = std::min(deadline, now_ms() + 2500);
    if (!write_exact(socket_.value, handshake_request, handshake_deadline, cancelled)) { error_ = ElegooError::unavailable; return false; }
    std::string headers; headers.reserve(1024);
    while (!headers.ends_with("\r\n\r\n") && headers.size() < 4096) {
      char value = 0;
      if (!read_exact(socket_.value, &value, 1, handshake_deadline, cancelled)) { error_ = ElegooError::unavailable; return false; }
      headers += value;
    }
    if (!elegoo_sdcp_valid_upgrade(headers, base64(digest.data(), digest.size()))) {
      // HTTP500 is not necessarily capacity: do not call every server failure a full client pool.
      error_ = headers.starts_with("HTTP/1.1 401 ") || headers.starts_with("HTTP/1.1 403 ") ? ElegooError::authorization : ElegooError::unsupported_response;
      return false;
    }
    started_at_ = now_ms();
    request_id_ = (static_cast<std::uint64_t>(esp_random()) << 32) | esp_random();
    if (!request(1, cancelled)) return false;
    next_status_ = now_ms() + 100;
    next_attributes_ = now_ms() + 5000;
    return true;
  }

  bool step(const Cancel& cancelled) {
    if (cancelled && cancelled()) { error_ = ElegooError::cancelled; return false; }
    const auto now = now_ms();
    if (now >= next_status_ && (!uniformation_ || !identity().serial.empty())) {
      if (!request(0, cancelled)) return false;
      next_status_ = now_ms() + (uniformation_ && live_ && uniformation_parser_->active_layer_cycle() ? 500 : 2000);
    } else if (identity().serial.empty() && now >= next_attributes_) {
      if (!request(1, cancelled)) return false;
      next_attributes_ = now_ms() + 5000;
    }
    if (!receive(cancelled)) return false;
    if (uniformation_ && live_ && ready(now_ms())) {
      const auto& task = uniformation_parser_->task_id();
      if (task != details_task_) { details_task_ = task; next_details_ = 0; }
      if (!task.empty() && !uniformation_parser_->task_details_known() && now_ms() >= next_details_) {
        if (!request(321, cancelled)) return false;
        next_details_ = now_ms() + 30000;
      }
      const auto layer = uniformation_parser_->current_layer();
      if (task != execution_task_ || layer != execution_layer_) {
        execution_task_ = task; execution_layer_ = layer;
        execution_attempts_ = 0; next_execution_ = 0;
      }
      if (execution_attempts_ < 3 && now_ms() >= next_execution_ &&
          uniformation_parser_->needs_layer_execution(now_ms())) {
        ++execution_attempts_; next_execution_ = now_ms() + 1500;
        if (const auto execution = fetch_resin_execution(address_, port_, cancelled))
          uniformation_parser_->ingest_layer_execution(*execution, now_ms());
      }
    }
    if (parser_error() != ElegooError::none) { error_ = parser_error(); return false; }
    const auto latest = last_status_ms() ? last_status_ms() : started_at_;
    if (now_ms() - latest >= kElegooSdcpStatusLifetimeMs ||
        (!ready(now_ms()) && now_ms() - started_at_ >= kElegooSdcpStatusLifetimeMs)) {
      error_ = ElegooError::timeout; return false;
    }
    return true;
  }
  ElegooError error() const { return error_; }
  bool ready(std::uint64_t now) const { return uniformation_ ? uniformation_parser_->ready(now) : parser_->ready(now); }
  const ElegooIdentity& identity() const { return uniformation_ ? uniformation_parser_->identity() : parser_->identity(); }
  ElegooError parser_error() const { return uniformation_ ? uniformation_parser_->error() : parser_->error(); }
  std::uint64_t last_status_ms() const { return uniformation_ ? uniformation_parser_->last_status_ms() : parser_->last_status_ms(); }
  std::shared_ptr<std::vector<std::uint8_t>> preview(const std::string& task, const Cancel& cancelled) const {
    return fetch_resin_preview(address_, port_, task, cancelled);
  }
  void snapshot_into(core::PrinterSnapshot& out, std::uint64_t now) const {
    if (uniformation_) uniformation_parser_->snapshot_into(out, now); else parser_->snapshot_into(out, now);
  }
  bool control(core::ResinControl action, const Cancel& cancelled) {
    if (!uniformation_ || !live_ || !ready(now_ms())) return false;
    const auto body = uniformation_sdcp_control_request(action, identity().serial,
        outer_id_, ++request_id_, static_cast<std::uint64_t>(std::time(nullptr)));
    if (!body || !send_frame(1, *body, cancelled)) return false;
    next_status_ = now_ms() + 100;
    return true;
  }
 private:
  bool send_frame(unsigned opcode, std::string_view payload, const Cancel& cancelled) {
    if (payload.size() > 512) { error_ = ElegooError::invalid_configuration; return false; }
    std::array<unsigned char, 4> mask{}; esp_fill_random(mask.data(), mask.size());
    std::string packet; packet.reserve(payload.size() + 8);
    packet += static_cast<char>(0x80 | opcode);
    if (payload.size() < 126) packet += static_cast<char>(0x80 | payload.size());
    else { packet += static_cast<char>(0xfe); packet += static_cast<char>(payload.size() >> 8); packet += static_cast<char>(payload.size()); }
    packet.append(reinterpret_cast<const char*>(mask.data()), mask.size());
    for (std::size_t i = 0; i < payload.size(); ++i) packet += static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
    if (!write_exact(socket_.value, packet, now_ms() + 500, cancelled)) { error_ = ElegooError::unavailable; return false; }
    return true;
  }
  bool request(unsigned command, const Cancel& cancelled) {
    const auto body = uniformation_
        ? uniformation_sdcp_read_request(command, identity().serial, outer_id_, ++request_id_, static_cast<std::uint64_t>(std::time(nullptr)), command == 321 ? uniformation_parser_->task_id() : "")
        : elegoo_sdcp_read_request(command, serial_, ++request_id_, now_ms(), outer_id_);
    if (!body) { error_ = ElegooError::invalid_configuration; return false; }
    return send_frame(1, *body, cancelled);
  }
  bool receive(const Cancel& cancelled) {
    // A quiet socket is not a transport failure. Once a frame starts, its entire
    // bounded payload must arrive promptly, preventing slow partial-frame stalls.
    if (!wait_socket(socket_.value, false, now_ms() + 20, cancelled)) return !(cancelled && cancelled());
    const auto deadline = now_ms() + 1500;
    std::array<char, 8> header{};
    if (!read_exact(socket_.value, header.data(), 2, deadline, cancelled)) { error_ = ElegooError::unavailable; return false; }
    const unsigned flags = static_cast<unsigned char>(header[0]);
    const unsigned length_code = static_cast<unsigned char>(header[1]);
    const unsigned opcode = flags & 15;
    const bool final = flags & 128;
    if ((flags & 112) || (length_code & 128) || (opcode != 0 && opcode != 1 && opcode != 8 && opcode != 9 && opcode != 10)) {
      error_ = ElegooError::unsupported_response; return false;
    }
    std::size_t length = length_code & 127;
    if (length == 126) {
      if (!read_exact(socket_.value, header.data(), 2, deadline, cancelled)) { error_ = ElegooError::unavailable; return false; }
      length = (static_cast<unsigned char>(header[0]) << 8) | static_cast<unsigned char>(header[1]);
      if (length < 126) { error_ = ElegooError::unsupported_response; return false; }
    } else if (length == 127) {
      // Every accepted message is <=16KB, so any 64-bit frame length is non-minimal or oversized.
      error_ = ElegooError::unsupported_response; return false;
    }
    if (length > kElegooSdcpMaximumMessage || (opcode >= 8 && (!final || length > 125 || (opcode == 8 && length == 1)))) {
      error_ = ElegooError::unsupported_response; return false;
    }
    std::array<char, 1024> bytes{};
    std::size_t offset = 0;
    do {
      const auto count = std::min(bytes.size(), length - offset);
      if (count && !read_exact(socket_.value, bytes.data(), count, deadline, cancelled)) { error_ = ElegooError::unavailable; return false; }
      if (opcode >= 8) {
        if (opcode == 8) {
          const unsigned code = length >= 2 ? (static_cast<unsigned char>(bytes[0]) << 8) | static_cast<unsigned char>(bytes[1]) : 1000;
          error_ = code == 1013 ? ElegooError::capacity : ElegooError::unavailable;
          return false;
        }
        if (opcode == 9 && !send_frame(10, std::string_view(bytes.data(), count), cancelled)) return false;
      } else {
        const auto result = frames_.append(opcode, final, length, offset, std::string_view(bytes.data(), count));
        if (result == ElegooSdcpFrames::Result::invalid) { error_ = ElegooError::unsupported_response; return false; }
        if (result == ElegooSdcpFrames::Result::complete) {
          auto message = frames_.take();
          const auto parsed = uniformation_ ? uniformation_parser_->ingest(message, now_ms()) : parser_->ingest(message, now_ms());
          if (parsed == ElegooSdcpMessage::invalid) { error_ = parser_error(); return false; }
          if (uniformation_ && !uniformation_parser_->outer_id().empty()) outer_id_ = uniformation_parser_->outer_id();
        }
      }
      offset += count;
    } while (offset < length);
    return true;
  }
  // Destruction order closes the socket before releasing its host lease.
  std::unique_ptr<HostLease> lease_;
  Socket socket_;
  std::unique_ptr<ElegooSdcpParser> parser_;
  std::unique_ptr<UniformationSdcpParser> uniformation_parser_;
  bool uniformation_ = false, live_ = false;
  std::string address_, details_task_;
  std::uint16_t port_ = 3030;
  std::uint64_t next_details_ = 0;
  std::string execution_task_;
  std::uint16_t execution_layer_ = 0;
  unsigned execution_attempts_ = 0;
  std::uint64_t next_execution_ = 0;
  ElegooSdcpFrames frames_;
  std::string serial_, outer_id_;
  std::uint64_t started_at_ = 0, request_id_ = 0, next_status_ = 0, next_attributes_ = 0;
  ElegooError error_ = ElegooError::none;
};

}  // namespace

static ElegooPollResult sdcp_probe(const core::PrinterProfile& profile, std::uint64_t deadline,
    const std::function<bool()>& cancelled, ElegooIdentity* identity) {
  ElegooPollResult result;
  const auto stop = [&] { return done(deadline, cancelled); };
  auto session = std::make_unique<Session>();
  if (!session->open(profile, true, deadline, stop)) {
    result.error = cancelled && cancelled() ? ElegooError::cancelled : now_ms() >= deadline ? ElegooError::timeout : session->error(); return result;
  }
  while (!done(deadline, cancelled)) {
    if (!session->step(stop)) {
      result.error = cancelled && cancelled() ? ElegooError::cancelled : now_ms() >= deadline ? ElegooError::timeout : session->error(); return result;
    }
    if (session->ready(now_ms())) {
      result.snapshot.emplace();
      session->snapshot_into(*result.snapshot, now_ms());
      if (identity) *identity = session->identity();
      return result;
    }
  }
  result.error = cancelled && cancelled() ? ElegooError::cancelled : ElegooError::timeout;
  return result;
}

ElegooPollResult elegoo_sdcp_probe(const core::PrinterProfile& profile, std::uint64_t deadline,
    const std::function<bool()>& cancelled, ElegooIdentity* identity) {
  if (profile.protocol != core::PrinterProtocol::elegoo_sdcp) return {.error = ElegooError::invalid_configuration};
  return sdcp_probe(profile, deadline, cancelled, identity);
}
ElegooPollResult uniformation_sdcp_probe(const core::PrinterProfile& profile, std::uint64_t deadline,
    const std::function<bool()>& cancelled, ElegooIdentity* identity) {
  if (profile.protocol != core::PrinterProtocol::uniformation_sdcp) return {.error = ElegooError::invalid_configuration};
  return sdcp_probe(profile, deadline, cancelled, identity);
}

void ElegooSdcpAdapter::configure(const core::PrinterProfile* profile) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const core::PrinterProfile next = profile && (profile->protocol == core::PrinterProtocol::elegoo_sdcp ||
      profile->protocol == core::PrinterProtocol::uniformation_sdcp) ? *profile : core::PrinterProfile{};
  if (core::same_printer_connection(profile_, next)) return;
  profile_ = next; ++generation_;
  controls_live_ = false;
  if (controls_) xQueueReset(controls_);
  snapshots_.invalidate(next.id, next.id ? core::LinkState::connecting : core::LinkState::stopped);
  if (task_) xTaskNotifyGive(task_);
}
esp_err_t ElegooSdcpAdapter::start(const core::PrinterProfile* profile, const NetworkService& network) {
  configure(profile);
  const std::lock_guard<std::mutex> lock(mutex_);
  if (running_) return stopping_ ? ESP_ERR_INVALID_STATE : ESP_OK;
  if (!profile_.id || !elegoo_sdcp_valid_mainboard_id(profile_.serial)) return ESP_ERR_INVALID_ARG;
  if (!controls_) controls_ = xQueueCreate(1, sizeof(QueuedControl));
  if (!controls_) return ESP_ERR_NO_MEM;
  xQueueReset(controls_);
  network_ = &network; stopping_ = false; running_ = true;
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "elegoo_sdcp", 49152, this, 4, &task_, kServiceCore,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    running_ = false; task_ = nullptr; return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
void ElegooSdcpAdapter::stop() {
  const std::lock_guard<std::mutex> lock(mutex_);
  controls_live_ = false;
  stopping_ = true; if (task_) xTaskNotifyGive(task_);
}
bool ElegooSdcpAdapter::request_control(const core::ResinControlRequest& request) {
  if (!running_ || stopping_ || !controls_live_ || !controls_) return false;
  const QueuedControl queued{request, now_ms(), generation_.load()};
  return xQueueSend(controls_, &queued, 0) == pdTRUE;
}
void ElegooSdcpAdapter::task_entry(void* context) {
  static_cast<ElegooSdcpAdapter*>(context)->run(); vTaskDeleteWithCaps(nullptr);
}
void ElegooSdcpAdapter::run() {
  while (!stopping_) {
    core::PrinterProfile profile; std::uint32_t generation = 0;
    { const std::lock_guard<std::mutex> lock(mutex_); profile = profile_; generation = generation_; }
    if (!profile.id) break;
    const auto cancelled = [&] { return stopping_ || generation != generation_ || !network_->status().station_connected; };
    auto session = std::make_unique<Session>();
    auto state = std::make_unique<core::PrinterSnapshot>();
    std::string preview_task;
    std::shared_ptr<std::vector<std::uint8_t>> preview;
    std::uint64_t next_preview = 0;
    unsigned preview_attempts = 0;
    if (!cancelled() && session->open(profile, false, now_ms() + 7000, cancelled)) {
      if (controls_) xQueueReset(controls_);
      controls_live_ = profile.protocol == core::PrinterProtocol::uniformation_sdcp;
      std::uint64_t published_at = 0;
      while (!cancelled() && session->step(cancelled)) {
        const auto now = now_ms();
        if (!session->ready(now) || now - published_at < 100) continue;
        session->snapshot_into(*state, now);
        QueuedControl control;
        if (controls_ && xQueueReceive(controls_, &control, 0) == pdTRUE) {
          // One-shot commands belong to this connection and this exact job.
          // Never replay a queued action after reconnection or a slow download.
          if (profile.protocol == core::PrinterProtocol::uniformation_sdcp &&
              control.generation == generation && now >= control.queued_at_ms &&
              now - control.queued_at_ms <= 2000 &&
              core::resin_control_matches(control.request, *state, now) && !cancelled()) {
            if (!session->control(control.request.action, cancelled)) break;
          }
        }
        if (profile.protocol == core::PrinterProtocol::uniformation_sdcp) {
          const auto& task = state->job.preview_hint;
          if (task != preview_task) {
            preview_task = task; preview.reset(); next_preview = 0; preview_attempts = 0;
          }
          // A changed job is published without the previous image before a
          // bounded download. Failed/missing previews never take status offline.
          if (!preview_requested_.load()) { preview.reset(); preview_attempts = 0; next_preview = 0; }
          state->job.preview = preview;
          if (preview_requested_.load() && !preview_task.empty() && !preview &&
              preview_attempts < 3 && now >= next_preview) {
            { const std::lock_guard<std::mutex> lock(mutex_);
              if (generation == generation_ && !stopping_) snapshots_.replace(*state); }
            ++preview_attempts; next_preview = now + 30000;
            preview = session->preview(preview_task, [&] {
              return cancelled() || !preview_requested_.load();
            });
            // Read the queued status before publishing an image: the print
            // may have changed while HTTP was in flight.
            continue;
          }
        }
        { const std::lock_guard<std::mutex> lock(mutex_);
          if (generation == generation_ && !stopping_) snapshots_.replace(std::move(*state)); }
        published_at = now;
      }
    }
    controls_live_ = false;
    if (controls_) xQueueReset(controls_);
    session.reset();  // release the socket before any backoff/reconfiguration
    { const std::lock_guard<std::mutex> lock(mutex_);
      if (generation == generation_ && !stopping_) {
        const auto link = network_->status().station_connected ? core::LinkState::failed : core::LinkState::waiting_for_network;
        snapshots_.invalidate(profile.id, link, now_ms());
      } }
    if (!stopping_ && generation == generation_) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000 + esp_random() % 1000));
  }
  ESP_LOGI("elegoo_sdcp", "Stopped worker stack high-water=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  const std::lock_guard<std::mutex> lock(mutex_); task_ = nullptr; running_ = false;
}

}  // namespace printdeck::platform
