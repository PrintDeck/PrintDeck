#pragma once

#include "printdeck/platform/prusalink_client.hpp"

namespace printdeck::platform {
// Discovery is identity evidence only, never authenticated compatibility.
bool prusalink_discovery_identity(const PrusaLinkHttpResponse& version,
                                 const PrusaLinkHttpResponse* root,
                                 bool advertised_prusalink_service = false);
}
