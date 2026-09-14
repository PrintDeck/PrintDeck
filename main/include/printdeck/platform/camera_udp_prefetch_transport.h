#pragma once
// C-only typed boundary: use the exact pinned upstream header, never its layout.
#include "udp.h"
typedef int (*printdeck_udp_receive_fn)(udp_socket_t *, esp_peer_addr_t *, uint8_t *, int, bool);
int printdeck_camera_udp_prefetch_receive(udp_socket_t *, esp_peer_addr_t *, uint8_t *,
                                        int, bool, printdeck_udp_receive_fn);
void printdeck_camera_udp_prefetch_socket_closing(udp_socket_t *);
