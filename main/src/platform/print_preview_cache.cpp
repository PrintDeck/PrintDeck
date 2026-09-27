#include "printdeck/platform/print_preview_cache.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_littlefs.h"
#endif

namespace printdeck::platform {
namespace {
std::mutex cache_mutex;
unsigned asset_writers = 0;
PrintPreviewCache::Transfer* pending_transfers = nullptr;
constexpr std::uint32_t kMagic = 0x31565050;
bool owned_cache_file(std::string_view name) {
  if (!((name.size() == 20 && name.ends_with(".bin")) ||
        (name.size() == 25 && name.ends_with(".bin.part")))) return false;
  return std::all_of(name.begin(), name.begin() + 16, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
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

PrintPreviewCache::AssetWriteGuard::AssetWriteGuard() {
  const std::lock_guard lock(cache_mutex);
  ++asset_writers;
  // Close partial writers before measuring or reclaiming flash. The service
  // still owns its image and publishes it from RAM when the transfer fails.
  for (auto* transfer = pending_transfers; transfer;) {
    auto* next = transfer->next_transfer_;
    if (!transfer->input_.empty()) transfer->cancel_locked();
    transfer = next;
  }
}

PrintPreviewCache::AssetWriteGuard::~AssetWriteGuard() {
  const std::lock_guard lock(cache_mutex);
  --asset_writers;
}

PrintPreviewCache::Transfer::~Transfer() { cancel(); }

void PrintPreviewCache::Transfer::cancel() {
  const std::lock_guard lock(cache_mutex);
  cancel_locked();
}

void PrintPreviewCache::Transfer::cancel_locked() {
  auto** transfer = &pending_transfers;
  while (*transfer && *transfer != this) transfer = &(*transfer)->next_transfer_;
  if (*transfer) *transfer = next_transfer_;
  next_transfer_ = nullptr;
  if (file_) { std::fclose(file_); file_ = nullptr; }
  if (directory_handle_) { closedir(directory_handle_); directory_handle_ = nullptr; }
  if (owns_staging_) std::remove(staging_.c_str());
  owns_staging_ = false;
  stage_ = Stage::idle;
  input_ = {};
  output_ = {};
}

PrintPreviewCache::Transfer::Result PrintPreviewCache::Transfer::fail() {
  cancel_locked();
  return Result::failed;
}

void PrintPreviewCache::Transfer::begin_write(const PrintPreviewCache& cache,
    std::string_view key, std::span<const std::uint8_t> bytes) {
  const std::lock_guard lock(cache_mutex);
  cancel_locked();
  if (asset_writers) return;
  if (key.empty() || bytes.size() > kMaximumBytes || !valid_image(bytes)) return;
  directory_ = cache.directory_;
  destination_ = cache.path(key);
  staging_ = destination_ + ".part";
  identity_ = identity(key);
  checksum_ = 14695981039346656037ULL;
  offset_ = used_ = 0;
  input_ = bytes;
  stage_ = Stage::prepare;
  next_transfer_ = pending_transfers;
  pending_transfers = this;
}

void PrintPreviewCache::Transfer::begin_read(const PrintPreviewCache& cache,
    std::string_view key, std::span<std::uint8_t> bytes) {
  const std::lock_guard lock(cache_mutex);
  cancel_locked();
  if (key.empty() || bytes.size() < 33 || bytes.size() > kMaximumBytes) return;
  directory_ = cache.directory_;
  destination_ = cache.path(key);
  identity_ = identity(key);
  checksum_ = 14695981039346656037ULL;
  offset_ = 0;
  output_ = bytes;
  stage_ = Stage::open_read;
  next_transfer_ = pending_transfers;
  pending_transfers = this;
}

PrintPreviewCache::Transfer::Result PrintPreviewCache::Transfer::step() {
  const std::lock_guard lock(cache_mutex);
  switch (stage_) {
    case Stage::idle: return Result::failed;
    case Stage::prepare:
      if (mkdir(directory_.c_str(), 0700) != 0 && errno != EEXIST) return fail();
      directory_handle_ = opendir(directory_.c_str());
      if (!directory_handle_) return fail();
      stage_ = Stage::scan;
      return Result::pending;
    case Stage::scan:
    case Stage::evict: {
      if (dirent* entry = readdir(directory_handle_)) {
        const std::string_view name(entry->d_name);
        if (name.size() == 25 && name.ends_with(".bin.part")) {
          std::remove((directory_ + "/" + entry->d_name).c_str());
        } else if (name.size() == 20 && name.ends_with(".bin")) {
          const std::string candidate = directory_ + "/" + entry->d_name;
          // Keep the previous destination valid until the replacement commits.
          if (candidate != destination_) {
            if (stage_ == Stage::evict) std::remove(candidate.c_str());
            else {
              struct stat info{};
              if (stat(candidate.c_str(), &info) == 0) used_ += info.st_size;
            }
          }
        }
        return Result::pending;
      }
      closedir(directory_handle_);
      directory_handle_ = nullptr;
      if (stage_ == Stage::scan && used_ + sizeof(Header) + input_.size() > 2 * 1024 * 1024) {
        directory_handle_ = opendir(directory_.c_str());
        if (!directory_handle_) return fail();
        stage_ = Stage::evict;
      } else stage_ = Stage::space;
      return Result::pending;
    }
    case Stage::space: {
#ifdef ESP_PLATFORM
      std::size_t total = 0, occupied = 0;
      if (esp_littlefs_info("assets", &total, &occupied) != ESP_OK || occupied > total ||
          total - occupied < input_.size() + sizeof(Header) + 128 * 1024) return fail();
#endif
      stage_ = Stage::open_write;
      return Result::pending;
    }
    case Stage::open_write: {
      file_ = std::fopen(staging_.c_str(), "wb");
      if (!file_) return fail();
      owns_staging_ = true;
      const Header header{kMagic, static_cast<std::uint32_t>(input_.size()), identity_, 0};
      if (std::fwrite(&header, sizeof(header), 1, file_) != 1) return fail();
      stage_ = Stage::write_data;
      return Result::pending;
    }
    case Stage::write_data: {
      const auto bytes = input_.subspan(offset_, std::min(kChunkBytes, input_.size() - offset_));
      if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) return fail();
      for (auto byte : bytes) { checksum_ ^= byte; checksum_ *= 1099511628211ULL; }
      offset_ += bytes.size();
      if (offset_ == input_.size()) stage_ = Stage::commit;
      return Result::pending;
    }
    case Stage::commit: {
      const Header header{kMagic, static_cast<std::uint32_t>(input_.size()), identity_, checksum_};
      if (std::fseek(file_, 0, SEEK_SET) != 0 ||
          std::fwrite(&header, sizeof(header), 1, file_) != 1) return fail();
      const bool closed = std::fclose(file_) == 0;
      file_ = nullptr;
      if (!closed || std::rename(staging_.c_str(), destination_.c_str()) != 0) return fail();
      owns_staging_ = false;
      cancel_locked();
      return Result::complete;
    }
    case Stage::open_read: {
      file_ = std::fopen(destination_.c_str(), "rb");
      Header header{};
      struct stat info{};
      if (!file_ || std::fread(&header, sizeof(header), 1, file_) != 1 ||
          header.magic != kMagic || header.identity != identity_ || header.size != output_.size() ||
          fstat(fileno(file_), &info) != 0 ||
          info.st_size != static_cast<off_t>(sizeof(Header) + header.size)) return fail();
      expected_checksum_ = header.checksum;
      stage_ = Stage::read_data;
      return Result::pending;
    }
    case Stage::read_data: {
      const auto bytes = output_.subspan(offset_, std::min(kChunkBytes, output_.size() - offset_));
      if (std::fread(bytes.data(), 1, bytes.size(), file_) != bytes.size()) return fail();
      for (auto byte : bytes) { checksum_ ^= byte; checksum_ *= 1099511628211ULL; }
      offset_ += bytes.size();
      if (offset_ != output_.size()) return Result::pending;
      if (!valid_image(output_) || checksum_ != expected_checksum_) return fail();
      cancel_locked();
      return Result::complete;
    }
  }
  return fail();
}

std::string PrintPreviewCache::path(std::string_view key) const {
  char name[32];
  std::snprintf(name, sizeof(name), "/%016llx.bin",
                static_cast<unsigned long long>(identity(key)));
  return directory_ + name;
}

std::size_t PrintPreviewCache::size(std::string_view key) const {
  const std::lock_guard lock(cache_mutex);
  if (key.empty()) return 0;
  FILE* file = std::fopen(path(key).c_str(), "rb");
  Header header{};
  const bool valid = read_header(file, key, header);
  if (file) std::fclose(file);
  return valid ? header.size : 0;
}

bool PrintPreviewCache::read(std::string_view key, std::span<std::uint8_t> bytes) const {
  const std::lock_guard lock(cache_mutex);
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
  Transfer transfer;
  transfer.begin_write(*this, key, bytes);
  Transfer::Result result;
  do { result = transfer.step(); } while (result == Transfer::Result::pending);
  return result == Transfer::Result::complete;
}

void PrintPreviewCache::remove(std::string_view key) const {
  const std::lock_guard lock(cache_mutex);
  if (key.empty()) return;
  const std::string filename = path(key);
  std::remove(filename.c_str());
  std::remove((filename + ".part").c_str());
}

std::size_t PrintPreviewCache::reclaim() const {
  const std::lock_guard lock(cache_mutex);
  if (!asset_writers) return 0;
  // LittleFS refuses to unlink files with open descriptors. Cancel any cache
  // reads only when reclamation is needed, so admission does not depend on the
  // preview worker finishing a read first. It can fetch that thumbnail again.
  for (auto* transfer = pending_transfers; transfer;) {
    auto* next = transfer->next_transfer_;
    if (transfer->directory_ == directory_) transfer->cancel_locked();
    transfer = next;
  }
  DIR* directory = opendir(directory_.c_str());
  if (!directory) return 0;
  std::size_t removed = 0;
  while (const auto* entry = readdir(directory)) {
    if (!owned_cache_file(entry->d_name)) continue;
    const auto candidate = directory_ + "/" + entry->d_name;
    struct stat info{};
    if (stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
        std::remove(candidate.c_str()) == 0) removed += info.st_size;
  }
  closedir(directory);
  return removed;
}
}  // namespace printdeck::platform
