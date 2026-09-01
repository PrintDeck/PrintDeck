#include "printdeck/platform/moonraker_camera_client.hpp"
#include "printdeck/platform/task_affinity.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "codec_api.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_dec_sw.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "esp_peer_default.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "mbedtls/base64.h"

namespace printdeck::platform {
namespace {

constexpr char kTag[] = "printdeck.mrcam";
constexpr std::size_t kMaximumJpegBytes = 1024U * 1024U;
constexpr std::uint16_t kOutputWidth = 400;
constexpr std::uint16_t kOutputHeight = 224;
constexpr std::int64_t kRefreshIntervalUs = 2000000;
constexpr std::int64_t kLivePublishIntervalUs = 125000;
// K2 keyframes take about two seconds of software H264 work on the S3.  Keep
// enough quiet time between accepted IDRs for Wi-Fi, Moonraker and IDLE0 even
// when the printer sends a burst of keyframes after the decoder catches up.
constexpr std::int64_t kCrealityMinimumDecodeIntervalUs = 4000000;
constexpr EventBits_t kWebsocketConnected = BIT0;
constexpr EventBits_t kWebsocketFailed = BIT1;

struct ResponseBuffer {
  std::vector<std::uint8_t> bytes;
  bool overflow = false;
  std::size_t maximum = kMaximumJpegBytes;
};

struct WebsocketWaiter {
  EventGroupHandle_t events = nullptr;
};

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool supports_snapmaker_u1(const core::PrinterProfile& profile) {
  if (profile.protocol != core::PrinterProtocol::moonraker || profile.id == 0) return false;
  const std::string identity = lower(profile.manufacturer + " " + profile.brand + " " +
                                     profile.model + " " + profile.display_name);
  return identity.find("snapmaker") != std::string::npos ||
         identity.find("snap maker") != std::string::npos ||
         identity.find(" u1") != std::string::npos;
}

bool supports_creality_k2(const core::PrinterProfile& profile) {
  if (profile.protocol != core::PrinterProtocol::moonraker || profile.id == 0) return false;
  const std::string identity = lower(profile.manufacturer + " " + profile.brand + " " +
                                     profile.model + " " + profile.display_name);
  const bool creality = identity.find("creality") != std::string::npos;
  const bool k2 = identity.find("k2") != std::string::npos;
  return creality && k2;
}

std::string base_url(std::string endpoint) {
  while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
  if (endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) {
    endpoint.insert(0, "http://");
  }
  return endpoint;
}

std::string websocket_url(const core::PrinterProfile& profile) {
  std::string url = base_url(profile.endpoint);
  url.replace(0, url.rfind("https://", 0) == 0 ? 5 : 4,
              url.rfind("https://", 0) == 0 ? "wss" : "ws");
  url += "/websocket";
  if (!profile.api_key.empty()) url += "?token=" + profile.api_key;
  return url;
}

esp_err_t response_event(esp_http_client_event_t* event) {
  if (event == nullptr || event->user_data == nullptr || event->event_id != HTTP_EVENT_ON_DATA ||
      event->data == nullptr || event->data_len <= 0) return ESP_OK;
  auto* response = static_cast<ResponseBuffer*>(event->user_data);
  const auto size = static_cast<std::size_t>(event->data_len);
  if (response->bytes.size() + size > response->maximum) {
    response->overflow = true;
    return ESP_FAIL;
  }
  const auto* begin = static_cast<const std::uint8_t*>(event->data);
  response->bytes.insert(response->bytes.end(), begin, begin + size);
  return ESP_OK;
}

bool fetch_bytes(const core::PrinterProfile& profile, const std::string& resource,
                 std::size_t maximum, std::vector<std::uint8_t>* bytes,
                 std::string* content_type = nullptr) {
  if (bytes == nullptr) return false;
  ResponseBuffer response;
  response.maximum = maximum;
  response.bytes.reserve(std::min<std::size_t>(maximum, 32U * 1024U));
  const std::string url = resource.rfind("http://", 0) == 0 ||
                                  resource.rfind("https://", 0) == 0
                              ? resource : base_url(profile.endpoint) + resource;
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 5000;
  config.event_handler = response_event;
  config.user_data = &response;
  config.buffer_size = 4096;
  if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return false;
  if (!profile.api_key.empty()) {
    esp_http_client_set_header(client, "X-Api-Key", profile.api_key.c_str());
  }
  const esp_err_t result = esp_http_client_perform(client);
  const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
  if (content_type != nullptr) {
    char* value = nullptr;
    if (esp_http_client_get_header(client, "Content-Type", &value) == ESP_OK && value != nullptr) {
      *content_type = value;
    } else {
      content_type->clear();
    }
  }
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300 || response.overflow) return false;
  *bytes = std::move(response.bytes);
  return true;
}

bool complete_jpeg(const std::vector<std::uint8_t>& bytes) {
  return bytes.size() >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8 &&
         bytes[bytes.size() - 2] == 0xff && bytes.back() == 0xd9;
}

bool decode_rgb565(const std::vector<std::uint8_t>& jpeg,
                   std::shared_ptr<std::vector<std::uint8_t>>* frame,
                   std::uint16_t* width, std::uint16_t* height) {
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  config.scale.width = kOutputWidth;
  config.scale.height = kOutputHeight;
  jpeg_dec_handle_t decoder = nullptr;
  if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == nullptr) return false;
  jpeg_dec_io_t io{};
  jpeg_dec_header_info_t header{};
  io.inbuf = const_cast<std::uint8_t*>(jpeg.data());
  io.inbuf_len = static_cast<int>(jpeg.size());
  void* decoded = nullptr;
  bool success = false;
  do {
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK) break;
    int decoded_bytes = 0;
    if (jpeg_dec_get_outbuf_len(decoder, &decoded_bytes) != JPEG_ERR_OK || decoded_bytes <= 0) break;
    decoded = jpeg_calloc_align(static_cast<std::size_t>(decoded_bytes), 16);
    if (decoded == nullptr) break;
    io.outbuf = static_cast<std::uint8_t*>(decoded);
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) break;
    auto output = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<std::size_t>(decoded_bytes));
    std::memcpy(output->data(), decoded, output->size());
    *frame = std::move(output);
    *width = kOutputWidth;
    *height = kOutputHeight;
    success = true;
  } while (false);
  if (decoded != nullptr) jpeg_free_align(decoded);
  jpeg_dec_close(decoder);
  return success;
}

std::string camera_host(std::string endpoint) {
  if (endpoint.rfind("http://", 0) == 0) endpoint.erase(0, 7);
  else if (endpoint.rfind("https://", 0) == 0) endpoint.erase(0, 8);
  const std::size_t slash = endpoint.find('/');
  if (slash != std::string::npos) endpoint.resize(slash);
  const std::size_t colon = endpoint.rfind(':');
  if (colon != std::string::npos && endpoint.find(':') == colon) endpoint.resize(colon);
  return endpoint;
}

std::vector<std::string> camera_resource_candidates(
    const core::PrinterProfile& profile, const std::string& resource) {
  if (resource.rfind("http://", 0) == 0 || resource.rfind("https://", 0) == 0) {
    return {resource};
  }
  const std::string path = resource.empty() || resource.front() == '/'
                               ? resource : "/" + resource;
  const std::string host = camera_host(profile.endpoint);
  std::vector<std::string> candidates{resource};
  auto append_unique = [&](std::string candidate) {
    if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
      candidates.push_back(std::move(candidate));
    }
  };
  // Moonraker webcam URLs are often relative to the web frontend, not to the
  // Moonraker API port which returned /server/webcams/list.  Try the ordinary
  // web origin and Creality's current Fluidd port without making either one a
  // mandatory brand-specific route.
  append_unique("http://" + host + path);
  append_unique("http://" + host + ":4408" + path);
  append_unique("http://" + host + ":4409" + path);
  return candidates;
}

bool starts_with_payload(const std::string& line, const char* attribute,
                         const std::string& payload) {
  const std::string prefix = std::string("a=") + attribute + ":" + payload;
  return line.rfind(prefix, 0) == 0 &&
         (line.size() == prefix.size() || line[prefix.size()] == ' ');
}

