#include "printdeck/core/audio_pack.hpp"

#include <algorithm>
#include <cstring>

namespace printdeck::core {
namespace {
std::uint32_t little32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
      static_cast<std::uint32_t>(data[1]) << 8 |
      static_cast<std::uint32_t>(data[2]) << 16 |
      static_cast<std::uint32_t>(data[3]) << 24;
}
}

const AudioPackEntry* AudioPackIndex::find(std::string_view key) const {
  for (std::size_t i = 0; i < count; ++i) {
    if (key == entries[i].key.data()) return &entries[i];
  }
  return nullptr;
}

namespace {
template <typename Read>
bool parse_index(std::size_t bytes, AudioPackIndex& index, Read read) {
  index.count = 0;
  if (bytes < kAudioPackHeaderBytes || bytes > kAudioPackMaximumBytes) return false;
  std::array<std::uint8_t, kAudioPackEntryBytes> buffer{};
  if (!read(buffer.data(), kAudioPackHeaderBytes) ||
      std::memcmp(buffer.data(), "PDAUDIO2", 8) != 0) return false;
  const std::size_t count = little32(buffer.data() + 8);
  if (count == 0 || count > kAudioPackMaximumEntries) return false;
  std::size_t next = kAudioPackHeaderBytes + count * kAudioPackEntryBytes;
  if (next > bytes) return false;
  for (std::size_t i = 0; i < count; ++i) {
    if (!read(buffer.data(), buffer.size())) return false;
    const auto terminator = std::find(buffer.begin(), buffer.begin() + 64, 0);
    if (terminator == buffer.begin() || terminator == buffer.begin() + 64) return false;
    for (auto p = buffer.begin(); p != terminator; ++p) {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    if (!std::all_of(terminator, buffer.begin() + 64, [](auto c) { return c == 0; })) return false;
    auto& entry = index.entries[i];
    std::memcpy(entry.key.data(), buffer.data(), entry.key.size());
    for (std::size_t j = 0; j < i; ++j) {
      if (entry.key == index.entries[j].key) return false;
    }
    entry.offset = little32(buffer.data() + 64);
    entry.compressed_bytes = little32(buffer.data() + 68);
    entry.decoded_bytes = little32(buffer.data() + 72);
    if (entry.offset != next || entry.compressed_bytes < 18 ||
        entry.compressed_bytes > 65536 + 1024 || entry.decoded_bytes < 12 ||
        entry.decoded_bytes > 65536 || entry.compressed_bytes > bytes - next) return false;
    next += entry.compressed_bytes;
  }
  if (next != bytes) return false;
  index.count = count;
  return true;
}

}  // namespace

bool read_audio_pack_index(std::FILE* file, std::size_t bytes, AudioPackIndex& index) {
  index.count = 0;
  if (!file || std::fseek(file, 0, SEEK_SET) != 0) return false;
  return parse_index(bytes, index, [file](std::uint8_t* output, std::size_t size) {
    return std::fread(output, 1, size, file) == size;
  });
}

bool read_audio_pack_index(std::span<const std::uint8_t> bytes, AudioPackIndex& index) {
  std::size_t offset = 0;
  return parse_index(bytes.size(), index, [&](std::uint8_t* output, std::size_t size) {
    if (size > bytes.size() - offset) return false;
    std::memcpy(output, bytes.data() + offset, size);
    offset += size;
    return true;
  });
}

}  // namespace printdeck::core
