#include "printdeck/platform/prusalink_http_decoder.hpp"

#include <limits>

namespace printdeck::platform {

PrusaLinkHttpDecoder::PrusaLinkHttpDecoder(std::size_t maximum_body)
    : maximum_body_(maximum_body) {
  http_parser_init(&parser_, HTTP_RESPONSE);
  parser_.data = this;
  callbacks_.on_header_field = header_field;
  callbacks_.on_header_value = header_value;
  callbacks_.on_headers_complete = headers_complete;
  callbacks_.on_body = body;
  callbacks_.on_message_complete = message_complete;
  callbacks_.on_chunk_header = chunk_header;
}

bool PrusaLinkHttpDecoder::commit_header() {
  if (field_.empty()) return true;
  if (field_ == "www-authenticate") {
    challenge_bytes_ += value_.size();
    if (challenge_bytes_ > 2048 || response_.challenges.size() >= 8) return false;
    response_.challenges.push_back(value_);
  } else if (field_ == "content-encoding") {
    if (!response_.content_encoding.empty()) return false;
    response_.content_encoding = value_;
  } else if (field_ == "content-type") {
    if (!response_.content_type.empty()) return false;
    response_.content_type = value_;
  }
  field_.clear();
  value_.clear();
  value_started_ = false;
  return true;
}

int PrusaLinkHttpDecoder::header_field(http_parser* parser, const char* data, std::size_t size) {
  auto& self = *static_cast<PrusaLinkHttpDecoder*>(parser->data);
  if (self.headers_done_) return 1;  // No credential-changing trailer fields.
  self.header_bytes_ += size;
  if (self.header_bytes_ > 16384 || (self.value_started_ && !self.commit_header()) ||
      self.field_.size() + size > 128) return 1;
  for (std::size_t i = 0; i < size; ++i) {
    char ch = data[i];
    self.field_ += ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 'a' - 'A') : ch;
  }
  return 0;
}

int PrusaLinkHttpDecoder::header_value(http_parser* parser, const char* data, std::size_t size) {
  auto& self = *static_cast<PrusaLinkHttpDecoder*>(parser->data);
  self.header_bytes_ += size;
  if (self.header_bytes_ > 16384 || self.value_.size() + size > 2048) return 1;
  self.value_started_ = true;
  self.value_.append(data, size);
  return 0;
}

int PrusaLinkHttpDecoder::headers_complete(http_parser* parser) {
  auto& self = *static_cast<PrusaLinkHttpDecoder*>(parser->data);
  if (!self.commit_header() || parser->status_code < 200 || parser->upgrade ||
      (parser->content_length != std::numeric_limits<std::uint64_t>::max() &&
       parser->content_length > self.maximum_body_)) return -1;
  self.headers_done_ = true;
  self.response_.status = parser->status_code;
  return 0;
}

int PrusaLinkHttpDecoder::body(http_parser* parser, const char* data, std::size_t size) {
  auto& self = *static_cast<PrusaLinkHttpDecoder*>(parser->data);
  if (size > self.maximum_body_ - self.response_.body.size()) return 1;
  self.response_.body.append(data, size);
  return 0;
}

int PrusaLinkHttpDecoder::message_complete(http_parser* parser) {
  auto& self = *static_cast<PrusaLinkHttpDecoder*>(parser->data);
  if (self.complete_) return 1;
  self.complete_ = true;
  return 0;
}

int PrusaLinkHttpDecoder::chunk_header(http_parser* parser) {
  auto& self = *static_cast<PrusaLinkHttpDecoder*>(parser->data);
  // Bound chunk framing work even when each chunk contributes one byte.
  return ++self.chunks_ > self.maximum_body_ + 1 ? 1 : 0;
}

bool PrusaLinkHttpDecoder::feed(std::string_view bytes) {
  if (response_.error != PrusaLinkError::none) return false;
  if (complete_ && !bytes.empty()) {
    response_.error = PrusaLinkError::unsupported_response;
    response_.body.clear();
    return false;
  }
  const auto parsed = http_parser_execute(&parser_, &callbacks_, bytes.data(), bytes.size());
  if (parsed != bytes.size() || HTTP_PARSER_ERRNO(&parser_) != HPE_OK) {
    response_.error = PrusaLinkError::unsupported_response;
    response_.body.clear();
    return false;
  }
  return true;
}

bool PrusaLinkHttpDecoder::finish() {
  if (!complete_ && !feed({})) return false;
  if (!complete_) response_.error = PrusaLinkError::unsupported_response;
  return complete_ && response_.error == PrusaLinkError::none;
}

}  // namespace printdeck::platform
