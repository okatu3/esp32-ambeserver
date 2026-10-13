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

/* ---------------------------------------------------------------- automatic equaliser
 * Learns the long-term spectral balance of the (noise-reduced) speech and nudges it towards a target
 * balance, so a dull PC microphone does not sound muffled on the air. Target = midpoint (in dB, per band)
 * between the operator's own voice recorded with a good microphone and the average of other D-STAR stations
 * (real AMBE chips) received on a reflector, lightly smoothed across bands. Overall level is preserved;
 * per-band correction limited to +/-s_eq_max dB and slewed at 2.5 dB/s. Speech frames only. */
#define EQ_BANDS 15
static const uint16_t EQ_EDGE[EQ_BANDS + 1] = {         /* FFT bins (15.625 Hz): 150 .. 3800 Hz */
    10, 16, 22, 29, 38, 48, 58, 70, 83, 102, 122, 141, 166, 192, 218, 243
};
/* Mean power per bin and band, dB re total, of: the operator's own voice (good microphone) and the average
 * of other D-STAR stations (real AMBE chips). The target is NAT + blend * (OTHERS - NAT). */
/* Both smoothed across bands (twice 1-2-1) so the target is a broad tilt, not one recording's formants. */
static const float EQ_NAT_DB[EQ_BANDS] = {
    -15.4f, -16.1f, -17.2f, -18.5f, -20.8f, -23.1f, -24.5f, -27.3f, -32.1f, -36.3f, -38.0f, -37.7f, -37.3f, -36.9f, -36.1f
};
static const float EQ_OTHERS_DB[EQ_BANDS] = {
    -16.2f, -16.3f, -16.9f, -18.9f, -22.0f, -24.8f, -26.1f, -26.4f, -26.6f, -27.2f, -28.0f, -28.6f, -29.1f, -29.8f, -30.1f
};
static float s_eq_blend = 1.0f;         /* 0 = own natural voice, 1 = other stations (chosen on Peanut) */
static bool s_eq_on = true;
static float s_eq_max = 8.0f;
static bool s_eq_dirty = true;          /* per-bin gains need recomputing */
static float s_eq_meas[EQ_BANDS];       /* long-term mean power per bin of the speech, per band */
static float s_eq_db[EQ_BANDS];         /* correction currently applied */
static uint32_t s_eq_frames;            /* speech frames seen */
static float s_eqg[NBIN];               /* per-bin linear gain */

void nr_set_auto_eq(bool on, float max_db)
{
    s_eq_on = on;
    s_eq_max = max_db;
}

void nr_set_eq_blend(float blend)
{
    s_eq_blend = blend;
}

static void auto_eq_update(void)
{
    float pc[EQ_BANDS], pn = 0.0f, ps = 0.0f;
    for (int b = 0; b < EQ_BANDS; ++b) {
        float acc = 0.0f;
        for (int k = EQ_EDGE[b]; k < EQ_EDGE[b + 1]; ++k) {
            float pw = s_re[k] * s_re[k] + s_im[k] * s_im[k];
            acc += s_gain[k] * s_gain[k] * pw;
            pn += s_noise[k];
        }
        pc[b] = acc / (float)(EQ_EDGE[b + 1] - EQ_EDGE[b]);
        ps += acc;
    }
    if (ps > 4.0f * pn * s_over) {                          /* clearly speech */
        /* Learn the microphone, not what is being said: plain mean over the first 5 s of speech (starting from
         * the first frame biased the result for minutes), then a slow average (~20 s). */
        for (int b = 0; b < EQ_BANDS; ++b) {
            if (s_eq_frames < 250) {
                s_eq_meas[b] = (s_eq_meas[b] * (float)s_eq_frames + pc[b]) / (float)(s_eq_frames + 1);
            } else {
                s_eq_meas[b] = 0.999f * s_eq_meas[b] + 0.001f * pc[b];
            }
        }
        s_eq_frames++;
    }
    if (s_eq_frames >= 250) {                               /* >= 5 s of speech learned */
        float want[EQ_BANDS], num = 0.0f, den = 0.0f;
        for (int b = 0; b < EQ_BANDS; ++b) {
            float target = EQ_NAT_DB[b] + s_eq_blend * (EQ_OTHERS_DB[b] - EQ_NAT_DB[b]);
            want[b] = target - 10.0f * log10f(s_eq_meas[b] + 1e-9f);
        }
        /* keep the overall speech power: choose the offset so sum(meas * 10^(want/10)) == sum(meas) */
        for (int b = 0; b < EQ_BANDS; ++b) {
            float bw = (float)(EQ_EDGE[b + 1] - EQ_EDGE[b]);
            num += s_eq_meas[b] * bw;
            den += s_eq_meas[b] * bw * powf(10.0f, want[b] / 10.0f);
        }
        float off = 10.0f * log10f(num / (den + 1e-12f));
        float sm[EQ_BANDS];
        for (int b = 0; b < EQ_BANDS; ++b) {
            float l = want[b > 0 ? b - 1 : b], c = want[b], r = want[b < EQ_BANDS - 1 ? b + 1 : b];
            sm[b] = 0.25f * l + 0.5f * c + 0.25f * r + off;  /* smooth across bands (no fine notches) */
            if (sm[b] > s_eq_max) sm[b] = s_eq_max;
            if (sm[b] < -s_eq_max) sm[b] = -s_eq_max;
            float d = sm[b] - s_eq_db[b];
            if (d > 0.05f) d = 0.05f;
            if (d < -0.05f) d = -0.05f;
            if (d != 0.0f) {
                s_eq_db[b] += d;
                s_eq_dirty = true;
            }
        }
    }
    if (!s_eq_dirty) {
        return;                                             /* gains unchanged: nothing to recompute */
    }
    s_eq_dirty = false;
    /* per-bin gain: band gains (15 powf) linearly interpolated between band centres */
    float lin[EQ_BANDS];
    for (int b = 0; b < EQ_BANDS; ++b) {
        lin[b] = powf(10.0f, s_eq_db[b] / 20.0f);
    }
    int b = 0;
    const float c0 = 0.5f * (EQ_EDGE[0] + EQ_EDGE[1]);
    const float cN = 0.5f * (EQ_EDGE[EQ_BANDS - 1] + EQ_EDGE[EQ_BANDS]);
    for (int k = 0; k < NBIN; ++k) {
        if ((float)k <= c0) {
            s_eqg[k] = lin[0];
        } else if ((float)k >= cN) {
            s_eqg[k] = lin[EQ_BANDS - 1];
        } else {
            while (b < EQ_BANDS - 2 && (float)k > 0.5f * (EQ_EDGE[b + 1] + EQ_EDGE[b + 2])) {
                b++;
            }
            float ca = 0.5f * (EQ_EDGE[b] + EQ_EDGE[b + 1]), cb = 0.5f * (EQ_EDGE[b + 1] + EQ_EDGE[b + 2]);
            float t = ((float)k - ca) / (cb - ca);
            s_eqg[k] = lin[b] + t * (lin[b + 1] - lin[b]);
        }
    }
}

void nr_eq_info(float out_db[15])
{
    memcpy(out_db, s_eq_db, sizeof(s_eq_db));
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
    if (s_eq_on) {
        auto_eq_update();
        for (int k = 0; k < NBIN; ++k) {
            s_gain[k] *= s_eqg[k];
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