std::string sanitize_creality_answer(const std::string& answer,
                                      std::size_t attempt,
                                      std::size_t* candidate_count) {
  std::vector<std::string> lines;
  for (std::size_t cursor = 0; cursor < answer.size();) {
    const std::size_t end = answer.find('\n', cursor);
    std::string line = answer.substr(cursor, end == std::string::npos ? std::string::npos
                                                                      : end - cursor);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::string::npos) break;
    cursor = end + 1;
  }
  std::size_t media_line = lines.size();
  std::size_t media_end = lines.size();
  std::vector<std::string> formats;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (lines[index].rfind("m=video ", 0) == 0) {
      media_line = index;
      std::size_t cursor = 0;
      std::vector<std::string> fields;
      while (cursor < lines[index].size()) {
        const std::size_t space = lines[index].find(' ', cursor);
        fields.push_back(lines[index].substr(cursor, space == std::string::npos
                                                        ? std::string::npos : space - cursor));
        if (space == std::string::npos) break;
        cursor = space + 1;
      }
      if (fields.size() > 3) formats.assign(fields.begin() + 3, fields.end());
      for (std::size_t next = index + 1; next < lines.size(); ++next) {
        if (lines[next].rfind("m=", 0) == 0) {
          media_end = next;
          break;
        }
      }
      break;
    }
  }
  if (media_line == lines.size()) {
    if (candidate_count != nullptr) *candidate_count = 0;
    return answer;
  }
  std::vector<std::string> candidates;
  for (const std::string& format : formats) {
    bool h264 = false;
    bool decoder_profile = true;
    for (std::size_t index = media_line + 1; index < media_end; ++index) {
      if (starts_with_payload(lines[index], "rtpmap", format)) {
        std::string codec = lower(lines[index]);
        h264 = codec.find(" h264/") != std::string::npos;
      }
      if (starts_with_payload(lines[index], "fmtp", format)) {
        const std::string params = lower(lines[index]);
        const std::size_t profile = params.find("profile-level-id=");
        if (profile != std::string::npos && profile + 19 <= params.size()) {
          decoder_profile = params.compare(profile + 17, 2, "42") == 0;
        }
      }
    }
    if (h264 && decoder_profile) candidates.push_back(format);
  }
  if (candidate_count != nullptr) *candidate_count = candidates.size();
  if (candidates.empty()) return answer;
  // Current Creality answers advertise a nominal first H264 format that their
  // own sender does not actually use.  Prefer the second format (also used by
  // their browser client), while retaining every other format as a retry.
  const std::size_t selected_index =
      (attempt + (candidates.size() > 1 ? 1U : 0U)) % candidates.size();
  const std::string& selected = candidates[selected_index];
  std::string mline = lines[media_line];
  std::size_t third_space = 0;
  for (int count = 0; count < 3; ++count) {
    third_space = mline.find(' ', third_space);
    if (third_space == std::string::npos) return answer;
    ++third_space;
  }
  mline.resize(third_space);
  mline += selected;
  lines[media_line] = std::move(mline);

  std::string output;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    bool keep = true;
    if (index > media_line && index < media_end) {
      for (const std::string& format : formats) {
        if (format == selected) continue;
        if (starts_with_payload(lines[index], "rtpmap", format) ||
            starts_with_payload(lines[index], "fmtp", format) ||
            starts_with_payload(lines[index], "rtcp-fb", format)) {
          keep = false;
          break;
        }
      }
      if (keep && starts_with_payload(lines[index], "fmtp", selected) &&
          lower(lines[index]).find("x-google") != std::string::npos) keep = false;
    }
    if (keep) {
      output += lines[index];
      output += "\r\n";
    }
  }
  return output;
}

std::shared_ptr<std::vector<std::uint8_t>> i420_to_rgb565(
    const std::uint8_t* y_plane, const std::uint8_t* u_plane,
    const std::uint8_t* v_plane, std::uint16_t source_width,
    std::uint16_t source_height, std::uint16_t y_stride,
    std::uint16_t chroma_stride, std::uint16_t* output_width,
    std::uint16_t* output_height) {
  if (y_plane == nullptr || u_plane == nullptr || v_plane == nullptr ||
      source_width == 0 || source_height == 0 || y_stride < source_width ||
      chroma_stride < source_width / 2U || source_width > 4096 ||
      source_height > 2160) return {};
  constexpr std::uint16_t maximum_width = 400;
  constexpr std::uint16_t maximum_height = 224;
  std::uint16_t width = maximum_width;
  std::uint16_t height = static_cast<std::uint16_t>(
      static_cast<std::uint32_t>(maximum_width) * source_height / source_width);
  if (height > maximum_height) {
    height = maximum_height;
    width = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(maximum_height) * source_width / source_height);
  }
  width = std::max<std::uint16_t>(2, width & ~1U);
  height = std::max<std::uint16_t>(2, height & ~1U);
  // Calculate the nearest-neighbour sampling map once.  The old inner-loop
  // division performed roughly 90,000 integer divides per snapshot even
  // though every output column always selects the same source column.
  std::array<std::uint16_t, maximum_width> source_x{};
  std::array<std::uint16_t, maximum_height> source_y{};
  for (std::uint16_t x = 0; x < width; ++x) {
    source_x[x] = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(x) * source_width / width);
  }
  for (std::uint16_t y = 0; y < height; ++y) {
    source_y[y] = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(y) * source_height / height);
  }
  auto pixels = std::make_shared<std::vector<std::uint8_t>>(
      static_cast<std::size_t>(width) * height * 2U);
  for (std::uint16_t y = 0; y < height; ++y) {
    const std::uint16_t sy = source_y[y];
    for (std::uint16_t x = 0; x < width; ++x) {
      const std::uint16_t sx = source_x[x];
      const int yy = static_cast<int>(y_plane[static_cast<std::size_t>(sy) * y_stride + sx]);
      const int uu = static_cast<int>(u_plane[static_cast<std::size_t>(sy / 2U) *
                                             chroma_stride + sx / 2U]) - 128;
      const int vv = static_cast<int>(v_plane[static_cast<std::size_t>(sy / 2U) *
                                             chroma_stride + sx / 2U]) - 128;
      const int c = std::max(0, yy - 16);
      const int r = std::clamp((298 * c + 409 * vv + 128) >> 8, 0, 255);
      const int g = std::clamp((298 * c - 100 * uu - 208 * vv + 128) >> 8, 0, 255);
      const int b = std::clamp((298 * c + 516 * uu + 128) >> 8, 0, 255);
      const std::uint16_t rgb = static_cast<std::uint16_t>(
          ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
      const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 2U;
      (*pixels)[offset] = static_cast<std::uint8_t>(rgb & 0xffU);
      (*pixels)[offset + 1] = static_cast<std::uint8_t>(rgb >> 8U);
    }
  }
  *output_width = width;
  *output_height = height;
  return pixels;
}

std::shared_ptr<std::vector<std::uint8_t>> contiguous_i420_to_rgb565(
    const std::uint8_t* source, std::uint16_t source_width,
    std::uint16_t source_height, std::uint16_t* output_width,
    std::uint16_t* output_height) {
  if (source == nullptr || source_width == 0 || source_height == 0) return {};
  const std::uint8_t* y = source;
  const std::uint8_t* u = y + static_cast<std::size_t>(source_width) * source_height;
  const std::uint8_t* v = u + static_cast<std::size_t>(source_width / 2U) *
                                   (source_height / 2U);
  return i420_to_rgb565(y, u, v, source_width, source_height, source_width,
                        source_width / 2U, output_width, output_height);
}

bool contains_h264_nal(const std::uint8_t* data, std::size_t size,
                       std::uint8_t wanted_type) {
  if (data == nullptr || size == 0) return false;
  bool found_start_code = false;
  for (std::size_t index = 0; index + 3 < size; ++index) {
    std::size_t header = 0;
    if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
      header = index + 3;
    } else if (index + 4 < size && data[index] == 0 && data[index + 1] == 0 &&
               data[index + 2] == 0 && data[index + 3] == 1) {
      header = index + 4;
    }
    if (header == 0 || header >= size) continue;
    found_start_code = true;
    if ((data[header] & 0x1fU) == wanted_type) return true;
    index = header;
  }
  if (found_start_code) return false;
  if ((data[0] & 0x1fU) == wanted_type) return true;
  if (size > 4) {
    const std::size_t nal_size = (static_cast<std::size_t>(data[0]) << 24U) |
                                 (static_cast<std::size_t>(data[1]) << 16U) |
                                 (static_cast<std::size_t>(data[2]) << 8U) |
                                 data[3];
    return nal_size > 0 && nal_size <= size - 4U &&
           (data[4] & 0x1fU) == wanted_type;
  }
  return false;
}

