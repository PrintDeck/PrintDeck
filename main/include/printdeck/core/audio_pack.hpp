#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace printdeck::core {

inline constexpr std::size_t kAudioStorageBudget = 512 * 1024;
inline constexpr std::size_t kAudioPackMaximumBytes = 160 * 1024;
inline constexpr std::size_t kAudioPackMaximumEntries = 24;
inline constexpr std::size_t kAudioPackHeaderBytes = 12;
inline constexpr std::size_t kAudioPackEntryBytes = 76;

struct AudioPackEntry {
  std::array<char, 64> key{};
  std::uint32_t offset = 0;
  std::uint32_t compressed_bytes = 0;
  std::uint32_t decoded_bytes = 0;
};

struct AudioPackIndex {
  std::array<AudioPackEntry, kAudioPackMaximumEntries> entries{};
  std::size_t count = 0;
  const AudioPackEntry* find(std::string_view key) const;
};

// Reads only the bounded index. The installer must also verify the whole-file
// digest and each gzip/PDIA payload before making a package active.
bool read_audio_pack_index(std::FILE* file, std::size_t bytes, AudioPackIndex& index);

// Reaction writes must leave the unused part of the audio budget available.
// Audio files already on disk count toward both filesystem usage and the budget.
constexpr std::size_t audio_storage_reserve(std::size_t audio_bytes) {
  return audio_bytes < kAudioStorageBudget ? kAudioStorageBudget - audio_bytes : 0;
}

}  // namespace printdeck::core
