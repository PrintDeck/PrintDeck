#pragma once
#include <functional>
#include <optional>
#include "printdeck/core/printer_address.hpp"
#include "printdeck/platform/network_service.hpp"
#include "printdeck/platform/printer_discovery_service.hpp"

namespace printdeck::platform {
using PrinterRecoveryCancel = std::function<bool()>;

// Credential-free API identity for vendor Moonraker builds without Zeroconf.
std::string moonraker_host_identity(const core::PrinterProfile& profile,
                                    const PrinterRecoveryCancel& cancelled);
// Used by the existing discovery worker to collect stable Moonraker identities.
std::vector<DiscoveredPrinter> discover_moonraker_identities();
// Learns only from the configured host + service port, after a successful status check.
std::optional<core::PrinterProfile> learn_moonraker_identity(
    const core::PrinterProfile& profile, const PrinterRecoveryCancel& cancelled);
// Fresh check before a new Moonraker connection sends credentials or consumes state.
// Legacy profiles remain usable until a verified identity can be learned.
bool moonraker_endpoint_identity_matches(const core::PrinterProfile& profile);
}  // namespace printdeck::platform
