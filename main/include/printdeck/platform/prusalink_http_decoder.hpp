#pragma once

#include "http_parser.h"
#include "printdeck/platform/prusalink_client.hpp"

namespace printdeck::platform {

// Streaming HTTP framing shared by ESP transport and host boundary tests.
class PrusaLinkHttpDecoder {
 public:
  explicit PrusaLinkHttpDecoder(std::size_t maximum_body);
  bool feed(std::string_view bytes);
  bool finish();
  bool complete() const { return complete_; }
  PrusaLinkHttpResponse take_response() { return std::move(response_); }

 private:
  bool commit_header();
  static int header_field(http_parser*, const char*, std::size_t);
  static int header_value(http_parser*, const char*, std::size_t);
  static int headers_complete(http_parser*);
  static int body(http_parser*, const char*, std::size_t);
  static int message_complete(http_parser*);
  static int chunk_header(http_parser*);
  http_parser parser_{};
  http_parser_settings callbacks_{};
  PrusaLinkHttpResponse response_;
  std::size_t maximum_body_;
  std::size_t header_bytes_ = 0;
  std::size_t challenge_bytes_ = 0;
  std::size_t chunks_ = 0;
  std::string field_;
  std::string value_;
  bool value_started_ = false;
  bool headers_done_ = false;
  bool complete_ = false;
};

}  // namespace printdeck::platform
