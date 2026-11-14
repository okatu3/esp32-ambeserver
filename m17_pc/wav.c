#include "wav.h"

#include <string.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void header(uint8_t h[44], uint32_t samples, uint16_t channels)
{
    uint32_t data = samples * 2;
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);                 /* PCM */
    put16(h + 22, channels);
    put32(h + 24, 8000);
    put32(h + 28, 16000u * channels);
    put16(h + 32, (uint16_t)(2 * channels));
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
}

static int open_ch(wav_t *w, const char *path, uint16_t channels)
{
    uint8_t h[44];
    w->samples = 0;
    w->channels = channels;
    w->f = fopen(path, "wb");
    if (!w->f) {
        return -1;
    }
    header(h, 0, channels);
    fwrite(h, 1, sizeof(h), w->f);
    return 0;
}

int wav_open(wav_t *w, const char *path)
{
    return open_ch(w, path, 1);
}

int wav_open_stereo(wav_t *w, const char *path)
{
    return open_ch(w, path, 2);
}

void wav_write(wav_t *w, const int16_t *pcm, uint32_t n)
{
    if (!w->f) {
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {          /* little endian on disk, whatever the host is */
        uint8_t b[2];
        put16(b, (uint16_t)pcm[i]);
        fwrite(b, 1, 2, w->f);
    }
    w->samples += n;
}

void wav_close(wav_t *w)
{
    if (!w->f) {
        return;
    }
    uint8_t h[44];
    header(h, w->samples, w->channels ? w->channels : 1);
    fseek(w->f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), w->f);
    fclose(w->f);
    w->f = NULL;
}
