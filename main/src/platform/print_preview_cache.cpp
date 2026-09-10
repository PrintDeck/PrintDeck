#include "printdeck/platform/print_preview_cache.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_littlefs.h"
#endif

namespace printdeck::platform {
namespace {
constexpr std::uint32_t kMagic = 0x31565050;
struct Header { std::uint32_t magic, size; std::uint64_t identity, checksum; };
std::uint64_t hash(std::span<const std::uint8_t> bytes) {
  std::uint64_t result = 14695981039346656037ULL;
  for (auto byte : bytes) { result ^= byte; result *= 1099511628211ULL; }
  return result;
}
std::uint64_t identity(std::string_view key) {
  return hash({reinterpret_cast<const std::uint8_t*>(key.data()), key.size()});
}
bool valid_image(std::span<const std::uint8_t> bytes) {
  constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
  return (bytes.size() >= 33 && std::memcmp(bytes.data(), signature.data(), 8) == 0 &&
          std::memcmp(bytes.data() + 12, "IHDR", 4) == 0) ||
         (bytes.size() >= 54 && bytes[0] == 'B' && bytes[1] == 'M');
}
bool read_header(FILE* file, std::string_view key, Header& header) {
  if (!file || std::fread(&header, sizeof(header), 1, file) != 1 ||
      header.magic != kMagic || header.identity != identity(key) ||
      header.size < 33 || header.size > PrintPreviewCache::kMaximumBytes) return false;
  struct stat info{};
  return fstat(fileno(file), &info) == 0 &&
         info.st_size == static_cast<off_t>(sizeof(header) + header.size);
}
}

std::string PrintPreviewCache::path(std::string_view key) const {
  char name[32];
  std::snprintf(name, sizeof(name), "/%016llx.bin",
                static_cast<unsigned long long>(identity(key)));
  return directory_ + name;
}

std::size_t PrintPreviewCache::size(std::string_view key) const {
  if (key.empty()) return 0;
  FILE* file = std::fopen(path(key).c_str(), "rb");
  Header header{};
  const bool valid = read_header(file, key, header);
  if (file) std::fclose(file);
  return valid ? header.size : 0;
}

bool PrintPreviewCache::read(std::string_view key, std::span<std::uint8_t> bytes) const {
  if (key.empty()) return false;
  FILE* file = std::fopen(path(key).c_str(), "rb");
  Header header{};
  const bool valid = read_header(file, key, header) && bytes.size() == header.size &&
      std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
      valid_image(bytes) && hash(bytes) == header.checksum;
  if (file) std::fclose(file);
  return valid;
}

bool PrintPreviewCache::write(std::string_view key,
                              std::span<const std::uint8_t> bytes) const {
  if (key.empty() || bytes.size() > kMaximumBytes || !valid_image(bytes)) return false;
  if (mkdir(directory_.c_str(), 0700) != 0 && errno != EEXIST) return false;
  const std::string destination = path(key);
  // Bound the entire disposable cache to 2 MiB, independently of printer count.
  std::size_t used = 0;
  if (DIR* directory = opendir(directory_.c_str())) {
    while (dirent* entry = readdir(directory)) {
      const std::string_view name(entry->d_name);
      if (name.size() == 25 && name.ends_with(".bin.part")) {
        std::remove((directory_ + "/" + entry->d_name).c_str());
        continue;
      }
      if (name.size() != 20 || !name.ends_with(".bin")) continue;
      const std::string candidate = directory_ + "/" + entry->d_name;
      if (candidate == destination) continue;
      struct stat info{};
      if (stat(candidate.c_str(), &info) == 0) used += info.st_size;
    }
    closedir(directory);
  }
  if (used + sizeof(Header) + bytes.size() > 2 * 1024 * 1024) {
    if (DIR* directory = opendir(directory_.c_str())) {
      while (dirent* entry = readdir(directory)) {
        const std::string_view name(entry->d_name);
        if (name.size() == 20 && name.ends_with(".bin"))
          std::remove((directory_ + "/" + entry->d_name).c_str());
      }
      closedir(directory);
    }
  }
#ifdef ESP_PLATFORM
  std::size_t total = 0, occupied = 0;
  if (esp_littlefs_info("assets", &total, &occupied) != ESP_OK || occupied > total ||
      total - occupied < bytes.size() + sizeof(Header) + 128 * 1024) return false;
#endif
  const std::string staging = destination + ".part";
  FILE* file = std::fopen(staging.c_str(), "wb");
  if (!file) return false;
  const Header header{kMagic, static_cast<std::uint32_t>(bytes.size()),
                      identity(key), hash(bytes)};
  const bool written = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
      std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  const bool closed = std::fclose(file) == 0;
  if (written && closed && std::rename(staging.c_str(), destination.c_str()) == 0) return true;
  std::remove(staging.c_str());
  return false;
}

void PrintPreviewCache::remove(std::string_view key) const {
  if (key.empty()) return;
  const std::string filename = path(key);
  std::remove(filename.c_str());
  std::remove((filename + ".part").c_str());
}
}  // namespace printdeck::platform
