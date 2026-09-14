#include "printdeck/core/reaction_sd_store.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace printdeck::core {
namespace {
std::uint32_t checksum(std::span<const std::uint8_t> bytes) {
  std::uint32_t crc = 0xffffffffU;
  for (auto byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
ReactionGif read(const std::string& path, std::size_t expected) {
  auto image = ReactionGifBytes::read(path, ReactionSdStore::maximum_bytes);
  return image && image->size() == expected ? image : ReactionGif{};
}
bool write_new(const std::string& path, std::span<const std::uint8_t> bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return false;
  FILE* file = ::fdopen(fd, "wb");
  if (!file) { ::close(fd); ::unlink(path.c_str()); return false; }
  const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
                       std::fflush(file) == 0 && ::fsync(fd) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!written || !closed) { ::unlink(path.c_str()); return false; }
  return true;
}
}  // namespace

ReactionGifBytes::~ReactionGifBytes() { std::free(data_); }

bool ReactionGifBytes::operator==(const ReactionGifBytes& other) const {
  return size_ == other.size_ && std::memcmp(data_, other.data_, size_) == 0;
}

ReactionGif ReactionGifBytes::copy(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > ReactionSdStore::maximum_bytes) return {};
  auto* data = static_cast<std::uint8_t*>(std::malloc(bytes.size()));
  if (!data) return {};
  auto* object = new (std::nothrow) ReactionGifBytes(data, bytes.size());
  if (!object) { std::free(data); return {}; }
  std::memcpy(data, bytes.data(), bytes.size());
  return ReactionGif(object);
}

ReactionGif ReactionGifBytes::read(const std::string& path, std::size_t maximum) {
  struct stat info{};
  if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<std::uint64_t>(info.st_size) > maximum ||
      static_cast<std::uint64_t>(info.st_size) > ReactionSdStore::maximum_bytes) return {};
  const auto length = static_cast<std::size_t>(info.st_size);
  auto* data = static_cast<std::uint8_t*>(std::malloc(length));
  if (!data) return {};
  auto* object = new (std::nothrow) ReactionGifBytes(data, length);
  if (!object) { std::free(data); return {}; }
  ReactionGif image(object);
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return {};
  const bool complete = std::fread(data, 1, length, file) == length &&
                        std::fgetc(file) == EOF && std::ferror(file) == 0;
  const bool closed = std::fclose(file) == 0;
  return complete && closed ? image : ReactionGif{};
}

bool ReactionSdStore::initialize(const ReactionSdIndex& index,
                                 std::uint16_t dimension, Persist persist,
                                 Random random) {
  if (index.version != 1 || index.owner == 0) return false;
  std::size_t total = 0;
  for (const auto& record : index.records) {
    if (record.reserved != 0 || record.bytes > maximum_bytes ||
        (record.file == 0) != (record.bytes == 0) ||
        (record.file == 0) != (record.card == 0)) return false;
    total += record.bytes;
    if (total > maximum_bytes) return false;
  }
  index_ = index;
  dimension_ = dimension;
  persist_ = std::move(persist);
  random_ = std::move(random);
  return true;
}

bool ReactionSdStore::valid(std::span<const std::uint8_t> bytes) const {
  GifMetadata metadata;
  return !bytes.empty() && bytes.size() <= maximum_bytes &&
         inspect_gif(bytes, metadata, dimension_, maximum_frames);
}

std::string ReactionSdStore::path(const ReactionSdRecord& record) const {
  // All card paths use FAT 8.3 names; no directory scanning or user path input.
  char name[24];
  std::snprintf(name, sizeof(name), "/%08lx.gif", static_cast<unsigned long>(record.file));
  return root_ + name;
}

bool ReactionSdStore::mount(std::string root, std::uint64_t card) {
  unmount();
  if (!card || !persist_) return false;
  root_ = std::move(root);
  if (::mkdir(root_.c_str(), 0700) != 0 && errno != EEXIST) return false;
  card_ = card;
  for (std::size_t i = 0; i < cache_.size(); ++i) {
    const auto& record = index_.records[i];
    if (!record.file || record.card != card_) continue;
    auto image = read(path(record), record.bytes);
    if (image && valid(*image) && checksum(*image) == record.checksum)
      cache_[i] = std::move(image);
  }
  return true;
}

