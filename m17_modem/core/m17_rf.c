#include "m17_rf.h"

#include <string.h>

#include "m17.h"        /* m17_crc() of m17_pc */

const uint8_t m17_rand_seq[46] = {
    0xD6, 0xB5, 0xE2, 0x30, 0x82, 0xFF, 0x84, 0x62, 0xBA, 0x4E, 0x96, 0x90, 0xD8, 0x98, 0xDD, 0x5D,
    0x0C, 0xC8, 0x52, 0x43, 0x91, 0x1D, 0xF8, 0x6E, 0x68, 0x2F, 0x35, 0xDA, 0x14, 0xEA, 0xCD, 0x76,
    0x19, 0x8D, 0xD5, 0x80, 0xD1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2D, 0x29, 0x78, 0xC3,
};

/* P1: a leading 1, then 15 x [1 0 1 1]; used 8 times over the 488 bits of the LSF */
const uint8_t m17_p1[61] = {
    1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
    1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1,
    0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
};
const uint8_t m17_p2[12] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 };

static const int8_t SYM_OF_DIBIT[4] = { +1, +3, -1, -3 };
static uint32_t s_golay[4096];

uint16_t m17_interleave_index(int i)
{
    return (uint16_t)((45u * (uint32_t)i + 92u * (uint32_t)i * (uint32_t)i) % 368u);
}

/* ------------------------------------------------------------------ Golay (24,12), g(x) = 0xC75 */
uint32_t m17_golay24_encode(uint16_t data12)
{
    uint32_t r = (uint32_t)(data12 & 0xFFF) << 11;
    for (int b = 22; b >= 11; --b) {                 /* polynomial division by g (degree 11) */
        if (r & (1u << b)) {
            r ^= 0xC75u << (b - 11);
        }
    }
    uint32_t cw23 = ((uint32_t)(data12 & 0xFFF) << 11) | (r & 0x7FF);
    uint32_t p = cw23;
    p ^= p >> 16; p ^= p >> 8; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    return (cw23 << 1) | (p & 1);                    /* even parity over all 24 bits */
}

static int popcount32(uint32_t v)
{
    int n = 0;
    while (v) {
        v &= v - 1;
        ++n;
    }
    return n;
}

int m17_golay24_decode(uint32_t cw, uint16_t *data12)
{
    int best = 25, bd = 0;
    for (int d = 0; d < 4096; ++d) {
        int e = popcount32((s_golay[d] ^ cw) & 0xFFFFFF);
        if (e < best) {
            best = e;
            bd = d;
            if (!e) {
                break;
            }
        }
    }
    *data12 = (uint16_t)bd;
    return best <= 3 ? best : -1;
}

void m17_rf_init(void)
{
    static bool done;
    if (!done) {
        for (int d = 0; d < 4096; ++d) {
            s_golay[d] = m17_golay24_encode((uint16_t)d);
        }
        done = true;
    }
}

/* ------------------------------------------------------------------ convolutional code K=5, r=1/2 */
int m17_conv_encode(const uint8_t *bits, int n, uint8_t *out)
{
    uint8_t sr = 0;                                  /* bit0 = D^1 ... bit3 = D^4 */
    int k = 0;
    for (int i = 0; i < n + 4; ++i) {
        uint8_t b = i < n ? (bits[i] & 1) : 0;
        out[k++] = b ^ ((sr >> 2) & 1) ^ ((sr >> 3) & 1);                 /* G1 = 1 + D^3 + D^4 */
        out[k++] = b ^ (sr & 1) ^ ((sr >> 1) & 1) ^ ((sr >> 3) & 1);      /* G2 = 1 + D + D^2 + D^4 */
        sr = (uint8_t)(((sr << 1) | b) & 0x0F);
    }
    return k;
}

static inline uint32_t bm(uint8_t g, uint16_t s)
{
    return g ? (uint32_t)(65535u - s) : s;
}