void append_annex_b(std::vector<std::uint8_t>* output,
                    const std::uint8_t* data, std::size_t size) {
  if (output == nullptr || data == nullptr || size == 0) return;
  if ((size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
      (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)) {
    output->insert(output->end(), data, data + size);
    return;
  }
  std::size_t cursor = 0;
  bool avcc = false;
  while (cursor + 4U < size) {
    const std::size_t nal_size = (static_cast<std::size_t>(data[cursor]) << 24U) |
                                 (static_cast<std::size_t>(data[cursor + 1]) << 16U) |
                                 (static_cast<std::size_t>(data[cursor + 2]) << 8U) |
                                 data[cursor + 3];
    if (nal_size == 0 || cursor + 4U + nal_size > size) {
      avcc = false;
      break;
    }
    avcc = true;
    static constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};
    output->insert(output->end(), std::begin(kStartCode), std::end(kStartCode));
    output->insert(output->end(), data + cursor + 4U,
                   data + cursor + 4U + nal_size);
    cursor += 4U + nal_size;
  }
  if (avcc && cursor == size) return;
  static constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};
  output->insert(output->end(), std::begin(kStartCode), std::end(kStartCode));
  output->insert(output->end(), data, data + size);
}

std::shared_ptr<std::vector<std::uint8_t>> decode_idr_snapshot(
    const std::uint8_t* data, std::size_t size, std::uint16_t* output_width,
    std::uint16_t* output_height) {
  if (!contains_h264_nal(data, size, 5)) return {};
  // K2's SDP avcC parameters describe a dummy 128x96 stream.  Its in-band
  // keyframes use these real 1920x1080 Main/CABAC parameters.
  static constexpr std::uint8_t kK2ParameterSets[] = {
      0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x00, 0x29, 0x8d, 0x8d, 0x40,
      0x3c, 0x01, 0x13, 0xf2, 0xcd, 0xc0, 0x40, 0x40, 0x50, 0x00, 0x00,
      0x5d, 0xc0, 0x00, 0x15, 0xf9, 0x00, 0x40,
      0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x38, 0x80,
  };
  std::vector<std::uint8_t> access_unit;
  access_unit.reserve(sizeof(kK2ParameterSets) + size + 4U);
  access_unit.insert(access_unit.end(), std::begin(kK2ParameterSets),
                     std::end(kK2ParameterSets));
  append_annex_b(&access_unit, data, size);

  ISVCDecoder* decoder = nullptr;
  if (WelsCreateDecoder(&decoder) != 0 || decoder == nullptr) return {};
  std::shared_ptr<std::vector<std::uint8_t>> pixels;
  SDecodingParam parameters{};
  parameters.uiTargetDqLayer = 0xff;
  parameters.eEcActiveIdc = ERROR_CON_DISABLE;
  parameters.sVideoProperty.size = sizeof(parameters.sVideoProperty);
  parameters.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
  int threads = 0;
  decoder->SetOption(DECODER_OPTION_NUM_OF_THREADS, &threads);
  if (decoder->Initialize(&parameters) == 0) {
    std::uint8_t* planes[3] = {nullptr, nullptr, nullptr};
    SBufferInfo info{};
    DECODING_STATE state = decoder->DecodeFrame2(
        access_unit.data(), static_cast<int>(access_unit.size()), planes, &info);
    // DecodeFrameNoDelay() unconditionally performs a second empty DecodeFrame2
    // call and can overwrite an already-ready IDR's iBufferStatus with zero.
    // K2 gives us a complete RTP access unit, so retain the first result and
    // flush only when that call genuinely has no picture yet.
    if (info.iBufferStatus == 0) {
      state = static_cast<DECODING_STATE>(
          static_cast<int>(state) |
          static_cast<int>(decoder->DecodeFrame2(nullptr, 0, planes, &info)));
    }
    if (info.iBufferStatus == 0) {
      int buffered_frames = 0;
      if (decoder->GetOption(DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER,
                             &buffered_frames) == 0 &&
          buffered_frames > 0) {
        state = static_cast<DECODING_STATE>(
            static_cast<int>(state) |
            static_cast<int>(decoder->FlushFrame(planes, &info)));
      }
    }
    ESP_LOGI(kTag,
             "Creality OpenH264 state=0x%x buffer=%d planes=%p/%p/%p size=%dx%d "
             "PSRAM free=%u largest=%u",
             static_cast<unsigned>(state), info.iBufferStatus, planes[0], planes[1],
             planes[2], info.UsrData.sSystemBuffer.iWidth,
             info.UsrData.sSystemBuffer.iHeight,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    // OpenH264 can flag recoverable bitstream damage when a receiver joins an
    // already-running WebRTC stream.  A complete output buffer is nevertheless
    // a valid snapshot; do not discard it solely because state is non-zero.
    if (info.iBufferStatus == 1 && planes[0] != nullptr &&
        planes[1] != nullptr && planes[2] != nullptr &&
        info.UsrData.sSystemBuffer.iWidth > 0 &&
        info.UsrData.sSystemBuffer.iHeight > 0) {
      const auto width = static_cast<std::uint16_t>(info.UsrData.sSystemBuffer.iWidth);
      const auto height = static_cast<std::uint16_t>(info.UsrData.sSystemBuffer.iHeight);
      const auto y_stride = static_cast<std::uint16_t>(info.UsrData.sSystemBuffer.iStride[0]);
      const auto chroma_stride = static_cast<std::uint16_t>(info.UsrData.sSystemBuffer.iStride[1]);
      pixels = i420_to_rgb565(planes[0], planes[1], planes[2], width, height,
                              y_stride, chroma_stride, output_width, output_height);
    }
    decoder->Uninitialize();
  }
  WelsDestroyDecoder(decoder);
  return pixels;
}

void websocket_event(void* argument, esp_event_base_t, std::int32_t event_id, void*) {
  auto* waiter = static_cast<WebsocketWaiter*>(argument);
  if (waiter == nullptr || waiter->events == nullptr) return;
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    xEventGroupSetBits(waiter->events, kWebsocketConnected);
  } else if (event_id == WEBSOCKET_EVENT_ERROR || event_id == WEBSOCKET_EVENT_DISCONNECTED) {
    xEventGroupSetBits(waiter->events, kWebsocketFailed);
  }
}

}  // namespace

void MoonrakerCameraClient::configure(const core::PrinterProfile* profile) {
  const core::PrinterProfile next = profile != nullptr ? *profile : core::PrinterProfile{};
  {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    profile_ = next;
  }
  backend_.store(Backend::unknown);
  reconfigure_requested_.store(true);
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_ = {};
  // The camera page is available for every Moonraker profile.  Support is
  // authoritative only after lazy backend discovery receives a usable source.
  snapshot_.supported = false;
  snapshot_.live_supported = false;
  snapshot_.detail = "Camera off";
}

void MoonrakerCameraClient::set_network_ready(bool ready) { network_ready_.store(ready); }

void MoonrakerCameraClient::set_enabled(bool enabled) {
  if (enabled_.exchange(enabled, std::memory_order_acq_rel) == enabled) return;
  if (!enabled) {
    // Any frame completing after the page was left belongs to the previous
    // visible session and must not be published when the page is reopened.
    creality_session_generation_.fetch_add(1, std::memory_order_acq_rel);
  }
  TaskHandle_t task = nullptr;
  TaskHandle_t decoder = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_;
    decoder = decoder_task_;
  }
  if (task != nullptr) xTaskNotifyGive(task);
  if (decoder != nullptr) xTaskNotifyGive(decoder);
}

void MoonrakerCameraClient::set_mode(bool live, int snapshot_fps) {
  const bool changed = live_mode_.exchange(live) != live;
  snapshot_fps_.store(snapshot_fps == 5 ? 5 : snapshot_fps == 2 ? 2 : 1);
  if (changed) reconfigure_requested_.store(true);
}