void ReactionSdStore::unmount() {
  cache_.fill({});
  card_ = 0;
}

std::size_t ReactionSdStore::missing() const {
  std::size_t count = 0;
  for (std::size_t i = 0; i < cache_.size(); ++i)
    if (owned(i) && !cache_[i]) ++count;
  return count;
}

std::size_t ReactionSdStore::bytes() const {
  std::size_t total = 0;
  for (const auto& record : index_.records) total += record.bytes;
  return total;
}

void ReactionSdStore::remove_replaced(const ReactionSdIndex& previous) {
  // Only files selected by this device's previous index may be removed.
  for (std::size_t i = 0; i < cache_.size(); ++i) {
    const auto& old = previous.records[i];
    if (old.file && old.card == card_ && old.file != index_.records[i].file)
      ::unlink(path(old).c_str());
  }
}

bool ReactionSdStore::save(const ReactionGifArray& changes) {
  if (!mounted()) return false;
  auto next = index_;
  std::size_t total = 0;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    if (changes[i] && !valid(*changes[i])) return false;
    total += changes[i] ? changes[i]->size() : next.records[i].bytes;
    if (total > maximum_bytes) return false;
  }
  std::vector<std::string> created;
  const auto rollback = [&] { for (const auto& file : created) ::unlink(file.c_str()); };
  for (std::size_t i = 0; i < changes.size(); ++i) {
    if (!changes[i]) continue;
    ReactionSdRecord record{};
    record.card = card_;
    record.bytes = changes[i]->size();
    record.checksum = checksum(*changes[i]);
    bool written = false;
    for (int attempt = 0; attempt < 8 && !written; ++attempt) {
      record.file = random_();
      if (!record.file) continue;
      written = write_new(path(record), *changes[i]);
      if (!written && errno != EEXIST) break;
    }
    if (!written) { rollback(); return false; }
    created.push_back(path(record));
    auto verified = read(path(record), record.bytes);
    if (!verified || *verified != *changes[i]) { rollback(); return false; }
    next.records[i] = record;
  }
  // A failed persistence call may have reached durable storage before its
  // acknowledgement failed. Retain the candidates so either index can reboot.
  if (!persist_(next)) return false;
  const auto previous = index_;
  index_ = next;
  for (std::size_t i = 0; i < changes.size(); ++i)
    if (changes[i]) cache_[i] = changes[i];
  remove_replaced(previous);
  return true;
}

bool ReactionSdStore::forget(std::size_t i) {
  if (i >= cache_.size()) return false;
  if (!owned(i)) return true;
  auto next = index_;
  next.records[i] = {};
  if (!persist_(next)) return false;
  const auto previous = index_;
  index_ = next;
  cache_[i].reset();
  remove_replaced(previous);
  return true;
}

bool ReactionSdStore::clear() {
  auto next = index_;
  next.records = {};
  if (!persist_(next)) return false;
  const auto previous = index_;
  index_ = next;
  cache_.fill({});
  remove_replaced(previous);
  return true;
}

bool ReactionSdStore::copy_to_internal(const std::string& root) const {
  if (missing()) return false;
  for (std::size_t i = 0; i < cache_.size(); ++i) {
    if (!owned(i)) continue;
    const auto& image = cache_[i];
    if (!image) return false;
    const auto destination = root + "/" + std::string(reaction_events()[i].id) + ".gif";
    const auto temporary = destination + ".sdtmp";
    ::unlink(temporary.c_str());
    if (!write_new(temporary, *image)) return false;
    const auto verified = read(temporary, image->size());
    if (!verified || *verified != *image || ::rename(temporary.c_str(), destination.c_str()) != 0) {
      ::unlink(temporary.c_str());
      return false;
    }
  }
  return true;
}
}  // namespace printdeck::core
