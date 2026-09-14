#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "printdeck/core/reactions.hpp"

namespace printdeck::core {

constexpr bool reaction_images_fit_internal(std::size_t total, std::size_t used,
    std::size_t image_bytes, std::size_t active_bytes, std::size_t limit,
    std::size_t reserve) {
  return used <= total && reserve <= total - used &&
      image_bytes <= total - used - reserve && active_bytes <= limit;
}

// Versioned NVS index. Files are immutable; one persisted index selects the
// complete transaction. An absent card never makes an older flash image active.
struct ReactionSdRecord {
  std::uint64_t card = 0;
  std::uint32_t file = 0;
  std::uint32_t bytes = 0;
  std::uint32_t checksum = 0;
  std::uint32_t reserved = 0;
};
struct ReactionSdIndex {
  std::uint32_t version = 1;
  std::uint32_t owner = 0;
  std::array<ReactionSdRecord, kReactionEventCount> records{};
};
// Large image allocations fail cleanly instead of throwing on an embedded
// build without C++ exceptions. Readers share immutable bytes across removal.
class ReactionGifBytes {
 public:
  ~ReactionGifBytes();
  const std::uint8_t* data() const { return data_; }
  std::size_t size() const { return size_; }
  const std::uint8_t* begin() const { return data_; }
  const std::uint8_t* end() const { return data_ + size_; }
  bool operator==(const ReactionGifBytes& other) const;
  operator std::span<const std::uint8_t>() const { return {data_, size_}; }
  static std::shared_ptr<const ReactionGifBytes> copy(std::span<const std::uint8_t> bytes);
  static std::shared_ptr<const ReactionGifBytes> read(const std::string& path, std::size_t maximum);
 private:
  ReactionGifBytes(std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
  std::uint8_t* data_;
  std::size_t size_;
};
static_assert(sizeof(ReactionSdRecord) == 24 && sizeof(ReactionSdIndex) == 440);
static_assert(std::is_trivially_copyable_v<ReactionSdIndex>);

using ReactionGif = std::shared_ptr<const ReactionGifBytes>;
using ReactionGifArray = std::array<ReactionGif, kReactionEventCount>;

class ReactionSdStore {
 public:
  using Persist = std::function<bool(const ReactionSdIndex&)>;
  using Random = std::function<std::uint32_t()>;
  bool initialize(const ReactionSdIndex& index, std::uint16_t dimension,
                  Persist persist, Random random);
  bool mount(std::string root, std::uint64_t card, bool retain_payloads = true);
  void unmount();
  // Validate OFF-mode files without retaining their payloads. Enabling builds
  // a complete replacement cache before publication; failure preserves it.
  bool set_payload_retention(bool enabled);
  // All non-null inputs are committed together. Existing files remain selected
  // after any write, verification, removal or NVS failure before commit.
  bool save(const ReactionGifArray& changes);
  bool forget(std::size_t index);
  bool clear();
  // Keep SD files and index intact; the caller commits the destination mode.
  bool copy_to_internal(const std::string& root) const;
  bool owned(std::size_t index) const { return index_.records[index].file != 0; }
  bool available(std::size_t index) const { return available_mask_ & (1UL << index); }
  const ReactionGif& image(std::size_t index) const { return cache_[index]; }
  const ReactionSdIndex& index() const { return index_; }
  bool mounted() const { return card_ != 0; }
  std::size_t missing() const;
  std::size_t bytes() const;
  static constexpr std::size_t maximum_bytes = 1536 * 1024;
  static constexpr std::uint16_t maximum_frames = 10;

 private:
  std::string path(const ReactionSdRecord& record) const;
  bool valid(std::span<const std::uint8_t> bytes) const;
  ReactionGif read_verified(std::size_t index) const;
  void remove_replaced(const ReactionSdIndex& previous);
  ReactionSdIndex index_{};
  ReactionGifArray cache_{};
  std::uint32_t available_mask_ = 0;
  bool retain_payloads_ = true;
  std::string root_;
  std::uint64_t card_ = 0;
  std::uint16_t dimension_ = 0;
  Persist persist_;
  Random random_;
};

}  // namespace printdeck::core