esp_err_t MoonrakerCameraClient::start() {
  const std::lock_guard<std::mutex> lock(task_mutex_);
  if (running_.load(std::memory_order_acquire)) {
    return stop_requested_.load(std::memory_order_acquire) ? ESP_ERR_INVALID_STATE
                                                           : ESP_OK;
  }
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  active_tasks_.store(2, std::memory_order_release);
  const BaseType_t decoder_created = xTaskCreatePinnedToCoreWithCaps(
      decoder_task_entry, "camera_decoder", 12288, this, 3, &decoder_task_, kServiceCore,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (decoder_created != pdPASS) {
    active_tasks_.store(0, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    return ESP_ERR_NO_MEM;
  }
  const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      task_entry, "moonraker_camera", 14336, this, 4, &task_, kServiceCore,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (created != pdPASS) {
    stop_requested_.store(true, std::memory_order_release);
    active_tasks_.store(1, std::memory_order_release);
    xTaskNotifyGive(decoder_task_);
  }
  return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void MoonrakerCameraClient::stop() {
  enabled_.store(false, std::memory_order_release);
  stop_requested_.store(true, std::memory_order_release);
  creality_session_generation_.fetch_add(1, std::memory_order_acq_rel);
  TaskHandle_t task = nullptr;
  TaskHandle_t decoder = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_;
    decoder = decoder_task_;
  }
  if (task != nullptr) xTaskNotifyGive(task);
  if (decoder != nullptr) xTaskNotifyGive(decoder);
  publish_status(false, "Camera off", true);
}

MoonrakerCameraSnapshot MoonrakerCameraClient::snapshot() const {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_;
}

core::PrinterProfile MoonrakerCameraClient::profile() const {
  std::lock_guard<std::mutex> lock(profile_mutex_);
  return profile_;
}

void MoonrakerCameraClient::publish_status(bool connected, const char* detail,
                                            bool clear_frame) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_.connected = connected;
  snapshot_.detail = detail == nullptr ? "" : detail;
  if (clear_frame) {
    snapshot_.refreshing = false;
    snapshot_.frame.reset();
    snapshot_.width = 0;
    snapshot_.height = 0;
  }
}

void MoonrakerCameraClient::set_refreshing(bool refreshing) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_.refreshing = refreshing;
}

void MoonrakerCameraClient::publish_frame(std::shared_ptr<std::vector<std::uint8_t>> frame,
                                          std::uint16_t width, std::uint16_t height) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_.connected = true;
  snapshot_.refreshing = false;
  snapshot_.detail = "Camera image updated";
  snapshot_.frame = std::move(frame);
  snapshot_.width = width;
  snapshot_.height = height;
}

bool MoonrakerCameraClient::fetch_frame(const core::PrinterProfile& profile,
                                        const char* path) {
  ResponseBuffer response;
  response.bytes.reserve(256U * 1024U);
  const std::string resource = path == nullptr ? "" : path;
  const std::string url = resource.rfind("http://", 0) == 0 ||
                                  resource.rfind("https://", 0) == 0
                              ? resource : base_url(profile.endpoint) + resource;
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 7000;
  config.event_handler = response_event;
  config.user_data = &response;
  config.buffer_size = 4096;
  if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return false;
  if (!profile.api_key.empty()) esp_http_client_set_header(client, "X-Api-Key", profile.api_key.c_str());
  esp_http_client_set_header(client, "Accept", "image/jpeg");
  const esp_err_t result = esp_http_client_perform(client);
  const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300 || response.overflow ||
      !complete_jpeg(response.bytes)) return false;
  if (!enabled_.load()) return false;
  std::shared_ptr<std::vector<std::uint8_t>> frame;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  if (!decode_rgb565(response.bytes, &frame, &width, &height)) return false;
  if (!enabled_.load()) return false;
  publish_frame(std::move(frame), width, height);
  return true;
}

bool MoonrakerCameraClient::detect_backend(const core::PrinterProfile& profile) {
  std::vector<std::uint8_t> metadata;
  std::vector<std::string> snapshot_urls;
  std::vector<std::string> stream_urls;
  if (fetch_bytes(profile, "/server/webcams/list", 64U * 1024U, &metadata) &&
      !metadata.empty()) {
    metadata.push_back('\0');
    cJSON* root = cJSON_Parse(reinterpret_cast<const char*>(metadata.data()));
    cJSON* result = root == nullptr ? nullptr :
        cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON* webcams = result == nullptr ? nullptr :
        cJSON_GetObjectItemCaseSensitive(result, "webcams");
    if (cJSON_IsArray(webcams)) {
      cJSON* webcam = nullptr;
      cJSON_ArrayForEach(webcam, webcams) {
        const cJSON* snapshot = cJSON_GetObjectItemCaseSensitive(webcam, "snapshot_url");
        const cJSON* stream = cJSON_GetObjectItemCaseSensitive(webcam, "stream_url");
        if (cJSON_IsString(snapshot) && snapshot->valuestring != nullptr &&
            snapshot->valuestring[0] != '\0') snapshot_urls.emplace_back(snapshot->valuestring);
        if (cJSON_IsString(stream) && stream->valuestring != nullptr &&
            stream->valuestring[0] != '\0') stream_urls.emplace_back(stream->valuestring);
      }
    }
    cJSON_Delete(root);
  }

  auto inspect_webrtc_page = [&](const std::string& resource) {
    for (const std::string& candidate : camera_resource_candidates(profile, resource)) {
      std::vector<std::uint8_t> page;
      if (!fetch_bytes(profile, candidate, 96U * 1024U, &page) || page.empty()) continue;
      const std::string html(reinterpret_cast<const char*>(page.data()), page.size());
      const std::size_t call = html.find("/call/webrtc_local");
      if (call == std::string::npos) continue;
      std::string suffix = ":8000/call/webrtc_local";
      const std::size_t colon = html.rfind(':', call);
      if (colon != std::string::npos && call > colon + 1 && call - colon <= 7) {
        bool numeric = true;
        for (std::size_t index = colon + 1; index < call; ++index) {
          numeric = numeric && std::isdigit(static_cast<unsigned char>(html[index]));
        }
        if (numeric) suffix = html.substr(
            colon, call + std::strlen("/call/webrtc_local") - colon);
      }
      creality_signal_url_ = "http://" + camera_host(profile.endpoint) + suffix;
      backend_.store(Backend::creality_webrtc);
      {
        const std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.supported = true;
        snapshot_.live_supported = !supports_creality_k2(profile);
        snapshot_.detail = "WebRTC camera detected";
      }
      ESP_LOGI(kTag, "Detected WebRTC page %s; signaling at %s", candidate.c_str(),
               creality_signal_url_.c_str());
      return true;
    }
    return false;
  };

  for (const std::string& resource : stream_urls) {
    if (inspect_webrtc_page(resource)) return true;
  }
  for (const std::string& resource : snapshot_urls) {
    for (const std::string& candidate : camera_resource_candidates(profile, resource)) {
      std::vector<std::uint8_t> sample;
      if (!fetch_bytes(profile, candidate, kMaximumJpegBytes, &sample)) continue;
      if (complete_jpeg(sample)) {
        snapshot_path_ = candidate;
        backend_.store(candidate.find("/webcam/") != std::string::npos
                           ? Backend::paxx_snapshot : Backend::generic_snapshot);
        const std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.supported = true;
        snapshot_.live_supported = false;
        snapshot_.detail = "Snapshot camera detected";
        return true;
      }
      const std::string page(reinterpret_cast<const char*>(sample.data()), sample.size());
      if (page.find("/call/webrtc_local") != std::string::npos &&
          inspect_webrtc_page(candidate)) return true;
    }
  }
  if (supports_creality_k2(profile) && inspect_webrtc_page("/camera.html")) return true;
  if (supports_snapmaker_u1(profile)) {
    backend_.store(Backend::snapmaker_stock);
    const std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_.supported = true;
    snapshot_.live_supported = false;
    snapshot_.detail = "Snapmaker camera ready";
    return true;
  }
  return false;
}

