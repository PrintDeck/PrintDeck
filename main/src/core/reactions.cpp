#include "printdeck/core/reactions.hpp"

#include <algorithm>

namespace printdeck::core {
namespace {

constexpr std::array<ReactionEventDefinition, kReactionEventCount> kEvents = {{
    {"standby", "Standby", PrinterActivity::standby},
    {"preparing", "Preparing", PrinterActivity::preparing},
    {"nozzle-heating", "Heating nozzle", PrinterActivity::nozzle_heating},
    {"bed-heating", "Heating bed", PrinterActivity::bed_heating},
    {"homing", "Homing toolhead", PrinterActivity::homing},
    {"bed-leveling", "Leveling the bed", PrinterActivity::bed_leveling},
    {"nozzle-cleaning", "Cleaning nozzle", PrinterActivity::nozzle_cleaning},
    {"calibrating", "Calibrating", PrinterActivity::calibrating},
    {"filament-changing", "Changing filament", PrinterActivity::filament_changing},
    {"filament-unloading", "Unloading filament", PrinterActivity::filament_unloading},
    {"filament-loading", "Loading filament", PrinterActivity::filament_loading},
    {"filament-purging", "Purging filament", PrinterActivity::filament_purging},
    {"printing", "Printing", PrinterActivity::printing},
    {"paused", "Paused", PrinterActivity::paused},
    {"completed", "Complete", PrinterActivity::completed},
    {"failed", "Failed", PrinterActivity::failed},
    {"cancelled", "Cancelled", PrinterActivity::cancelled},
    {"unavailable", "Unavailable", PrinterActivity::unknown},
}};

bool skip_sub_blocks(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
  while (cursor < bytes.size()) {
    const std::size_t length = bytes[cursor++];
    if (length == 0) return true;
    if (length > bytes.size() - cursor) return false;
    cursor += length;
  }
  return false;
}

std::uint16_t little_endian_u16(std::span<const std::uint8_t> bytes,
                                std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1] << 8U);
}

}  // namespace

const std::array<ReactionEventDefinition, kReactionEventCount>& reaction_events() {
  return kEvents;
}

const ReactionEventDefinition* reaction_event(std::string_view id) {
  const auto found = std::find_if(kEvents.begin(), kEvents.end(), [id](const auto& event) {
    return event.id == id;
  });
  return found == kEvents.end() ? nullptr : &*found;
}

const ReactionEventDefinition& reaction_event(PrinterActivity activity) {
  const auto found = std::find_if(kEvents.begin(), kEvents.end(), [activity](const auto& event) {
    return event.activity == activity;
  });
  return found == kEvents.end() ? kEvents.back() : *found;
}

std::size_t reaction_event_index(PrinterActivity activity) {
  return static_cast<std::size_t>(&reaction_event(activity) - kEvents.data());
}

bool inspect_gif(std::span<const std::uint8_t> bytes, GifMetadata& metadata,
                 std::uint16_t maximum_dimension, std::uint16_t maximum_frames) {
  metadata = {};
  if (bytes.size() < 14 ||
      (!std::equal(bytes.begin(), bytes.begin() + 6, "GIF87a") &&
       !std::equal(bytes.begin(), bytes.begin() + 6, "GIF89a"))) {
    return false;
  }
  metadata.width = little_endian_u16(bytes, 6);
  metadata.height = little_endian_u16(bytes, 8);
  if (metadata.width == 0 || metadata.height == 0 ||
      metadata.width > maximum_dimension || metadata.height > maximum_dimension) {
    return false;
  }
  std::size_t cursor = 13;
  const std::uint8_t packed = bytes[10];
  if ((packed & 0x80U) != 0) {
    const std::size_t table_bytes = 3U << ((packed & 0x07U) + 1U);
    if (table_bytes > bytes.size() - cursor) return false;
    cursor += table_bytes;
  }
  bool trailer = false;
  while (cursor < bytes.size()) {
    const std::uint8_t marker = bytes[cursor++];
    if (marker == 0x3bU) {
      trailer = true;
      break;
    }
    if (marker == 0x21U) {
      if (cursor >= bytes.size()) return false;
      ++cursor;
      if (!skip_sub_blocks(bytes, cursor)) return false;
      continue;
    }
    if (marker != 0x2cU || bytes.size() - cursor < 9) return false;
    const std::uint16_t left = little_endian_u16(bytes, cursor);
    const std::uint16_t top = little_endian_u16(bytes, cursor + 2);
    const std::uint16_t width = little_endian_u16(bytes, cursor + 4);
    const std::uint16_t height = little_endian_u16(bytes, cursor + 6);
    const std::uint8_t image_packed = bytes[cursor + 8];
    cursor += 9;
    if (width == 0 || height == 0 ||
        static_cast<std::uint32_t>(left) + width > metadata.width ||
        static_cast<std::uint32_t>(top) + height > metadata.height) {
      return false;
    }
    if ((image_packed & 0x80U) != 0) {
      const std::size_t table_bytes = 3U << ((image_packed & 0x07U) + 1U);
      if (table_bytes > bytes.size() - cursor) return false;
      cursor += table_bytes;
    }
    if (cursor >= bytes.size() || bytes[cursor++] == 0) return false;
    if (!skip_sub_blocks(bytes, cursor)) return false;
    if (++metadata.frame_count > maximum_frames) return false;
  }
  metadata.animated = metadata.frame_count > 1;
  return trailer && metadata.frame_count > 0;
}

}  // namespace printdeck::core