uint32_t m17_viterbi(const uint16_t *soft, int n_out_bits, uint8_t *bits, int n_data)
{
    enum { MAXSTEP = 260 };
    static uint16_t dec[MAXSTEP];                    /* per step: bit x of each next state */
    uint32_t m[16], nm[16];
    int steps = n_out_bits / 2;
    if (steps > MAXSTEP) {
        steps = MAXSTEP;
    }
    for (int s = 0; s < 16; ++s) {
        m[s] = s ? 0x40000000u : 0;
    }
    for (int t = 0; t < steps; ++t) {
        uint16_t d = 0;
        uint16_t s0 = soft[2 * t], s1 = soft[2 * t + 1];
        for (int nx = 0; nx < 16; ++nx) {
            uint8_t b = nx & 1;
            uint32_t best = 0xFFFFFFFFu;
            int bx = 0;
            for (int x = 0; x < 2; ++x) {
                uint8_t sr = (uint8_t)((nx >> 1) | (x << 3));   /* previous state */
                uint8_t g1 = b ^ ((sr >> 2) & 1) ^ ((sr >> 3) & 1);
                uint8_t g2 = b ^ (sr & 1) ^ ((sr >> 1) & 1) ^ ((sr >> 3) & 1);
                uint32_t c = m[sr] + bm(g1, s0) + bm(g2, s1);
                if (c < best) {
                    best = c;
                    bx = x;
                }
            }
            nm[nx] = best;
            d |= (uint16_t)(bx << nx);
        }
        dec[t] = d;
        memcpy(m, nm, sizeof(m));
    }
    int st = 0;                                      /* flushed: ends in state 0 */
    uint32_t cost = m[0];
    for (int t = steps - 1; t >= 0; --t) {
        uint8_t b = st & 1;
        if (t < n_data) {
            bits[t] = b;
        }
        int x = (dec[t] >> st) & 1;
        st = (st >> 1) | (x << 3);
    }
    return cost;
}

/* ------------------------------------------------------------------ framing helpers */
static void bits_from_bytes(const uint8_t *by, int nbits, uint8_t *bits)
{
    for (int i = 0; i < nbits; ++i) {
        bits[i] = (by[i >> 3] >> (7 - (i & 7))) & 1;
    }
}

static void bytes_from_bits(const uint8_t *bits, int nbits, uint8_t *by)
{
    memset(by, 0, (size_t)(nbits + 7) / 8);
    for (int i = 0; i < nbits; ++i) {
        by[i >> 3] |= (uint8_t)((bits[i] & 1) << (7 - (i & 7)));
    }
}

static int puncture(const uint8_t *in, int n, const uint8_t *pat, int plen, uint8_t *out)
{
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (pat[i % plen]) {
            out[k++] = in[i];
        }
    }
    return k;
}

static void depuncture(const uint16_t *in, int n_full, const uint8_t *pat, int plen, uint16_t *out)
{
    int k = 0;
    for (int i = 0; i < n_full; ++i) {
        out[i] = pat[i % plen] ? in[k++] : 32767;
    }
}

static inline uint8_t rand_bit(int i)
{
    return (m17_rand_seq[(i >> 3) % 46] >> (7 - (i & 7))) & 1;
}

void m17_rf_sync_symbols(uint16_t word, int8_t sym[8])
{
    for (int i = 0; i < 8; ++i) {
        sym[i] = SYM_OF_DIBIT[(word >> (14 - 2 * i)) & 3];
    }
}

/* type 2/3 bits (368) -> interleave -> randomize -> symbols after the sync burst */
static void finish_frame(uint16_t sync, const uint8_t *t3, int8_t sym[M17_SYMS_PER_FRAME])
{
    uint8_t t4[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_PAYLOAD_BITS; ++i) {
        t4[i] = t3[m17_interleave_index(i)] ^ rand_bit(i);
    }
    m17_rf_sync_symbols(sync, sym);
    for (int i = 0; i < M17_PAYLOAD_BITS / 2; ++i) {
        sym[8 + i] = SYM_OF_DIBIT[(t4[2 * i] << 1) | t4[2 * i + 1]];
    }
}

void m17_rf_preamble(int8_t sym[M17_SYMS_PER_FRAME])
{
    for (int i = 0; i < M17_SYMS_PER_FRAME; ++i) {
        sym[i] = (i & 1) ? -3 : +3;
    }
}

void m17_rf_eot(int8_t sym[M17_SYMS_PER_FRAME])
{
    for (int i = 0; i < M17_SYMS_PER_FRAME; i += 8) {
        m17_rf_sync_symbols(M17_EOT_WORD, sym + i);
    }
}

void m17_rf_lsf_frame(const uint8_t lsf[M17_LSF_BYTES], int8_t sym[M17_SYMS_PER_FRAME])
{
    uint8_t t1[240], t2[488], t3[M17_PAYLOAD_BITS];
    bits_from_bytes(lsf, 240, t1);
    m17_conv_encode(t1, 240, t2);
    puncture(t2, 488, m17_p1, 61, t3);
    finish_frame(M17_SYNC_LSF, t3, sym);
}