bool MoonrakerCameraClient::send_stock_command(const core::PrinterProfile& profile,
                                                bool start) {
  EventGroupHandle_t events = xEventGroupCreate();
  if (events == nullptr) return false;
  WebsocketWaiter waiter{events};
  const std::string uri = websocket_url(profile);
  esp_websocket_client_config_t config{};
  config.uri = uri.c_str();
  config.network_timeout_ms = 5000;
  if (uri.rfind("wss://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
  if (client == nullptr) {
    vEventGroupDelete(events);
    return false;
  }
  esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event, &waiter);
  bool success = esp_websocket_client_start(client) == ESP_OK;
  if (success) {
    const EventBits_t result = xEventGroupWaitBits(
        events, kWebsocketConnected | kWebsocketFailed, pdTRUE, pdFALSE, pdMS_TO_TICKS(6000));
    success = (result & kWebsocketConnected) != 0;
  }
  if (success) {
    const char* message = start
        ? "{\"jsonrpc\":\"2.0\",\"method\":\"camera.start_monitor\",\"params\":{\"domain\":\"lan\",\"interval\":2},\"id\":1}"
        : "{\"jsonrpc\":\"2.0\",\"method\":\"camera.stop_monitor\",\"params\":{\"domain\":\"lan\"},\"id\":2}";
    success = esp_websocket_client_send_text(client, message, std::strlen(message),
                                              pdMS_TO_TICKS(3000)) >= 0;
    vTaskDelay(pdMS_TO_TICKS(start ? 500 : 100));
  }
  esp_websocket_client_stop(client);
  esp_websocket_client_destroy(client);
  vEventGroupDelete(events);
  return success;
}

int MoonrakerCameraClient::peer_state_callback(esp_peer_state_t state, void* context) {
  auto* camera = static_cast<MoonrakerCameraClient*>(context);
  if (camera == nullptr) return 0;
  ESP_LOGI(kTag, "Creality WebRTC state=%d", static_cast<int>(state));
  if (state == ESP_PEER_STATE_PAIRED && camera->peer_ != nullptr) {
    esp_peer_addr_t address{};
    if (esp_peer_get_paired_addr(static_cast<esp_peer_handle_t>(camera->peer_), &address) ==
        ESP_PEER_ERR_NONE) {
      ESP_LOGI(kTag, "Creality ICE pair=%u.%u.%u.%u:%u",
               static_cast<unsigned>(address.ipv4[0]),
               static_cast<unsigned>(address.ipv4[1]),
               static_cast<unsigned>(address.ipv4[2]),
               static_cast<unsigned>(address.ipv4[3]),
               static_cast<unsigned>(address.port));
    }
  }
  if (state == ESP_PEER_STATE_CONNECTED ||
      state == ESP_PEER_STATE_REMOTE_VIDEO_TRACK_ADDED) {
    camera->peer_connected_.store(true);
    camera->publish_status(true, "Live camera connected");
  } else if (state == ESP_PEER_STATE_CONNECT_FAILED ||
             state == ESP_PEER_STATE_DISCONNECTED) {
    camera->peer_connected_.store(false);
  }
  return 0;
}

int MoonrakerCameraClient::peer_message_callback(esp_peer_msg_t* message, void* context) {
  auto* camera = static_cast<MoonrakerCameraClient*>(context);
  if (camera == nullptr || message == nullptr || message->type != ESP_PEER_MSG_TYPE_SDP ||
      message->data == nullptr || message->size <= 0) return 0;
  {
    const std::lock_guard<std::mutex> lock(camera->peer_mutex_);
    camera->pending_offer_.assign(reinterpret_cast<const char*>(message->data),
                                  static_cast<std::size_t>(message->size));
    while (!camera->pending_offer_.empty() && camera->pending_offer_.back() == '\0') {
      camera->pending_offer_.pop_back();
    }
  }
  camera->offer_ready_.store(true);
  return 0;
}

int MoonrakerCameraClient::peer_video_info_callback(esp_peer_video_stream_info_t* info,
                                                     void* context) {
  auto* camera = static_cast<MoonrakerCameraClient*>(context);
  if (camera == nullptr || info == nullptr) return 0;
  ESP_LOGI(kTag, "Creality WebRTC video codec=%d size=%dx%d fps=%d",
           static_cast<int>(info->codec), info->width, info->height, info->fps);
  return info->codec == ESP_PEER_VIDEO_CODEC_H264 ? 0 : -1;
}

int MoonrakerCameraClient::peer_video_callback(esp_peer_video_frame_t* frame,
                                                void* context) {
  auto* camera = static_cast<MoonrakerCameraClient*>(context);
  if (camera == nullptr || frame == nullptr || frame->data == nullptr || frame->size <= 0) {
    return 0;
  }
  if (!camera->enabled_.load()) return 0;
  camera->last_creality_video_us_.store(
      static_cast<std::uint64_t>(esp_timer_get_time()));
  const std::uint32_t callback = camera->video_callback_count_.fetch_add(1) + 1U;
  if (callback <= 8U) {
    const std::uint8_t* data = frame->data;
    ESP_LOGI(kTag,
             "Creality video callback=%u size=%d pts=%u head=%02x%02x%02x%02x idr=%d",
             static_cast<unsigned>(callback), frame->size,
             static_cast<unsigned>(frame->pts), data[0],
             frame->size > 1 ? data[1] : 0, frame->size > 2 ? data[2] : 0,
             frame->size > 3 ? data[3] : 0,
             contains_h264_nal(data, static_cast<std::size_t>(frame->size), 5));
  }
  // SPS/PPS and partial access units are valid WebRTC deliveries even when they do
  // not immediately produce a picture.  Returning an error here would make the
  // peer discard a healthy stream before the first complete frame arrives.
  camera->decode_creality_frame(frame->data, static_cast<std::size_t>(frame->size),
                                frame->pts);
  return 0;
}

bool MoonrakerCameraClient::start_creality_peer(const core::PrinterProfile& profile) {
  stop_creality_peer();
  // A 1080p keyframe arrives as a short RTP burst. Avoid modem sleep and
  // synchronous library diagnostics while that burst is being received.
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_log_level_set("BUF_MNGR", ESP_LOG_WARN);
  esp_log_level_set("DTLS", ESP_LOG_WARN);
  esp_log_level_set("AGENT", ESP_LOG_WARN);
  esp_log_level_set("PEER_DEF", ESP_LOG_WARN);
  // Several Creality WebRTC builds only parse the first DTLS cipher in the
  // ClientHello.  Try their known-good AES-GCM first, then fall back to the
  // library order and ChaCha after cycling through both advertised H264 formats.
  const std::size_t cipher_attempt = (creality_codec_attempt_ / 2U) % 3U;
  const esp_peer_dtls_cipher_pref_t cipher =
      cipher_attempt == 0 ? ESP_PEER_DTLS_CIPHER_AES_GCM
                          : cipher_attempt == 1 ? ESP_PEER_DTLS_CIPHER_AUTO
                                                : ESP_PEER_DTLS_CIPHER_CHACHA;
  esp_peer_set_dtls_cipher_pref(cipher);
  // Keep the lightweight Baseline decoder for normal WebRTC cameras.  The K2
  // is the exception: despite its SDP it sends 1080p Main/CABAC, decoded only
  // for a single keyframe by the bounded snapshot decoder below.
  idr_snapshot_decoder_.store(supports_creality_k2(profile));
  creality_idr_count_.store(0);
  last_creality_idr_queued_us_.store(0);
  last_creality_video_us_.store(0);
  if (!idr_snapshot_decoder_.load()) {
    esp_h264_dec_cfg_sw_t decoder_config{};
    decoder_config.pic_type = ESP_H264_RAW_FMT_I420;
    esp_h264_dec_handle_t decoder = nullptr;
    if (esp_h264_dec_sw_new(&decoder_config, &decoder) != ESP_H264_ERR_OK ||
        decoder == nullptr || esp_h264_dec_open(decoder) != ESP_H264_ERR_OK) {
      if (decoder != nullptr) esp_h264_dec_del(decoder);
      ESP_LOGE(kTag, "H264 decoder could not be started");
      return false;
    }
    h264_decoder_ = decoder;
  }

  esp_peer_default_cfg_t peer_defaults{};
  peer_defaults.agent_recv_timeout = 10;
  peer_defaults.max_candidates = 8;
  peer_defaults.alive_binding_retries = 3;
  peer_defaults.rtp_cfg.video_recv_jitter.cache_timeout = 500;
  peer_defaults.rtp_cfg.video_recv_jitter.resend_delay = 10;
  peer_defaults.rtp_cfg.video_recv_jitter.pli_send_interval = 500;
  peer_defaults.rtp_cfg.video_recv_jitter.cache_size = 400U * 1024U;
  esp_peer_cfg_t config{};
  config.role = ESP_PEER_ROLE_CONTROLLING;
  config.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
  config.video_info.codec = ESP_PEER_VIDEO_CODEC_H264;
  config.video_info.width = 400;
  config.video_info.height = 224;
  config.video_info.fps = 15;
  config.audio_dir = ESP_PEER_MEDIA_DIR_NONE;
  config.video_dir = ESP_PEER_MEDIA_DIR_RECV_ONLY;
  config.no_auto_reconnect = true;
  config.extra_cfg = &peer_defaults;
  config.extra_size = sizeof(peer_defaults);
  config.ctx = this;
  config.on_state = peer_state_callback;
  config.on_msg = peer_message_callback;
  config.on_video_info = peer_video_info_callback;
  config.on_video_data = peer_video_callback;
  esp_peer_handle_t peer = nullptr;
  if (esp_peer_open(&config, esp_peer_get_default_impl(), &peer) != ESP_PEER_ERR_NONE ||
      peer == nullptr) {
    stop_creality_peer();
    ESP_LOGE(kTag, "WebRTC peer could not be opened");
    return false;
  }
  peer_ = peer;
  offer_ready_.store(false);
  peer_connected_.store(false);
  frame_received_.store(false);
  h264_parameter_sets_sent_.store(false);
  video_callback_count_.store(0);
  last_published_frame_us_.store(0);
  if (esp_peer_new_connection(peer) != ESP_PEER_ERR_NONE) {
    stop_creality_peer();
    return false;
  }
  publish_status(false, "Negotiating local WebRTC camera");
  return true;
}

void MoonrakerCameraClient::stop_creality_peer() {
  const bool peer_was_active = peer_ != nullptr || h264_decoder_ != nullptr;
  creality_session_generation_.fetch_add(1);
  {
    const std::lock_guard<std::mutex> lock(pending_idr_mutex_);
    pending_idr_.reset();
  }
  set_refreshing(false);
  offer_ready_.store(false);
  peer_connected_.store(false);
  frame_received_.store(false);
  {
    const std::lock_guard<std::mutex> lock(peer_mutex_);
    pending_offer_.clear();
  }
  if (peer_ != nullptr) {
    esp_peer_close(static_cast<esp_peer_handle_t>(peer_));
    peer_ = nullptr;
  }
  if (h264_decoder_ != nullptr) {
    auto decoder = static_cast<esp_h264_dec_handle_t>(h264_decoder_);
    esp_h264_dec_close(decoder);
    esp_h264_dec_del(decoder);
    h264_decoder_ = nullptr;
  }
  if (peer_was_active) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

bool MoonrakerCameraClient::exchange_creality_offer(const core::PrinterProfile& profile) {
  std::string offer;
  {
    const std::lock_guard<std::mutex> lock(peer_mutex_);
    offer.swap(pending_offer_);
  }
  offer_ready_.store(false);
  if (offer.empty() || peer_ == nullptr) return false;
  // K2's Pion bridge only starts its real Main/CABAC sender for the dynamic
  // payload IDs used by Chromium/go2rtc (96 and 98).  It accepts esp_peer's
  // native 102/106 offer and completes ICE/DTLS, but then sends no RTP at all.
  const auto replace_all = [&](const std::string& from, const std::string& to) {
    std::size_t position = 0;
    while ((position = offer.find(from, position)) != std::string::npos) {
      offer.replace(position, from.size(), to);
      position += to.size();
    }
  };
  replace_all("m=video 9 UDP/TLS/RTP/SAVPF 102 106",
              "m=video 9 UDP/TLS/RTP/SAVPF 96 98");
  for (const char* attribute : {"rtpmap", "rtcp-fb", "fmtp"}) {
    replace_all(std::string("a=") + attribute + ":102 ",
                std::string("a=") + attribute + ":96 ");
    replace_all(std::string("a=") + attribute + ":106 ",
                std::string("a=") + attribute + ":98 ");
  }
  // esp_peer currently emits Main profile for both H264 payloads. Creality's
  // browser/Pion endpoint expects Baseline/Constrained Baseline in the offer,
  // even though its selected 98 stream is actually Main/CABAC.
  const auto set_offer_profile = [&](const char* payload, const char* profile_id) {
    const std::string prefix = std::string("a=fmtp:") + payload + " ";
    const std::size_t line = offer.find(prefix);
    if (line == std::string::npos) return;
    const std::size_t end = offer.find('\n', line);
    const std::size_t profile = offer.find("profile-level-id=", line);
    if (profile == std::string::npos || (end != std::string::npos && profile >= end)) return;
    offer.replace(profile + std::strlen("profile-level-id="), 6, profile_id);
  };
  set_offer_profile("96", "42001f");
  set_offer_profile("98", "42e01f");
  // Creality's local ICE implementation does not start its connectivity checks
  // unless the candidate repeats the offer ufrag and the candidate list is
  // explicitly complete. Browsers/Pion emit both, while esp_peer normally omits
  // them because the session-level credentials are sufficient for standard ICE.
  const std::string ufrag_prefix = "a=ice-ufrag:";
  const std::size_t ufrag_line = offer.find(ufrag_prefix);
  if (ufrag_line != std::string::npos) {
    const std::size_t value = ufrag_line + ufrag_prefix.size();
    const std::size_t end = offer.find_first_of("\r\n", value);
    const std::string ufrag = offer.substr(value, end - value);
    std::size_t candidate = 0;
    while (!ufrag.empty() && (candidate = offer.find("a=candidate:", candidate)) !=
                                 std::string::npos) {
      const std::size_t line_end = offer.find_first_of("\r\n", candidate);
      const std::size_t insert_at = line_end == std::string::npos ? offer.size() : line_end;
      if (offer.find(" ufrag ", candidate) >= insert_at) {
        offer.insert(insert_at, " ufrag " + ufrag);
        candidate = insert_at + 7 + ufrag.size();
      } else {
        candidate = insert_at;
      }
    }
  }
  if (offer.find("a=end-of-candidates") == std::string::npos) {
    while (!offer.empty() && (offer.back() == '\r' || offer.back() == '\n')) offer.pop_back();
    offer += "\r\na=end-of-candidates\r\n";
  }
  if (creality_codec_attempt_ == 0) {
    ESP_LOGI(kTag, "Creality local offer SDP:\n%s", offer.c_str());
  }

  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) return false;
  cJSON_AddStringToObject(root, "type", "offer");
  cJSON_AddStringToObject(root, "sdp", offer.c_str());
  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json == nullptr) return false;
  const std::size_t json_size = std::strlen(json);
  std::vector<std::uint8_t> encoded(4U * ((json_size + 2U) / 3U) + 1U);
  std::size_t encoded_size = 0;
  const int encode_result = mbedtls_base64_encode(encoded.data(), encoded.size(),
                                                   &encoded_size,
                                                   reinterpret_cast<const std::uint8_t*>(json),
                                                   json_size);
  cJSON_free(json);
  if (encode_result != 0) return false;
  encoded.resize(encoded_size);

  ResponseBuffer response;
  response.maximum = 128U * 1024U;
  response.bytes.reserve(16U * 1024U);
  const std::string url = creality_signal_url_.empty()
                              ? "http://" + camera_host(profile.endpoint) +
                                    ":8000/call/webrtc_local"
                              : creality_signal_url_;
  esp_http_client_config_t http_config{};
  http_config.url = url.c_str();
  http_config.method = HTTP_METHOD_POST;
  http_config.timeout_ms = 10000;
  http_config.event_handler = response_event;
  http_config.user_data = &response;
  http_config.buffer_size = 4096;
  esp_http_client_handle_t client = esp_http_client_init(&http_config);
  if (client == nullptr) return false;
  esp_http_client_set_header(client, "Content-Type", "plain/text");
  esp_http_client_set_post_field(client, reinterpret_cast<const char*>(encoded.data()),
                                 static_cast<int>(encoded.size()));
  const esp_err_t request_result = esp_http_client_perform(client);
  const int status = request_result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
  esp_http_client_cleanup(client);
  if (request_result != ESP_OK || status < 200 || status >= 300 || response.overflow ||
      response.bytes.empty()) return false;

  response.bytes.erase(std::remove_if(response.bytes.begin(), response.bytes.end(),
                                      [](std::uint8_t value) { return std::isspace(value); }),
                       response.bytes.end());
  std::vector<std::uint8_t> decoded(response.bytes.size());
  std::size_t decoded_size = 0;
  if (mbedtls_base64_decode(decoded.data(), decoded.size(), &decoded_size,
                            response.bytes.data(), response.bytes.size()) != 0) return false;
  decoded.resize(decoded_size);
  decoded.push_back('\0');
  cJSON* answer_json = cJSON_Parse(reinterpret_cast<const char*>(decoded.data()));
  if (answer_json == nullptr) return false;
  const cJSON* type = cJSON_GetObjectItemCaseSensitive(answer_json, "type");
  const cJSON* sdp = cJSON_GetObjectItemCaseSensitive(answer_json, "sdp");
  if (!cJSON_IsString(type) || !cJSON_IsString(sdp) || type->valuestring == nullptr ||
      sdp->valuestring == nullptr || std::strcmp(type->valuestring, "answer") != 0) {
    cJSON_Delete(answer_json);
    return false;
  }
  std::size_t candidate_count = 0;
  std::string answer = sanitize_creality_answer(sdp->valuestring,
                                                 creality_codec_attempt_,
                                                 &candidate_count);
  if (creality_codec_attempt_ == 0) {
    ESP_LOGI(kTag, "Creality sanitized answer SDP:\n%s", answer.c_str());
  }
  cJSON_Delete(answer_json);
  if (candidate_count > 0) {
    const std::size_t selected_index =
        (creality_codec_attempt_ + (candidate_count > 1 ? 1U : 0U)) % candidate_count;
    ESP_LOGI(kTag, "Creality SDP H264 candidates=%u selected=%u retry=%u",
             static_cast<unsigned>(candidate_count),
             static_cast<unsigned>(selected_index),
             static_cast<unsigned>(creality_codec_attempt_));
  }
  esp_peer_msg_t message{};
  message.type = ESP_PEER_MSG_TYPE_SDP;
  message.data = reinterpret_cast<std::uint8_t*>(answer.data());
  message.size = static_cast<int>(answer.size());
  return esp_peer_send_msg(static_cast<esp_peer_handle_t>(peer_), &message) ==
         ESP_PEER_ERR_NONE;
}

