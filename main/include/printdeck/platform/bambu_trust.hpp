#pragma once

namespace printdeck::platform {

// Manufacturer-issued trust anchors used only to verify local-printer TLS.
const char* bambu_trust_anchors();

}  // namespace printdeck::platform
