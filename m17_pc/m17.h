#pragma once
/* M17 over the internet, as M17 reflectors (mrefd, UDP 17000) speak it:
 *   client -> reflector  CONN + callsign(6) + module(1)   = 11 bytes   link to one module (A..Z)
 *                        PONG + callsign(6)               = 10 bytes   answer to PING
 *                        DISC + callsign(6)               = 10 bytes   unlink
 *   reflector -> client  ACKN / NACK (4), PING + reflector callsign (10), DISC (4 or 10)
 *   both ways            stream packet, 54 bytes, one per 40 ms:
 *       "M17 " | stream id (2) | LSF without CRC: DST(6) SRC(6) TYPE(2) META(14) | FN(2) | payload(16) | CRC(2)
 *       FN counts up from 0, its top bit marks the last packet of a stream; CRC = M17 CRC-16 of bytes 0..51.
 * Callsigns are base-40 coded in 6 bytes (big endian), up to 9 characters of " A-Z0-9-/.";
 * 0xFFFFFFFFFFFF is the broadcast "@ALL". Voice: Codec 2 3200 (two 8-byte 20 ms frames per packet, TYPE 0x0005)
 * or Codec 2 1600 + 8 bytes of data (one 8-byte 40 ms frame, TYPE 0x0007). Big endian everywhere. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M17_PORT          17000
#define M17_STREAM_LEN    54
#define M17_FN_LAST       0x8000u
#define M17_TYPE_V3200    0x0005u          /* stream, voice only (Codec 2 3200) */
#define M17_TYPE_V1600    0x0007u          /* stream, voice + data (Codec 2 1600) */
#define M17_CALL_LEN      10               /* 9 characters + NUL */

uint16_t m17_crc(const uint8_t *p, size_t n);
void m17_encode_call(const char *call, uint8_t out[6]);            /* "@ALL" -> broadcast; bad chars -> space */
void m17_decode_call(const uint8_t in[6], char out[M17_CALL_LEN]); /* trailing spaces removed */

size_t m17_conn(const char *call, char module, uint8_t *out);      /* returns the length to send */
size_t m17_pong(const char *call, uint8_t *out);
size_t m17_disc(const char *call, uint8_t *out);

typedef struct {
    uint16_t stream_id;
    char dst[M17_CALL_LEN], src[M17_CALL_LEN];
    uint16_t type;
    uint8_t meta[14];
    uint16_t fn;                            /* without the last-packet bit */
    bool last;
    uint8_t payload[16];
} m17_stream_t;

void m17_build_stream(const m17_stream_t *s, uint8_t out[M17_STREAM_LEN]);

typedef enum { MP_NONE, MP_ACKN, MP_NACK, MP_PING, MP_DISC, MP_STREAM, MP_OTHER } m17_kind_t;

typedef struct {
    m17_kind_t kind;
    bool crc_ok;                            /* MP_STREAM only */
    m17_stream_t s;                         /* MP_STREAM */
    char from[M17_CALL_LEN];                /* MP_PING: the reflector's callsign */
} m17_pkt_t;

m17_pkt_t m17_parse(const uint8_t *p, size_t n);
const char *m17_type_name(uint16_t type);  /* "3200", "1600", "DATA", "PKT", "?" */
