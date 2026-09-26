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

bool read_audio_pack_index(std::FILE* file, std::size_t bytes, AudioPackIndex& index) {
  index.count = 0;
  if (file == nullptr || bytes < kAudioPackHeaderBytes ||
      bytes > kAudioPackMaximumBytes || std::fseek(file, 0, SEEK_SET) != 0) return false;
  std::array<std::uint8_t, kAudioPackEntryBytes> buffer{};
  if (std::fread(buffer.data(), 1, kAudioPackHeaderBytes, file) != kAudioPackHeaderBytes ||
      std::memcmp(buffer.data(), "PDAUDIO1", 8) != 0) return false;
  const std::size_t count = little32(buffer.data() + 8);
  if (count == 0 || count > kAudioPackMaximumEntries) return false;
  std::size_t next = kAudioPackHeaderBytes + count * kAudioPackEntryBytes;
  if (next > bytes) return false;
  for (std::size_t i = 0; i < count; ++i) {
    if (std::fread(buffer.data(), 1, buffer.size(), file) != buffer.size()) return false;
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

}  // namespace printdeck::core
