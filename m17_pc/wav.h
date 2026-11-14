#pragma once
/* Minimal WAV writer: 8 kHz, 16 bit, mono (one file per received call) or stereo (interleaved L,R). */
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    uint32_t samples;                               /* all channels together */
    uint16_t channels;
} wav_t;

int wav_open(wav_t *w, const char *path);           /* 0 = ok; mono */
int wav_open_stereo(wav_t *w, const char *path);    /* 0 = ok; wav_write then takes L,R,L,R,... */
void wav_write(wav_t *w, const int16_t *pcm, uint32_t n);
void wav_close(wav_t *w);                           /* fills in the sizes */
