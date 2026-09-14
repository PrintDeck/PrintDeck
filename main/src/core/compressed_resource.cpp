#include "printdeck/core/compressed_resource.hpp"

#include <cstring>
#include <algorithm>
#include <array>
#include <limits>
#include "zlib.h"

namespace printdeck::core {
namespace {
struct Allocator {
  void* (*allocate)(std::size_t);
  void (*release)(void*);
};

voidpf allocate_inflate(voidpf opaque, uInt count, uInt size) {
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return nullptr;
  const auto bytes = static_cast<std::size_t>(count) * size;
  auto* result = static_cast<Allocator*>(opaque)->allocate(bytes);
  if (result != nullptr) std::memset(result, 0, bytes);
  return result;
}

void release_inflate(voidpf opaque, voidpf address) {
  static_cast<Allocator*>(opaque)->release(address);
}
}  // namespace

bool decompress_gzip_exact(const std::uint8_t* input, std::size_t input_size,
                           std::uint8_t* output, std::size_t output_size,
                           void* (*allocate)(std::size_t),
                           void (*release)(void*)) noexcept {
  if (input == nullptr || output == nullptr || allocate == nullptr || release == nullptr ||
      input_size < 18 || output_size == 0 ||
      input_size > std::numeric_limits<uInt>::max() ||
      output_size > std::numeric_limits<uInt>::max()) return false;
  Allocator allocator{allocate, release};
  z_stream stream{};
  stream.zalloc = allocate_inflate;
  stream.zfree = release_inflate;
  stream.opaque = &allocator;
  stream.next_in = const_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_size);
  stream.next_out = output;
  stream.avail_out = static_cast<uInt>(output_size);
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return false;
  const int status = inflate(&stream, Z_FINISH);
  const bool valid = status == Z_STREAM_END && stream.total_in == input_size &&
                     stream.total_out == output_size;
  inflateEnd(&stream);
  return valid;
}

bool decompress_gzip_slice(const std::uint8_t* input, std::size_t input_size,
                          std::size_t total_output_size, std::size_t offset,
                          std::uint8_t* output, std::size_t output_size,
                          void* (*allocate)(std::size_t),
                          void (*release)(void*)) noexcept {
  if (!input || !output || !allocate || !release || input_size < 18 ||
      output_size == 0 || offset > total_output_size ||
      output_size > total_output_size - offset ||
      input_size > std::numeric_limits<uInt>::max() ||
      total_output_size > std::numeric_limits<uInt>::max()) return false;
  Allocator allocator{allocate, release};
  z_stream stream{};
  stream.zalloc = allocate_inflate;
  stream.zfree = release_inflate;
  stream.opaque = &allocator;
  stream.next_in = const_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_size);
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return false;
  std::array<std::uint8_t, 1024> chunk{};
  std::size_t position = 0;
  int status = Z_OK;
  while (status == Z_OK) {
    stream.next_out = chunk.data();
    stream.avail_out = chunk.size();
    const auto previous_input = stream.total_in;
    status = inflate(&stream, Z_NO_FLUSH);
    const std::size_t produced = chunk.size() - stream.avail_out;
    if (produced > total_output_size - position) { status = Z_DATA_ERROR; break; }
    const auto begin = std::max(position, offset);
    const auto end = std::min(position + produced, offset + output_size);
    if (end > begin) std::memcpy(output + begin - offset, chunk.data() + begin - position,
                                 end - begin);
    position += produced;
    if (status == Z_OK && produced == 0 && stream.total_in == previous_input) {
      status = Z_DATA_ERROR;
    }
  }
  const bool valid = status == Z_STREAM_END && stream.total_in == input_size &&
                     position == total_output_size;
  inflateEnd(&stream);
  return valid;
}

}  // namespace printdeck::core
