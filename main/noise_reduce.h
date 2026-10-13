#pragma once
/* Single-channel noise reduction for the encoder input (8 kHz, 160-sample frames).
 * STFT (320-sample sqrt-Hann window, 50 % overlap, 512-point FFT), per-bin noise tracking and a
 * decision-directed Wiener gain. Adds one frame (20 ms) of delay. Portable C, no ESP-IDF dependency. */
#include <stdbool.h>
#include <stdint.h>

#define NR_FRAME 160

void nr_init(void);                       /* builds tables; call once */
void nr_reset(void);                      /* forget the noise estimate (new stream) */
void nr_set_params(float floor_db, float over);   /* gain floor (e.g. -18 dB), noise over-estimation (1.0) */
void nr_set_freq_smooth(bool on);         /* smooth the gain across neighbouring bins (anti musical noise) */
void nr_set_auto_eq(bool on, float max_db); /* automatic equaliser towards the target voice balance */
void nr_eq_info(float out_db[15]);        /* current per-band correction (150..3800 Hz), dB */
void nr_set_eq_blend(float blend);        /* target: 0 = own natural voice .. 1 = other D-STAR stations */
void nr_process(int16_t frame[NR_FRAME]); /* in place; output is the previous frame's audio */
