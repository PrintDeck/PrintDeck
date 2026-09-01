#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "printdeck/core/job_state.hpp"

namespace printdeck::core {

struct ReactionEventDefinition {
  std::string_view id;
  std::string_view label;
  PrinterActivity activity;
};

inline constexpr std::size_t kReactionEventCount = 18;

const std::array<ReactionEventDefinition, kReactionEventCount>& reaction_events();
const ReactionEventDefinition* reaction_event(std::string_view id);
const ReactionEventDefinition& reaction_event(PrinterActivity activity);
std::size_t reaction_event_index(PrinterActivity activity);

struct GifMetadata {
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::uint16_t frame_count = 0;
  bool animated = false;
};

bool inspect_gif(std::span<const std::uint8_t> bytes, GifMetadata& metadata,
                 std::uint16_t maximum_dimension = 466,
                 std::uint16_t maximum_frames = 120);

}  // namespace printdeck::core
