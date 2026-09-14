#include "printdeck/platform/camera_udp_prefetch.h"
#include "printdeck/platform/camera_udp_prefetch_transport.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

enum { SLOT_COUNT = 64, SLOT_BYTES = 1400, LOW_WATER = 32,
       REAL_LIMIT = 128, DELIVERY_LIMIT = 128, FILL_LIMIT = 64, FILL_US = 2000 };
typedef struct {
    uint8_t payload[SLOT_BYTES];
    esp_peer_addr_t address;
    int result, error_number;
    int64_t queued_us;
} Slot;
typedef struct {
    Slot *slots;
    udp_socket_t *socket;
    uint32_t generation;
    unsigned head, count, attempts, deliveries;
    bool connected, disabled, fault, error_pending;
    printdeck_udp_prefetch_stats stats;
} Queue;
static _Atomic(uintptr_t) owner;
static Queue queue;

static bool owns(void) {
    const uintptr_t task = atomic_load_explicit(&owner, memory_order_acquire);
    return task != 0 && task == printdeck_camera_udp_prefetch_task();
}
static void clear_packets(void) {
    queue.head = queue.count = 0;
    queue.socket = NULL;
    queue.error_pending = false;
}
static int fault(void) {
    if (!queue.fault) ++queue.stats.contract_faults;
    queue.fault = true;
    clear_packets();
    errno = EMSGSIZE;
    return -1;
}
static int no_data(void) { errno = EWOULDBLOCK; return 0; }
static bool can_read(void) {
    if (queue.attempts < REAL_LIMIT) return true;
    ++queue.stats.attempt_limits;
    return false;
}
static int read_one(udp_socket_t *socket, esp_peer_addr_t *address, uint8_t *buffer,
                    int length, bool nowait, printdeck_udp_receive_fn real) {
    ++queue.attempts;
    ++queue.stats.real_attempts;
    return real(socket, address, buffer, length, nowait);
}