bool MoonrakerCameraClient::decode_creality_frame(const std::uint8_t* data,
                                                   std::size_t size,
                                                   std::uint32_t pts) {
  if (data == nullptr || size == 0 ||
      stop_requested_.load(std::memory_order_acquire)) return false;
  if (idr_snapshot_decoder_.load()) {
    if (!contains_h264_nal(data, size, 5)) return false;
    // A 1080p Main/CABAC decode takes most of one core for roughly two
    // seconds. Keep the WebRTC session alive, but decode every other keyframe
    // so Moonraker, networking and the idle watchdog retain CPU time.
    if ((creality_idr_count_.fetch_add(1) & 1U) != 0U ||
        creality_decoder_busy_.load()) return false;
    const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time());
    const std::uint64_t last_queued = last_creality_idr_queued_us_.load();
    if (last_queued != 0 && now - last_queued < kCrealityMinimumDecodeIntervalUs) {
      return false;
    }
    {
      const std::lock_guard<std::mutex> lock(pending_idr_mutex_);
      if (pending_idr_ || creality_decoder_busy_.load()) return false;
      pending_idr_ = std::make_shared<std::vector<std::uint8_t>>(data, data + size);
      pending_idr_generation_ = creality_session_generation_.load();
      last_creality_idr_queued_us_.store(now);
    }
    set_refreshing(true);
    TaskHandle_t decoder = nullptr;
    {
      const std::lock_guard<std::mutex> lock(task_mutex_);
      decoder = decoder_task_;
    }
    if (decoder != nullptr && !stop_requested_.load(std::memory_order_acquire)) {
      xTaskNotifyGive(decoder);
    }
    return true;
  }
  if (h264_decoder_ == nullptr) return false;
  auto decoder = static_cast<esp_h264_dec_handle_t>(h264_decoder_);
  // K2 starts a new receiver in the middle of its GOP and does not repeat the
  // short SPS/PPS carried by its browser stream. Seed the decoder once with the
  // same Baseline parameter sets. If firmware later sends its own SPS/PPS they
  // remain authoritative and simply replace these decoder parameters.
  if (!h264_parameter_sets_sent_.exchange(true)) {
    // The avcC header exposed by K2's WebRTC bridge contains a 128x96
    // placeholder.  The first in-band IDR carries these real 1920x1080 Main
    // profile parameter sets; use those while waiting for the next IDR.
    static std::uint8_t kCrealityK2ParameterSets[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x00, 0x29, 0x8d, 0x8d, 0x40,
        0x3c, 0x01, 0x13, 0xf2, 0xcd, 0xc0, 0x40, 0x40, 0x50, 0x00, 0x00,
        0x5d, 0xc0, 0x00, 0x15, 0xf9, 0x00, 0x40,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x38, 0x80,
    };
    esp_h264_dec_in_frame_t parameters{};
    parameters.raw_data.buffer = kCrealityK2ParameterSets;
    parameters.raw_data.len = sizeof(kCrealityK2ParameterSets);
    while (parameters.raw_data.len > 0) {
      esp_h264_dec_out_frame_t ignored{};
      const esp_h264_err_t result = esp_h264_dec_process(decoder, &parameters, &ignored);
      if (parameters.consume == 0 || result != ESP_H264_ERR_OK) break;
      parameters.raw_data.buffer += parameters.consume;
      parameters.raw_data.len -= std::min(parameters.raw_data.len, parameters.consume);
    }
    ESP_LOGI(kTag, "Seeded Creality H264 decoder parameters");
  }
  esp_h264_dec_in_frame_t input{};
  input.raw_data.buffer = const_cast<std::uint8_t*>(data);
  input.raw_data.len = static_cast<std::uint32_t>(size);
  input.pts = pts;
  bool decoded_image = false;
  while (input.raw_data.len > 0) {
    esp_h264_dec_out_frame_t output{};
    const esp_h264_err_t result = esp_h264_dec_process(decoder, &input, &output);
    if (input.consume == 0) break;
    input.raw_data.buffer += input.consume;
    input.raw_data.len -= std::min(input.raw_data.len, input.consume);
    if (result != ESP_H264_ERR_OK) {
      ESP_LOGW(kTag, "H264 frame decode failed: %d", static_cast<int>(result));
      return false;
    }
    if (output.outbuf == nullptr || output.out_size == 0) continue;
    esp_h264_dec_param_sw_handle_t parameters = nullptr;
    esp_h264_resolution_t resolution{};
    if (esp_h264_dec_sw_get_param_hd(decoder, &parameters) != ESP_H264_ERR_OK ||
        parameters == nullptr ||
        esp_h264_dec_get_resolution(parameters, &resolution) != ESP_H264_ERR_OK) continue;
    const std::size_t expected = static_cast<std::size_t>(resolution.width) *
                                 resolution.height * 3U / 2U;
    if (output.out_size < expected) continue;
    decoded_image = true;
    const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time());
    const std::uint64_t last = last_published_frame_us_.load();
    if (live_mode_.load() && last != 0 && now - last < kLivePublishIntervalUs) continue;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    auto pixels = contiguous_i420_to_rgb565(output.outbuf, resolution.width,
                                            resolution.height, &width, &height);
    if (pixels) {
      frame_received_.store(true);
      last_published_frame_us_.store(now);
      publish_frame(std::move(pixels), width, height);
    }
  }
  return decoded_image;
}

