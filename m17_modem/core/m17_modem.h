#pragma once
/* M17 4FSK baseband at 48 kHz (10 samples per symbol), as fed to / taken from an FM radio's data port.
 * Modulator: symbols -> impulses -> root-raised-cosine (alpha 0.5, 81 taps) -> int16.
 * Demodulator: int16 -> matched RRC filter -> sync burst search (normalized correlation, any polarity and gain,
 * DC removed per burst) -> 184 payload symbols sampled at the sync timing -> soft bits -> callback per frame.
 * After a frame it looks for the next sync burst 192 symbols later (+-4 samples), so the timing follows the sender. */
#include <stdbool.h>
#include <stdint.h>

#include "m17_rf.h"

#define M17_FS          48000
#define M17_SPS         10
#define M17_RRC_TAPS    81

typedef struct {
    float h[M17_RRC_TAPS];
    int8_t hist[9];          /* last 9 symbols, newest first */
    float gain;
} m17_mod_t;

void m17_rrc_taps(float h[M17_RRC_TAPS]);
void m17_mod_init(m17_mod_t *m, float peak);                  /* peak: about the output for +3 symbols (int16) */
int m17_mod(m17_mod_t *m, const int8_t *sym, int n, int16_t *out);   /* writes n * 10 samples */

typedef enum { M17_FR_LSF, M17_FR_STREAM, M17_FR_PACKET, M17_FR_BERT, M17_FR_EOT, M17_FR_LOST } m17_frame_t;

typedef struct {
    m17_frame_t type;
    float score;             /* |correlation| of the sync burst, 1 = perfect */
    float gain;              /* signal per symbol unit (negative = inverted polarity) */
    float dc;
    float timing;            /* sub-sample fraction found for this frame (-0.5..0.5) */
    float evm;               /* RMS distance of the payload symbols from the nearest level, in symbol units */
    uint64_t at;             /* sample index of the last sync symbol */
} m17_sync_info_t;

/* soft is NULL for M17_FR_EOT / M17_FR_LOST */
typedef void (*m17_frame_cb)(void *ctx, const m17_sync_info_t *info, const uint16_t *soft);

typedef struct {
    float h[M17_RRC_TAPS];
    float x[M17_RRC_TAPS];   /* input history (ring) */
    int xpos;
    float y[4096];           /* filtered samples (ring) */
    uint64_t n;              /* samples pushed so far */
    int state;               /* 0 search, 1 peak, 2 collect, 3 track */
    m17_frame_t type;
    float best;
    uint64_t best_at, sync_at;
    uint64_t holdoff;        /* no search before this sample (rest of an EoT) */
    double sync_pos;         /* sync_at plus the sub-sample fraction */
    float thr_search, thr_track;
    float min_gain;          /* filtered signal per symbol unit needed to accept a burst (default 100) */
    bool from_search;        /* the burst being collected was found by searching (must decode) */
    int polarity;            /* +1 normal, -1 inverted: learned from the first frame that decodes */
    m17_frame_cb cb;
    void *ctx;
} m17_demod_t;

void m17_demod_init(m17_demod_t *d, m17_frame_cb cb, void *ctx);
void m17_demod_push(m17_demod_t *d, const int16_t *x, int n);