bool printdeck_camera_udp_prefetch_begin(uint32_t generation) {
    const uintptr_t current = printdeck_camera_udp_prefetch_task();
    if (!current) return false;
    if (atomic_load_explicit(&owner, memory_order_acquire) == current)
        printdeck_camera_udp_prefetch_end();
    uintptr_t vacant = 0;
    if (!atomic_compare_exchange_strong_explicit(&owner, &vacant, current,
                                                 memory_order_acq_rel, memory_order_acquire))
        return false;
    memset(&queue, 0, sizeof(queue));
    queue.generation = generation;
    queue.stats.allocation_requested = SLOT_COUNT * sizeof(Slot);
    queue.slots = heap_caps_malloc(queue.stats.allocation_requested,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    queue.disabled = queue.slots == NULL;
    if (queue.slots) queue.stats.allocation_actual = heap_caps_get_allocated_size(queue.slots);
    return !queue.disabled;
}
void printdeck_camera_udp_prefetch_end(void) {
    if (!owns()) return;
    heap_caps_free(queue.slots);
    memset(&queue, 0, sizeof(queue));
    atomic_store_explicit(&owner, 0, memory_order_release);
}
void printdeck_camera_udp_prefetch_connected(uint32_t generation, bool connected) {
    if (!owns() || generation != queue.generation) return;
    if (queue.connected != connected) clear_packets();
    queue.connected = connected;
}
void printdeck_camera_udp_prefetch_slice_begin(void) {
    if (!owns()) return;
    queue.attempts = queue.deliveries = 0;
}
bool printdeck_camera_udp_prefetch_faulted(void) { return owns() && queue.fault; }
printdeck_udp_prefetch_stats printdeck_camera_udp_prefetch_stats(void) {
    printdeck_udp_prefetch_stats result = {0};
    if (owns()) { result = queue.stats; result.queued = queue.count; }
    return result;
}
size_t printdeck_camera_udp_prefetch_slot_bytes(void) { return sizeof(Slot); }
size_t printdeck_camera_udp_prefetch_state_bytes(void) { return sizeof(queue) + sizeof(owner); }
void printdeck_camera_udp_prefetch_socket_closing(udp_socket_t *socket) {
    // The pinned peer closes its transport on the owning receiver before reuse.
    if (owns() && queue.socket == socket) clear_packets();
}

static void copy_address(esp_peer_addr_t *target, const esp_peer_addr_t *source) {
    // Match the fields changed by pinned udp_socket_recv_dispatch. In particular,
    // IPv4 must not overwrite the caller's remaining twelve union bytes/padding.
    if (source->family == AF_INET || source->family == AF_INET6) {
        target->family = source->family;
        target->port = source->port;
        memcpy(target->ipv6, source->ipv6, source->family == AF_INET ? 4 : 16);
    }
}
static void fill(printdeck_udp_receive_fn real) {
    if (queue.count > LOW_WATER || queue.error_pending || queue.fault) return;
    int next_errno = errno;
    ++queue.stats.fill_calls;
    const int64_t started = esp_timer_get_time();
    for (unsigned attempt = 0; attempt < FILL_LIMIT && queue.count < SLOT_COUNT; ++attempt) {
        if (!queue.connected || printdeck_camera_udp_prefetch_scope() != 1) break;
        if (!can_read()) break;
        if (esp_timer_get_time() - started >= FILL_US) {
            ++queue.stats.fill_deadlines;
            break;
        }
        Slot *slot = &queue.slots[(queue.head + queue.count) % SLOT_COUNT];
        memset(&slot->address, 0, sizeof(slot->address));
        errno = next_errno;
        ++queue.stats.prefetch_attempts;
        const int result = read_one(queue.socket, &slot->address, slot->payload,
                                    SLOT_BYTES, true, real);
        const int result_errno = errno;
        next_errno = result_errno;
        if (result == 0) break;  // Includes quota EWOULDBLOCK: never enqueue it.
        if (result > SLOT_BYTES) { fault(); break; }
        slot->result = result;
        slot->error_number = result_errno;
        slot->queued_us = esp_timer_get_time();
        ++queue.count;
        if (queue.count > queue.stats.peak_queued) queue.stats.peak_queued = queue.count;
        if (result < 0) {
            queue.error_pending = true;
            ++queue.stats.queued_errors;
            break;
        }
    }
}

int printdeck_camera_udp_prefetch_receive(udp_socket_t *socket, esp_peer_addr_t *address,
                                        uint8_t *buffer, int length, bool nowait,
                                        printdeck_udp_receive_fn real) {
    const int incoming_errno = errno;
    if (!owns() || queue.disabled) {
        errno = incoming_errno;
        return real(socket, address, buffer, length, nowait);
    }
    const int scope = printdeck_camera_udp_prefetch_scope();
    if (scope == 2) { clear_packets(); return no_data(); }
    if (queue.fault) { errno = EMSGSIZE; return -1; }
    if (!queue.connected || scope == 0) {
        if (scope == 0 && queue.connected && queue.count) return fault();
        if (!queue.connected) clear_packets();
        errno = incoming_errno;
        return real(socket, address, buffer, length, nowait);
    }
    if (queue.deliveries >= DELIVERY_LIMIT) {
        ++queue.stats.delivery_limits;
        return no_data();  // Keep queued packets for the next slice.
    }
    if (queue.socket && queue.socket != socket && socket) {
        ++queue.stats.bypass_sockets;
        if (!can_read()) return no_data();
        errno = incoming_errno;
        const int result = read_one(socket, address, buffer, length, nowait, real);
        const int result_errno = errno;
        if (result > 0 && printdeck_camera_udp_prefetch_scope() != 1) {
            clear_packets(); return no_data();
        }
        if (result > 0) { ++queue.deliveries; ++queue.stats.delivered; }
        errno = result_errno;
        return result;
    }
    if (!socket || !address || !buffer || length <= 0 || length > SLOT_BYTES) {
        // A nonempty queue cannot safely bypass a contract violation: that would
        // reorder data or pretend truncated bytes are recoverable. Restart once.
        if (queue.count) return fault();
        queue.disabled = true;
        errno = incoming_errno;
        return real(socket, address, buffer, length, nowait);
    }
    if (!queue.socket) queue.socket = socket;
    if (queue.count) {
        Slot *slot = &queue.slots[queue.head];
        const int result = slot->result > length ? length : slot->result;
        const int result_errno = slot->error_number;
        const int64_t age = esp_timer_get_time() - slot->queued_us;
        if (age > 0 && (uint64_t)age > queue.stats.max_age_us) queue.stats.max_age_us = (uint64_t)age;
        if (printdeck_camera_udp_prefetch_scope() != 1 || !queue.connected) {
            clear_packets(); return no_data();
        }
        if (result > 0) {
            memcpy(buffer, slot->payload, (size_t)result);
            copy_address(address, &slot->address);
        } else queue.error_pending = false;
        queue.head = (queue.head + 1) % SLOT_COUNT;
        --queue.count;
        errno = result_errno;
        if (result > 0) fill(real);
        if (result > 0 && printdeck_camera_udp_prefetch_scope() != 1) {
            clear_packets(); return no_data();
        }
        if (result > 0) {
            ++queue.deliveries; ++queue.stats.delivered;
            ++queue.stats.cached_deliveries;
            queue.stats.cached_bytes += (unsigned)result;
        }
        errno = result_errno;
        return result;
    }
    if (!can_read()) return no_data();
    errno = incoming_errno;
    const int result = read_one(socket, address, buffer, length, nowait, real);
    const int result_errno = errno;
    if (result > 0) {
        fill(real);  // Uses separate slots; caller data/address remain untouched.
        if (printdeck_camera_udp_prefetch_scope() != 1) {
            clear_packets(); return no_data();
        }
        ++queue.deliveries; ++queue.stats.delivered;
    }
    errno = result_errno;
    return result;
}
