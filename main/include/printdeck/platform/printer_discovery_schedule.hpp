#pragma once

#include <array>
#include <cstddef>
#include <optional>

namespace printdeck::platform {

// One outstanding port per address. A completed port advances that address;
// its lane accepts the next address only after all known services were tried.
class PrinterDiscoverySchedule {
 public:
  static constexpr std::size_t capacity = 5;
  struct Probe { std::size_t address; std::size_t service; std::size_t lane; };

  PrinterDiscoverySchedule(std::size_t addresses, std::size_t services)
      : addresses_(addresses), services_(services) {}

  std::optional<Probe> next() {
    if (services_ == 0) return {};
    for (std::size_t index = 0; index < lanes_.size(); ++index) {
      auto& lane = lanes_[index];
      if (lane.busy) continue;
      if (!lane.assigned) {
        if (next_address_ == addresses_) continue;
        lane = {.address = next_address_++, .assigned = true};
      }
      lane.busy = true;
      return Probe{lane.address, lane.service, index};
    }
    return {};
  }

  bool complete(Probe probe) {
    if (probe.lane >= lanes_.size()) return false;
    auto& lane = lanes_[probe.lane];
    if (!lane.busy || lane.address != probe.address || lane.service != probe.service) return false;
    lane.busy = false;
    if (++lane.service == services_) lane = {};
    return true;
  }

  bool done() const {
    if (services_ == 0) return true;
    if (next_address_ != addresses_) return false;
    for (const auto& lane : lanes_) if (lane.assigned) return false;
    return true;
  }

 private:
  struct Lane {
    std::size_t address = 0;
    std::size_t service = 0;
    bool assigned = false;
    bool busy = false;
  };
  std::array<Lane, capacity> lanes_{};
  std::size_t addresses_;
  std::size_t services_;
  std::size_t next_address_ = 0;
};

}  // namespace printdeck::platform
