#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Receiver-owned bounded UDP queue. No socket or payload is exposed by metrics.
typedef struct {
    uint32_t real_attempts, delivered, queued, peak_queued;
    uint32_t prefetch_attempts, cached_deliveries;
    uint64_t cached_bytes;
    uint32_t fill_calls, fill_deadlines, attempt_limits, delivery_limits;
    uint32_t queued_errors, bypass_sockets, contract_faults;
    uint64_t max_age_us;
    size_t allocation_requested, allocation_actual;
} printdeck_udp_prefetch_stats;

bool printdeck_camera_udp_prefetch_begin(uint32_t generation);
void printdeck_camera_udp_prefetch_end(void);
void printdeck_camera_udp_prefetch_connected(uint32_t generation, bool connected);
void printdeck_camera_udp_prefetch_slice_begin(void);
bool printdeck_camera_udp_prefetch_faulted(void);
printdeck_udp_prefetch_stats printdeck_camera_udp_prefetch_stats(void);
size_t printdeck_camera_udp_prefetch_slot_bytes(void);
size_t printdeck_camera_udp_prefetch_state_bytes(void);

// Supplied by CameraReceiveSlice; 0=no scope, 1=active, 2=cancelled.
uintptr_t printdeck_camera_udp_prefetch_task(void);
int printdeck_camera_udp_prefetch_scope(void);
#ifdef __cplusplus
}
#endif
