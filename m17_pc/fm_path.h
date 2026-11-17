#pragma once
/* Signal paths of a radio without a 9600 bps data port, e.g. the IC-705 over USB (48 kHz).
 *
 * Receive: the radio's USB output set to "IF" carries the 12 kHz IF. fmp_ifdem_* mixes it to zero, low-passes it and
 * takes the phase difference: an FM discriminator that is flat down to DC, like a 9600 port. Its output is the
 * baseband in int16 units with +-2.4 kHz deviation (an M17 +3 symbol) = FMP_NOMINAL.
 *
 * Transmit: the radio's modulator input (USB MOD) goes through the radio's audio processing before the FM modulator:
 * a high-pass, pre-emphasis, a limiter and a low-pass. fmp_pre_* is the inverse of the linear part: a regularized
 * inverse high-pass (it restores the lows down to floor_hz) and de-emphasis, so that after the radio's filters the
 * deviation follows the M17 baseband again. The limiter is not undone: the level must stay below it.
 *
 * fmp_model_* is a model of such a radio for tests (no sound card): TX audio path -> FM -> 12 kHz IF (or the AF path
 * with de-emphasis and the voice-band filters). */
#include <stdbool.h>
#include <stdint.h>

#define FMP_FS         48000
#define FMP_IF_HZ      12000.0
#define FMP_DEV_HZ     2400.0             /* deviation of a +3 symbol */
#define FMP_NOMINAL    12000.0f           /* int16 units of a +3 symbol at the discriminator output */

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } fmp_bq_t;

/* the radio's TX audio path, as far as the precompensation and the model need it */
typedef struct {
    double hp_hz;          /* high-pass corner, 0 = none */
    int hp_order;          /* 1 or 2 (Butterworth) */
    double preemph_us;     /* pre-emphasis time constant (750 us = 212 Hz), 0 = none */
    double preemph_top_hz; /* where the pre-emphasis stops rising */
    double lp_hz;          /* low-pass corner (4th order Butterworth), 0 = none */
} fmp_radio_t;

void fmp_radio_ic705_guess(fmp_radio_t *r);   /* a guess until measured: HP 300 Hz 2nd order, no pre-emphasis (FM-D), LP 3 kHz */

/* ---- TX precompensation: in place, int16 */
typedef struct {
    fmp_bq_t q[3];
    int nq;
    double gain;           /* applied after the filters */
    unsigned clips;
} fmp_pre_t;

/* floor_hz: lowest frequency restored (e.g. 15 Hz). The output gets louder in the lows: gain scales it back. */
void fmp_pre_init(fmp_pre_t *p, const fmp_radio_t *r, double floor_hz, double gain);
extern double fmp_pre_eps_hz;   /* 2nd-order high-pass: leaves a 1st-order cut at floor_hz down to this (5); 0 = 2nd */
void fmp_pre_run(fmp_pre_t *p, int16_t *x, int n);
double fmp_pre_peak_gain(const fmp_radio_t *r, double floor_hz);   /* largest gain of the inverse filter (linear) */

/* ---- RX: 12 kHz IF -> discriminator */
typedef struct {
    double ph, dph;
    fmp_bq_t li[2], lq[2];
    double pi, pq;
    double dc;             /* slow average of the output: the frequency offset (Hz) */
} fmp_ifdem_t;

void fmp_ifdem_init(fmp_ifdem_t *d, double if_hz);
void fmp_ifdem_run(fmp_ifdem_t *d, const int16_t *in, int16_t *out, int n);
double fmp_ifdem_offset_hz(const fmp_ifdem_t *d);

/* ---- a radio model for tests */
typedef struct {
    fmp_radio_t r;
    double limit;          /* limiter, in units of a +3 symbol deviation (e.g. 1.4); 0 = none */
    bool rx_if;            /* true: output the 12 kHz IF; false: the AF path (de-emphasis, HP, LP) */
    double rx_offset_hz;   /* frequency error of the receiver */
    double in_scale;       /* TX input int16 -> deviation units: in * in_scale = 1 for a +3 symbol */
    fmp_bq_t tq[6];
    int ntq;
    fmp_bq_t rq[6];
    int nrq;
    double phase;
    unsigned limited;      /* samples the limiter touched */
} fmp_model_t;

void fmp_model_init(fmp_model_t *m, const fmp_radio_t *r, double limit, bool rx_if, double in_scale);
/* TX audio in -> what the receiving radio gives on USB (IF or AF), noise_rms in output int16 units.
 * other (may be NULL): another station on the channel, in deviation units (1 = +3 symbol), added on the air */
void fmp_model_run(fmp_model_t *m, const int16_t *in, const float *other, int16_t *out, int n, double noise_rms,
                   unsigned *seed);
