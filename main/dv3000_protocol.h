#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transmit callback: must send all `len` bytes (blocking) and return true on success. */
typedef bool (*dv3000_tx_fn)(const uint8_t *data, size_t len);

void dv3000_protocol_init(dv3000_tx_fn tx);

/* Drops any partially received packet. Call when a stream (re)starts, e.g. a new TCP client connects,
 * so half a packet of the previous session can never corrupt the next one. */
void dv3000_protocol_reset_stream(void);

/* Feed raw bytes from the host in chunks of any size (1..N bytes, several packets at once). */
void dv3000_protocol_feed(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
