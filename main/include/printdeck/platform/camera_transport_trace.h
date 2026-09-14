#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Diagnostic bridge only; the C transport wrappers keep the upstream C ABI.
bool printdeck_camera_transport_trace_owned(void);
void printdeck_camera_trace_udp_send(int requested_bytes, int result, int64_t elapsed_us);
void printdeck_camera_trace_udp_receive(bool nowait, int result, int64_t elapsed_us);

#ifdef __cplusplus
}
#endif
