#include "sdkconfig.h"

#if (defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) || defined(PRINTDECK_K2_UDP_PREFETCH)
#include <errno.h>
#include "esp_timer.h"
#include "udp.h"
#include "printdeck/platform/camera_transport_trace.h"
#ifdef PRINTDECK_K2_UDP_PREFETCH
#include "printdeck/platform/camera_udp_prefetch_transport.h"
extern __typeof__(udp_socket_close) __real_udp_socket_close;
extern __typeof__(udp_socket_close) __wrap_udp_socket_close;
#endif

extern __typeof__(udp_socket_sendto) __real_udp_socket_sendto;
extern __typeof__(udp_socket_sendto) __wrap_udp_socket_sendto;
extern __typeof__(udp_socket_recvfrom_nowait) __real_udp_socket_recvfrom_nowait;
extern __typeof__(udp_socket_recvfrom_nowait) __wrap_udp_socket_recvfrom_nowait;

#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
int __wrap_udp_socket_sendto(udp_socket_t *socket, esp_peer_addr_t *address,
                           const uint8_t *buffer, int length) {
    const int incoming_errno = errno;
    const bool measure = printdeck_camera_transport_trace_owned();
    const int64_t started_us = measure ? esp_timer_get_time() : 0;
    errno = incoming_errno;
    const int result = __real_udp_socket_sendto(socket, address, buffer, length);
    const int result_errno = errno;
    if (measure) {
        const int64_t elapsed_us = esp_timer_get_time() - started_us;
        printdeck_camera_trace_udp_send(length, result, elapsed_us);
    }
    errno = result_errno;
    return result;
}


#endif
static int camera_real_receive(udp_socket_t *socket, esp_peer_addr_t *address,
                                    uint8_t *buffer, int length, bool nowait) {
    const int incoming_errno = errno;
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    const bool measure = printdeck_camera_transport_trace_owned();
    const int64_t started_us = measure ? esp_timer_get_time() : 0;
#endif
    errno = incoming_errno;
    const int result = __real_udp_socket_recvfrom_nowait(socket, address, buffer, length, nowait);
    const int result_errno = errno;
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    if (measure) {
        const int64_t elapsed_us = esp_timer_get_time() - started_us;
        printdeck_camera_trace_udp_receive(nowait, result, elapsed_us);
    }
#endif
    errno = result_errno;
    return result;
}
int __wrap_udp_socket_recvfrom_nowait(udp_socket_t *socket, esp_peer_addr_t *address,
                                    uint8_t *buffer, int length, bool nowait) {
#ifdef PRINTDECK_K2_UDP_PREFETCH
    return printdeck_camera_udp_prefetch_receive(socket, address, buffer, length,
                                                nowait, camera_real_receive);
#else
    return camera_real_receive(socket, address, buffer, length, nowait);
#endif
}
#ifdef PRINTDECK_K2_UDP_PREFETCH
void __wrap_udp_socket_close(udp_socket_t *socket) {
    const int incoming_errno = errno;
    printdeck_camera_udp_prefetch_socket_closing(socket);
    errno = incoming_errno;
    __real_udp_socket_close(socket);
}
#endif
#endif
