#pragma once
/* DV3000 over TCP (BlueDV style): a plain byte stream carrying DV3000 packets (0x61 ...), exactly as on
 * the serial link. One client at a time (the codec state is shared); a new connection replaces the old one
 * so a half-open connection can never lock the real client out. */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     connected;
    uint32_t accepted, rx_bytes, tx_bytes, tx_errors;
    uint32_t client_ip;             /* host byte order, 0 = none yet */
    uint16_t client_port;
} tcp_stats_t;

/* Blocking server loop (portable BSD/lwIP sockets; also built on the host for tests). Never returns. */
void tcp_server_run(uint16_t port);

#ifdef ESP_PLATFORM
void tcp_server_start(uint16_t port);   /* creates the server task */
#endif

void tcp_server_get_stats(tcp_stats_t *out);
