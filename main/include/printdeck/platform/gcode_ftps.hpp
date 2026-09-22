#pragma once
#include "printdeck/platform/prusalink_client.hpp"

namespace printdeck::platform {
// Original files only. The caller owns the network gate and the selected-job lease.
PrusaLinkHttpResponse gcode_ftps_range(const core::PrinterProfile& profile,
    const std::string& path, std::uint64_t offset, std::uint32_t length,
    std::uint64_t deadline, const std::function<bool()>& cancelled);
}
