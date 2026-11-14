#pragma once
/* M17 over the air (Part 1 of the M17 specification): frames of 192 symbols (8 sync + 184 payload) at 4800 sym/s.
 *   LSF:    240 bits (30-byte LSF incl. CRC) + 4 flush -> conv. code K=5 r=1/2 (488) -> P1 puncture (368)
 *   Stream: LICH 48 bits -> 4 x Golay(24,12) (96) | FN(16)+128 bits + 4 flush -> conv (296) -> P2 (272)
 *   then QPP interleave (out[i] = in[(45i + 92i^2) mod 368]), randomizer (46-byte table), sync burst in front.
 * Symbols are int8 in {+3, +1, -1, -3}; dibit 00 -> +1, 01 -> +3, 10 -> -1, 11 -> -3 (MSB first).
 * Soft bits are uint16: 0 = surely 0, 65535 = surely 1, 32767 = unknown. Pure C, no allocation. */
#include <stdbool.h>
#include <stdint.h>

#define M17_SYMS_PER_FRAME   192
#define M17_SYNC_SYMS        8
#define M17_PAYLOAD_BITS     368
#define M17_LSF_BYTES        30
#define M17_SYNC_LSF         0x55F7u
#define M17_SYNC_STREAM      0xFF5Du
#define M17_SYNC_PACKET      0x75FFu
#define M17_SYNC_BERT        0xDF55u
#define M17_EOT_WORD         0x555Du

void m17_rf_init(void);                                   /* builds the Golay table; call once */

/* ---- transmit: each fills 192 symbols */
void m17_rf_preamble(int8_t sym[M17_SYMS_PER_FRAME]);    /* +3,-3,... for an LSF (ends on -3) */
void m17_rf_eot(int8_t sym[M17_SYMS_PER_FRAME]);
void m17_rf_lsf_frame(const uint8_t lsf[M17_LSF_BYTES], int8_t sym[M17_SYMS_PER_FRAME]);
/* fn: frame number 0..0x7FFF (LICH counter = fn % 6), last: end-of-stream bit, payload: 16 bytes */
void m17_rf_stream_frame(const uint8_t lsf[M17_LSF_BYTES], uint16_t fn, bool last, const uint8_t payload[16],
                         int8_t sym[M17_SYMS_PER_FRAME]);
void m17_rf_sync_symbols(uint16_t word, int8_t sym[8]);

/* ---- receive: soft bits of the 368 payload bits that follow a sync burst (still randomized and interleaved) */
typedef struct {
    bool crc_ok;                 /* LSF: CRC of the 30 bytes */
    uint32_t viterbi_cost;       /* path metric, 0 = clean (scale: 65535 per wrong bit) */
} m17_rx_info_t;

bool m17_rf_decode_lsf(const uint16_t soft[M17_PAYLOAD_BITS], uint8_t lsf[M17_LSF_BYTES], m17_rx_info_t *info);

typedef struct {
    uint16_t fn;                 /* without the last bit */
    bool last;
    uint8_t payload[16];
    uint8_t lich[6];             /* 40-bit LSF chunk + counter byte */
    int lich_cnt;                /* 0..5, or -1 if a Golay word was beyond repair */
    int golay_errors;            /* corrected bits in the 4 Golay words */
    uint32_t viterbi_cost;
} m17_stream_rx_t;

void m17_rf_decode_stream(const uint16_t soft[M17_PAYLOAD_BITS], m17_stream_rx_t *out);

/* ---- building blocks (exposed for the tests) */
uint32_t m17_golay24_encode(uint16_t data12);
int m17_golay24_decode(uint32_t cw, uint16_t *data12);    /* corrected bits (0..3) or -1 */
int m17_conv_encode(const uint8_t *bits, int n, uint8_t *out);   /* n data bits + 4 flush -> 2(n+4) */
uint32_t m17_viterbi(const uint16_t *soft, int n_out_bits, uint8_t *bits, int n_data);   /* depunctured soft */
extern const uint8_t m17_rand_seq[46];
extern const uint8_t m17_p1[61], m17_p2[12];
uint16_t m17_interleave_index(int i);
