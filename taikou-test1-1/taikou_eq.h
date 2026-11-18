/* taikou_eq: a receive / transmit equalizer learned from a known M17 burst.
 * The IC-705 FM-D path (2026-11-18) delays the lows (5-12 ms below 150 Hz) and the 4-level eye closes even after the
 * magnitude-only precompensation. A least-squares FIR (taps every TEQ_STRIDE samples at 48 kHz) learned from the
 * known burst opened it: 99 of 100 frames without errors. */
#ifndef TAIKOU_EQ_H
#define TAIKOU_EQ_H

#include <stdbool.h>
#include <stdint.h>

#define TEQ_TAPS 161
#define TEQ_STRIDE 5

typedef struct {
    double w[TEQ_TAPS];
    float hist[TEQ_TAPS * TEQ_STRIDE];
    int hpos;
} teq_t;

typedef struct {
    double pos_s;          /* where the burst was found in the recording */
    double drift_ppm;      /* clock difference of the two sound cards */
    double nmse_gain_db;   /* error with a gain only (no equalizer) */
    double nmse_eq_db;     /* error with the equalizer */
    double nmse_half_db;   /* learned on the first half, measured on the second half */
    double peak;           /* correlation peak relative to the burst energy (0..1): is it really there? */
} teq_report_t;

/* rec: the demodulated baseband (48 kHz), ref: the burst as sent before any precompensation. false if not found. */
bool teq_train(const int16_t *rec, int nrec, const int16_t *ref, int nref, double *w, teq_report_t *rep);
/* the channel and the equalizer at a few frequencies, relative to 1 kHz (printed) */
void teq_print_response(const double *w);
bool teq_save(const char *path, const double *w, const char *note);
bool teq_load(const char *path, double *w);
void teq_init(teq_t *q, const double *w);
void teq_run(teq_t *q, int16_t *x, int n);          /* in place */
/* scale w to 0 dB at 1 kHz (for the transmitter: keep the deviation where the level settings put it) */
void teq_unity_1k(double *w);

#endif