void MoonrakerCameraClient::task_entry(void* context) {
  auto* camera = static_cast<MoonrakerCameraClient*>(context);
  camera->task_loop();
  camera->finish_task(false);
  vTaskDeleteWithCaps(nullptr);
}

void MoonrakerCameraClient::decoder_task_entry(void* context) {
  auto* camera = static_cast<MoonrakerCameraClient*>(context);
  camera->decoder_loop();
  camera->finish_task(true);
  vTaskDeleteWithCaps(nullptr);
}

void MoonrakerCameraClient::finish_task(bool decoder) {
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    if (decoder) decoder_task_ = nullptr;
    else task_ = nullptr;
  }
  if (active_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    running_.store(false, std::memory_order_release);
  }
}

void MoonrakerCameraClient::decoder_loop() {
  while (!stop_requested_.load(std::memory_order_acquire)) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!stop_requested_.load(std::memory_order_acquire)) {
      std::shared_ptr<std::vector<std::uint8_t>> encoded;
      std::uint32_t generation = 0;
      {
        const std::lock_guard<std::mutex> lock(pending_idr_mutex_);
        if (!pending_idr_) break;
        creality_decoder_busy_.store(true);
        encoded.swap(pending_idr_);
        generation = pending_idr_generation_;
      }
      if (!encoded || encoded->empty()) {
        creality_decoder_busy_.store(false);
        break;
      }
      if (generation != creality_session_generation_.load()) {
        creality_decoder_busy_.store(false);
        continue;
      }
      if (!enabled_.load() || !idr_snapshot_decoder_.load()) {
        creality_decoder_busy_.store(false);
        continue;
      }
      ESP_LOGI(kTag, "Creality IDR queued for decode (%u bytes)",
               static_cast<unsigned>(encoded->size()));
      std::uint16_t width = 0;
      std::uint16_t height = 0;
      auto pixels = decode_idr_snapshot(encoded->data(), encoded->size(), &width, &height);
      if (!pixels) {
        ESP_LOGW(kTag, "Creality Main/CABAC keyframe decode failed");
        if (generation == creality_session_generation_.load()) set_refreshing(false);
        creality_decoder_busy_.store(false);
        continue;
      }
      if (generation != creality_session_generation_.load() || !enabled_.load() ||
          !idr_snapshot_decoder_.load()) {
        if (generation == creality_session_generation_.load()) set_refreshing(false);
        creality_decoder_busy_.store(false);
        continue;
      }
      frame_received_.store(true);
      last_published_frame_us_.store(static_cast<std::uint64_t>(esp_timer_get_time()));
      publish_frame(std::move(pixels), width, height);
      ESP_LOGI(kTag, "Creality keyframe published at %ux%u", width, height);
      creality_decoder_busy_.store(false);
    }
  }
}

