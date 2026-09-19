#pragma once

#include <algorithm>
#include <charconv>
#include <optional>
#include "printdeck/core/settings.hpp"

namespace printdeck::core {

enum class PrinterOrderResult { unchanged, changed, invalid, conflict };

inline std::optional<std::vector<std::uint32_t>> parse_printer_order(std::string_view text) {
  if (text.empty() || text.size() > kMaximumProfiles * 11 - 1) return std::nullopt;
  std::vector<std::uint32_t> ids;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto token = text.substr(0, comma);
    std::uint32_t id = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || id == 0 ||
        ids.size() >= kMaximumProfiles || std::find(ids.begin(), ids.end(), id) != ids.end())
      return std::nullopt;
    ids.push_back(id);
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
    if (text.empty()) return std::nullopt;
  }
  return ids;
}

// Compare the full starting order under the settings write lock. A concurrent
// add, delete or reorder must not silently overwrite another browser's change.
inline PrinterOrderResult reorder_printers(DeviceSettings& settings,
    std::string_view expected_text, std::string_view order_text) {
  const auto expected = parse_printer_order(expected_text);
  const auto order = parse_printer_order(order_text);
  if (!expected || !order || expected->size() != order->size()) return PrinterOrderResult::invalid;
  auto a = *expected, b = *order;
  std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
  if (a != b) return PrinterOrderResult::invalid;
  if (settings.profiles.size() != expected->size()) return PrinterOrderResult::conflict;
  for (std::size_t i = 0; i < expected->size(); ++i)
    if (settings.profiles[i].id != (*expected)[i]) return PrinterOrderResult::conflict;
  if (*expected == *order) return PrinterOrderResult::unchanged;
  auto profiles = settings.profiles;
  for (std::size_t i = 0; i < order->size(); ++i)
    profiles[i] = *std::find_if(settings.profiles.begin(), settings.profiles.end(),
        [&](const auto& profile) { return profile.id == (*order)[i]; });
  settings.profiles = std::move(profiles);
  return PrinterOrderResult::changed;
}
}  // namespace printdeck::core
