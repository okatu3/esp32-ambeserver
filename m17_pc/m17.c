#include "m17.h"

#include <stdbool.h>
#include <string.h>

#define M17_HASH_BASE 262144000000000ull   /* 40^9 = 0xEE6B28000000 */

static const char CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";

/* CRC-16 of the M17 specification: polynomial 0x5935, initial value 0xFFFF, no reflection, no final XOR. */
uint16_t m17_crc(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x5935) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static int char_value(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    const char *p = strchr(CHARSET, c);
    return (p && c) ? (int)(p - CHARSET) : 0;
}

void m17_encode_call(const char *call, uint8_t out[6])
{
    if (!strcmp(call, "@ALL")) {
        memset(out, 0xFF, 6);
        return;
    }
    /* "#NAME" (e.g. #PARROT, #BOOT): the spec's hash-address range, 40^9 + the base-40 value of NAME */
    bool hash = call[0] == '#';
    if (hash) {
        ++call;
    }
    uint64_t v = 0;
    size_t n = strlen(call);
    if (n > (hash ? 8u : 9u)) {
        n = hash ? 8u : 9u;
    }
    for (size_t i = n; i-- > 0;) {                /* the first character is the least significant digit */
        v = v * 40 + (uint64_t)char_value(call[i]);
    }
    if (hash) {
        v += M17_HASH_BASE;
    }
    for (int i = 5; i >= 0; --i) {
        out[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

void m17_decode_call(const uint8_t in[6], char out[M17_CALL_LEN])
{
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) {
        v = (v << 8) | in[i];
    }
    if (v == 0xFFFFFFFFFFFFull) {
        strcpy(out, "@ALL");
        return;
    }
    int n = 0;
    if (v >= M17_HASH_BASE) {                     /* 40^9 and up: "#NAME" */
        v -= M17_HASH_BASE;
        out[n++] = '#';
    }
    while (v && n < M17_CALL_LEN - 1) {
        out[n++] = CHARSET[v % 40];
        v /= 40;
    }
    out[n] = 0;
    while (n && out[n - 1] == ' ') {
        out[--n] = 0;
    }
}

static size_t magic_call(const char *magic, const char *call, uint8_t *out)
{
    memcpy(out, magic, 4);
    m17_encode_call(call, out + 4);
    return 10;
}

size_t m17_conn(const char *call, char module, uint8_t *out)
{
    magic_call("CONN", call, out);
    out[10] = (uint8_t)module;
    return 11;
}

size_t m17_pong(const char *call, uint8_t *out)
{
    return magic_call("PONG", call, out);
}

size_t m17_disc(const char *call, uint8_t *out)
{
    return magic_call("DISC", call, out);
}

void m17_build_stream(const m17_stream_t *s, uint8_t out[M17_STREAM_LEN])
{
    memcpy(out, "M17 ", 4);
    out[4] = (uint8_t)(s->stream_id >> 8);
    out[5] = (uint8_t)s->stream_id;
    m17_encode_call(s->dst, out + 6);
    m17_encode_call(s->src, out + 12);
    out[18] = (uint8_t)(s->type >> 8);
    out[19] = (uint8_t)s->type;
    memcpy(out + 20, s->meta, 14);
    uint16_t fn = (uint16_t)((s->fn & 0x7FFF) | (s->last ? M17_FN_LAST : 0));
    out[34] = (uint8_t)(fn >> 8);
    out[35] = (uint8_t)fn;
    memcpy(out + 36, s->payload, 16);
    uint16_t crc = m17_crc(out, 52);
    out[52] = (uint8_t)(crc >> 8);
    out[53] = (uint8_t)crc;
}

m17_pkt_t m17_parse(const uint8_t *p, size_t n)
{
    m17_pkt_t k;
    memset(&k, 0, sizeof(k));
    if (n < 4) {
        k.kind = MP_NONE;
        return k;
    }
    if (n == M17_STREAM_LEN && !memcmp(p, "M17 ", 4)) {
        k.kind = MP_STREAM;
        k.s.stream_id = (uint16_t)(p[4] << 8 | p[5]);
        m17_decode_call(p + 6, k.s.dst);
        m17_decode_call(p + 12, k.s.src);
        k.s.type = (uint16_t)(p[18] << 8 | p[19]);
        memcpy(k.s.meta, p + 20, 14);
        uint16_t fn = (uint16_t)(p[34] << 8 | p[35]);
        k.s.fn = fn & 0x7FFF;
        k.s.last = (fn & M17_FN_LAST) != 0;
        memcpy(k.s.payload, p + 36, 16);
        k.crc_ok = m17_crc(p, 52) == (uint16_t)(p[52] << 8 | p[53]);
    } else if (!memcmp(p, "ACKN", 4)) {
        k.kind = MP_ACKN;
    } else if (!memcmp(p, "NACK", 4)) {
        k.kind = MP_NACK;
    } else if (!memcmp(p, "PING", 4)) {
        k.kind = MP_PING;
        if (n >= 10) {
            m17_decode_call(p + 4, k.from);
        }
    } else if (!memcmp(p, "DISC", 4)) {
        k.kind = MP_DISC;
    } else {
        k.kind = MP_OTHER;
    }
    return k;
}

const char *m17_type_name(uint16_t type)
{
    if (!(type & 1)) {
        return "PKT";
    }
    switch ((type >> 1) & 3) {
    case 1: return "DATA";
    case 2: return "3200";
    case 3: return "1600";
    }
    return "?";
}
