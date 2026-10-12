#include "noise_reduce.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define WIN   320                 /* analysis window = 2 frames */
#define NFFT  512
#define NBIN  (NFFT / 2 + 1)

static float s_win[WIN];          /* sqrt-Hann: analysis x synthesis = Hann, sums to 1 at 50 % overlap */
static float s_cos[NFFT / 2], s_sin[NFFT / 2];
static uint16_t s_rev[NFFT];
static float s_in[WIN];           /* last two input frames */
static float s_tail[NR_FRAME];    /* overlap-add tail */
static float s_re[NFFT], s_im[NFFT];
static float s_noise[NBIN], s_psmooth[NBIN], s_prev_clean[NBIN];
static bool s_have_noise;
static float s_gmin = 0.0562f;    /* -25 dB ("strong": preferred in listening tests on a noisy PC mic) */
static float s_over = 1.5f;       /* noise over-estimation */
static float s_gain[NBIN];
static bool s_freq_smooth = true;       /* smooth the gain across bins (nr_set_freq_smooth) */

void nr_set_freq_smooth(bool on)
{
    s_freq_smooth = on;
}

static void fft(float *re, float *im, bool inverse)
{
    for (int i = 0; i < NFFT; ++i) {
        int j = s_rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= NFFT; len <<= 1) {
        int half = len >> 1, step = NFFT / len;
        for (int i = 0; i < NFFT; i += len) {
            for (int k = 0; k < half; ++k) {
                float wr = s_cos[k * step];
                float wi = inverse ? s_sin[k * step] : -s_sin[k * step];
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
        }
    }
}

void nr_init(void)
{
    for (int i = 0; i < WIN; ++i) {
        s_win[i] = sqrtf(0.5f - 0.5f * cosf(2.0f * (float)M_PI * ((float)i + 0.5f) / (float)WIN));
    }
    for (int k = 0; k < NFFT / 2; ++k) {
        s_cos[k] = cosf(2.0f * (float)M_PI * (float)k / (float)NFFT);
        s_sin[k] = sinf(2.0f * (float)M_PI * (float)k / (float)NFFT);
    }
    for (int i = 0; i < NFFT; ++i) {
        int r = 0;
        for (int b = 0; b < 9; ++b) {
            if (i & (1 << b)) r |= 1 << (8 - b);
        }
        s_rev[i] = (uint16_t)r;
    }
    nr_reset();
}

void nr_reset(void)
{
    memset(s_in, 0, sizeof(s_in));
    memset(s_tail, 0, sizeof(s_tail));
    memset(s_prev_clean, 0, sizeof(s_prev_clean));
    s_have_noise = false;
}

void nr_set_params(float floor_db, float over)
{
    s_gmin = powf(10.0f, floor_db / 20.0f);
    s_over = over;
}

void nr_process(int16_t frame[NR_FRAME])
{
    memmove(s_in, s_in + NR_FRAME, NR_FRAME * sizeof(float));
    for (int i = 0; i < NR_FRAME; ++i) {
        s_in[NR_FRAME + i] = (float)frame[i];
    }
    for (int i = 0; i < NFFT; ++i) {
        s_re[i] = i < WIN ? s_in[i] * s_win[i] : 0.0f;
        s_im[i] = 0.0f;
    }
    fft(s_re, s_im, false);

    for (int k = 0; k < NBIN; ++k) {
        float pw = s_re[k] * s_re[k] + s_im[k] * s_im[k];
        if (!s_have_noise) {
            /* Start from a LOW noise estimate (white noise of RMS ~50 -> 2500 x 160 per bin): learning it
             * from the first frame treated a stream that starts with speech/tone as noise and removed it. */
            s_noise[k] = pw < 4.0e5f ? pw : 4.0e5f;
            s_psmooth[k] = pw;
        }
        s_psmooth[k] = 0.8f * s_psmooth[k] + 0.2f * pw;
        if (s_psmooth[k] < s_noise[k]) {
            s_noise[k] = 0.9f * s_noise[k] + 0.1f * s_psmooth[k];   /* fall quickly */
        } else {
            s_noise[k] *= 1.01f;                                     /* rise slowly (+4 dB/s) */
        }
        float nk = s_noise[k] * s_over + 1e-6f;
        float gamma = pw / nk;
        float xi = 0.98f * s_prev_clean[k] / nk + 0.02f * (gamma > 1.0f ? gamma - 1.0f : 0.0f);
        float g = xi / (1.0f + xi);
        if (g < s_gmin) {
            g = s_gmin;
        }
        s_prev_clean[k] = g * g * pw;
        s_gain[k] = g;
    }
    s_have_noise = true;

    if (s_freq_smooth) {
        /* Smooth the gain across neighbouring bins: isolated surviving noise bins ("musical noise") would
         * otherwise become short tones that the encoder renders as false harmonics. */
        float prev = s_gain[0];
        for (int k = 1; k < NBIN - 1; ++k) {
            float cur = s_gain[k];
            s_gain[k] = 0.25f * prev + 0.5f * cur + 0.25f * s_gain[k + 1];
            prev = cur;
        }
    }
    for (int k = 0; k < NBIN; ++k) {
        s_re[k] *= s_gain[k];
        s_im[k] *= s_gain[k];
        if (k > 0 && k < NFFT / 2) {
            s_re[NFFT - k] = s_re[k];
            s_im[NFFT - k] = -s_im[k];
        }
    }

    fft(s_re, s_im, true);
    for (int i = 0; i < NR_FRAME; ++i) {
        float y = s_tail[i] + s_re[i] / (float)NFFT * s_win[i];
        s_tail[i] = s_re[NR_FRAME + i] / (float)NFFT * s_win[NR_FRAME + i];
        if (y > 32767.0f) y = 32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        frame[i] = (int16_t)y;
    }
}