void MoonrakerCameraClient::task_loop() {
  bool stock_monitor_started = false;
  core::PrinterProfile last_profile;
  std::int64_t last_capture_us = 0;
  std::int64_t last_detection_us = 0;
  std::int64_t peer_started_us = 0;
  std::int64_t next_peer_start_us = 0;
  std::uint32_t failures = 0;
  while (!stop_requested_.load(std::memory_order_acquire)) {
    if (reconfigure_requested_.exchange(false)) {
      stop_creality_peer();
      stock_monitor_started = false;
      last_capture_us = 0;
      last_detection_us = 0;
      peer_started_us = 0;
      next_peer_start_us = 0;
      failures = 0;
      creality_codec_attempt_ = 0;
    }
    const core::PrinterProfile current = profile();
    last_profile = current;
    if (current.protocol != core::PrinterProtocol::moonraker || current.id == 0) {
      stop_creality_peer();
      publish_status(false, "Camera unsupported", true);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }
    if (!network_ready_.load()) {
      stop_creality_peer();
      publish_status(false, "Camera waiting for Wi-Fi", true);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }
    if (!enabled_.load()) {
      if (stock_monitor_started) {
        send_stock_command(current, false);
        stock_monitor_started = false;
      }
      stop_creality_peer();
      publish_status(false, "Camera off", true);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      continue;
    }
    const std::int64_t now = esp_timer_get_time();
    if (backend_.load() == Backend::unknown &&
        (last_detection_us == 0 || now - last_detection_us >= 5000000)) {
      publish_status(false, "Detecting local camera");
      last_detection_us = now;
      detect_backend(current);
    }
    Backend backend = backend_.load();
    if (backend == Backend::unknown) {
      publish_status(false, "No camera detected", false);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }
    if (backend == Backend::creality_webrtc) {
      if (peer_ == nullptr && now >= next_peer_start_us) {
        if (start_creality_peer(current)) {
          peer_started_us = now;
        } else {
          ++failures;
          next_peer_start_us = now + 3000000;
        }
      }
      if (peer_ != nullptr) {
        esp_peer_main_loop(static_cast<esp_peer_handle_t>(peer_));
        if (offer_ready_.load() && !exchange_creality_offer(current)) {
          ESP_LOGW(kTag, "Creality WebRTC signaling exchange failed");
          stop_creality_peer();
          ++failures;
          next_peer_start_us = esp_timer_get_time() + 2000000;
        } else if (!live_mode_.load() && !idr_snapshot_decoder_.load() &&
                   frame_received_.load()) {
          failures = 0;
          stop_creality_peer();
          const int fps = snapshot_fps_.load();
          next_peer_start_us = esp_timer_get_time() + 1000000 / std::max(1, fps);
        } else if (idr_snapshot_decoder_.load() && frame_received_.load() &&
                   last_creality_video_us_.load() != 0 &&
                   esp_timer_get_time() - last_creality_video_us_.load() > 12000000) {
          ESP_LOGW(kTag, "Creality WebRTC stream stalled; reconnecting");
          stop_creality_peer();
          next_peer_start_us = esp_timer_get_time() + 1000000;
        } else if (!frame_received_.load() && peer_started_us != 0 &&
                   esp_timer_get_time() - peer_started_us > 12000000) {
          ESP_LOGW(kTag, "No frame for SDP codec attempt %u; trying next candidate",
                   static_cast<unsigned>(creality_codec_attempt_));
          stop_creality_peer();
          ++creality_codec_attempt_;
          ++failures;
          next_peer_start_us = esp_timer_get_time() + 1000000;
        }
      }
      // Drain the socket fast enough for a multi-packet 1080p IDR burst.  A
      // 10 ms cadence was sufficient for small P-frames but overflowed the UDP
      // receive mailbox before a complete keyframe could be reassembled.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
      continue;
    }

    const std::int64_t refresh_interval = backend == Backend::snapmaker_stock
        ? kRefreshIntervalUs
        : 1000000 / std::max(1, snapshot_fps_.load());
    if (last_capture_us != 0 && now - last_capture_us < refresh_interval) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
      continue;
    }
    publish_status(false, "Loading camera image");
    set_refreshing(true);
    bool captured = false;
    if (backend == Backend::generic_snapshot || backend == Backend::paxx_snapshot) {
      captured = fetch_frame(current, snapshot_path_.empty()
                                          ? "/webcam/snapshot.jpg"
                                          : snapshot_path_.c_str());
    }
    if (!captured && backend == Backend::snapmaker_stock) {
      captured = fetch_frame(current, "/webcam/snapshot.jpg");
      if (captured) {
        backend_.store(Backend::paxx_snapshot);
        snapshot_path_ = "/webcam/snapshot.jpg";
      }
    }
    if (!captured && backend == Backend::snapmaker_stock) {
      if (!stock_monitor_started) stock_monitor_started = send_stock_command(current, true);
      if (stock_monitor_started) {
        captured = fetch_frame(current, "/server/files/camera/monitor.jpg");
      }
    }
    last_capture_us = esp_timer_get_time();
    if (captured) {
      failures = 0;
      ESP_LOGD(kTag, "Snapmaker camera snapshot updated");
      continue;
    }
    set_refreshing(false);
    ++failures;
    if (failures >= 3) {
      backend_.store(Backend::unknown);
      stock_monitor_started = false;
      snapshot_path_.clear();
    }
    publish_status(false, "Camera unavailable", false);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(failures < 3 ? 2000 : 5000));
  }
  if (stock_monitor_started && last_profile.id != 0 && network_ready_.load()) {
    send_stock_command(last_profile, false);
  }
  stop_creality_peer();
  publish_status(false, "Camera off", true);
}

}  // namespace printdeck::platform