void m17_rf_stream_frame(const uint8_t lsf[M17_LSF_BYTES], uint16_t fn, bool last, const uint8_t payload[16],
                         int8_t sym[M17_SYMS_PER_FRAME])
{
    uint8_t t3[M17_PAYLOAD_BITS];
    /* LICH: 5 bytes of the LSF + counter, 4 Golay words */
    int cnt = fn % 6;
    uint8_t lich[6];
    memcpy(lich, lsf + 5 * cnt, 5);
    lich[5] = (uint8_t)(cnt << 5);
    uint16_t part[4] = {
        (uint16_t)((lich[0] << 4) | (lich[1] >> 4)), (uint16_t)(((lich[1] & 0x0F) << 8) | lich[2]),
        (uint16_t)((lich[3] << 4) | (lich[4] >> 4)), (uint16_t)(((lich[4] & 0x0F) << 8) | lich[5]),
    };
    for (int p = 0; p < 4; ++p) {
        uint32_t cw = m17_golay24_encode(part[p]);
        for (int b = 0; b < 24; ++b) {
            t3[24 * p + b] = (cw >> (23 - b)) & 1;
        }
    }
    /* FN + 128 bits */
    uint8_t c[18], t1[144], t2[296];
    uint16_t f = (uint16_t)((fn & 0x7FFF) | (last ? 0x8000 : 0));
    c[0] = (uint8_t)(f >> 8);
    c[1] = (uint8_t)f;
    memcpy(c + 2, payload, 16);
    bits_from_bytes(c, 144, t1);
    m17_conv_encode(t1, 144, t2);
    puncture(t2, 296, m17_p2, 12, t3 + 96);
    finish_frame(M17_SYNC_STREAM, t3, sym);
}

/* soft (after sync, randomized + interleaved) -> type 2/3 soft bits */
static void derand_deinterleave(const uint16_t *soft, uint16_t *t3)
{
    for (int i = 0; i < M17_PAYLOAD_BITS; ++i) {
        uint16_t s = rand_bit(i) ? (uint16_t)(65535u - soft[i]) : soft[i];
        t3[m17_interleave_index(i)] = s;
    }
}

bool m17_rf_decode_lsf(const uint16_t soft[M17_PAYLOAD_BITS], uint8_t lsf[M17_LSF_BYTES], m17_rx_info_t *info)
{
    uint16_t t3[M17_PAYLOAD_BITS], t2[488];
    uint8_t t1[240];
    derand_deinterleave(soft, t3);
    depuncture(t3, 488, m17_p1, 61, t2);
    uint32_t cost = m17_viterbi(t2, 488, t1, 240);
    bytes_from_bits(t1, 240, lsf);
    bool ok = m17_crc(lsf, M17_LSF_BYTES) == 0;
    if (info) {
        info->crc_ok = ok;
        info->viterbi_cost = cost;
    }
    return ok;
}

void m17_rf_decode_stream(const uint16_t soft[M17_PAYLOAD_BITS], m17_stream_rx_t *out)
{
    uint16_t t3[M17_PAYLOAD_BITS], t2[296];
    uint8_t t1[144], c[18];
    derand_deinterleave(soft, t3);
    /* LICH */
    uint16_t part[4];
    out->golay_errors = 0;
    bool lich_ok = true;
    for (int p = 0; p < 4; ++p) {
        uint32_t cw = 0;
        for (int b = 0; b < 24; ++b) {
            cw = (cw << 1) | (t3[24 * p + b] >= 32768 ? 1u : 0u);
        }
        int e = m17_golay24_decode(cw, &part[p]);
        if (e < 0) {
            lich_ok = false;
        } else {
            out->golay_errors += e;
        }
    }
    out->lich[0] = (uint8_t)(part[0] >> 4);
    out->lich[1] = (uint8_t)(((part[0] & 0x0F) << 4) | (part[1] >> 8));
    out->lich[2] = (uint8_t)part[1];
    out->lich[3] = (uint8_t)(part[2] >> 4);
    out->lich[4] = (uint8_t)(((part[2] & 0x0F) << 4) | (part[3] >> 8));
    out->lich[5] = (uint8_t)part[3];
    out->lich_cnt = lich_ok && (out->lich[5] >> 5) < 6 ? (out->lich[5] >> 5) : -1;
    /* FN + payload */
    depuncture(t3 + 96, 296, m17_p2, 12, t2);
    out->viterbi_cost = m17_viterbi(t2, 296, t1, 144);
    bytes_from_bits(t1, 144, c);
    uint16_t f = (uint16_t)((c[0] << 8) | c[1]);
    out->fn = f & 0x7FFF;
    out->last = (f & 0x8000) != 0;
    memcpy(out->payload, c + 2, 16);
}
