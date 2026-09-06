#include "printdeck/core/compressed_resource.hpp"

#include <cstring>
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

}  // namespace printdeck::core
