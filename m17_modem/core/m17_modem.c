#include "m17_modem.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RB_MASK 4095

void m17_rrc_taps(float h[M17_RRC_TAPS])
{
    const double a = 0.5;
    double e = 0;
    for (int i = 0; i < M17_RRC_TAPS; ++i) {
        double t = (i - (M17_RRC_TAPS - 1) / 2) / (double)M17_SPS;      /* in symbols */
        double v;
        if (fabs(t) < 1e-9) {
            v = 1.0 - a + 4.0 * a / M_PI;
        } else if (fabs(fabs(t) - 1.0 / (4.0 * a)) < 1e-9) {
            v = a / sqrt(2.0) * ((1 + 2 / M_PI) * sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * cos(M_PI / (4 * a)));
        } else {
            v = (sin(M_PI * t * (1 - a)) + 4 * a * t * cos(M_PI * t * (1 + a))) /
                (M_PI * t * (1 - (4 * a * t) * (4 * a * t)));
        }
        h[i] = (float)v;
        e += v * v;
    }
    for (int i = 0; i < M17_RRC_TAPS; ++i) {
        h[i] = (float)(h[i] / sqrt(e));                              /* unit energy */
    }
}

/* ------------------------------------------------------------------ modulator */
void m17_mod_init(m17_mod_t *m, float peak)
{
    memset(m, 0, sizeof(*m));
    m17_rrc_taps(m->h);
    float dc = 0;                                                    /* response to a long run of +1 symbols */
    for (int i = 0; i < M17_RRC_TAPS; i += M17_SPS) {
        dc += m->h[i];
    }
    m->gain = peak / (3.0f * dc);
}

int m17_mod(m17_mod_t *m, const int8_t *sym, int n, int16_t *out)
{
    int k = 0;
    for (int s = 0; s < n; ++s) {
        memmove(m->hist + 1, m->hist, sizeof(m->hist) - 1);
        m->hist[0] = sym[s];
        for (int j = 0; j < M17_SPS; ++j) {
            float acc = 0;
            for (int q = 0; q < 9; ++q) {
                int t = j + M17_SPS * q;
                if (t < M17_RRC_TAPS) {
                    acc += m->h[t] * m->hist[q];
                }
            }
            float v = acc * m->gain;
            out[k++] = (int16_t)(v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v));
        }
    }
    return k;
}

/* ------------------------------------------------------------------ demodulator */
static int8_t P_LSF[8], P_STR[8], P_PKT[8], P_BERT[8], P_EOT[8];

void m17_demod_init(m17_demod_t *d, m17_frame_cb cb, void *ctx)
{
    memset(d, 0, sizeof(*d));
    m17_rrc_taps(d->h);
    d->cb = cb;
    d->ctx = ctx;
    d->thr_search = 0.88f;
    d->thr_track = 0.55f;
    d->polarity = 1;
    d->min_gain = 100.0f;
    m17_rf_sync_symbols(M17_SYNC_LSF, P_LSF);
    m17_rf_sync_symbols(M17_SYNC_STREAM, P_STR);
    m17_rf_sync_symbols(M17_SYNC_PACKET, P_PKT);
    m17_rf_sync_symbols(M17_SYNC_BERT, P_BERT);
    m17_rf_sync_symbols(M17_EOT_WORD, P_EOT);
}

static inline float Y(const m17_demod_t *d, uint64_t i)
{
    return d->y[i & RB_MASK];
}

/* cubic (Catmull-Rom) value at sample base + frac, frac in -1..+1 */
static float Yf(const m17_demod_t *d, uint64_t base, float frac)
{
    float fl = floorf(frac);
    uint64_t i = base + (uint64_t)(int64_t)fl;
    float f = frac - fl;
    float a = Y(d, i - 1), b = Y(d, i), c = Y(d, i + 1), e = Y(d, i + 2);
    return b + 0.5f * f * (c - a + f * (2 * a - 5 * b + 4 * c - e + f * (3 * (b - c) + e - a)));
}

/* Correlation of the 8 symbols ending at sample "at" with pattern p, means removed (so DC does not matter).
 * Returns the normalized value (-1..1, sign = polarity); *cov gets the raw covariance (largest at the eye centre). */
static float corr(const m17_demod_t *d, uint64_t at, const int8_t *p, float *cov, float *gain, float *dc)
{
    float v[8], mv = 0, mp = 0;
    for (int k = 0; k < 8; ++k) {
        v[k] = Y(d, at - (uint64_t)(M17_SPS * (7 - k)));
        mv += v[k];
        mp += p[k];
    }
    mv /= 8;
    mp /= 8;
    float c = 0, ev = 0, ep = 0;
    for (int k = 0; k < 8; ++k) {
        float a = v[k] - mv, b = p[k] - mp;
        c += a * b;
        ev += a * a;
        ep += b * b;
    }
    if (cov) {
        *cov = c;
    }
    if (ev < 1e-6f || ep < 1e-6f) {
        return 0;
    }
    if (gain) {
        *gain = c / ep;
        *dc = mv - *gain * mp;
    }
    return c / sqrtf(ev * ep);
}

static const int8_t *pattern(m17_frame_t t)
{
    switch (t) {
    case M17_FR_LSF: return P_LSF;
    case M17_FR_STREAM: return P_STR;
    case M17_FR_PACKET: return P_PKT;
    case M17_FR_BERT: return P_BERT;
    default: return P_EOT;
    }
}

/* The stream sync is the LSF sync negated (and packet = BERT negated), so a burst found while searching is checked
 * by decoding it both ways (LSF: CRC; stream: LICH Golay words and Viterbi cost); the reading that decodes sets the
 * polarity. Bursts while tracking use the polarity learned. This returns the score (> 0) and the type for tracking. */
static float classify_track(const m17_demod_t *d, uint64_t at, m17_frame_t *type)
{
    float pol = (float)d->polarity;
    float cs = corr(d, at, P_STR, NULL, NULL, NULL) * pol;
    float ce = corr(d, at, P_EOT, NULL, NULL, NULL) * pol;
    float best = cs;
    *type = M17_FR_STREAM;
    if (ce > best) {
        best = ce;
        *type = M17_FR_EOT;
    }
    return best;
}

static inline float slice_level(float s)
{
    return s > 2 ? 3 : (s > 0 ? 1 : (s > -2 ? -1 : -3));
}

/* Reads the frame at sync_at as pattern p: sub-sample timing by the smallest EVM (-1..+1 sample, 1/8 steps), each
 * offset with its own least-squares gain and DC against the decided levels; fills info and the soft bits. */
static void read_frame(m17_demod_t *d, const int8_t *p, m17_sync_info_t *info, uint16_t soft[M17_PAYLOAD_BITS])
{
    float g0, dc0;
    info->at = d->sync_at;
    info->score = fabsf(corr(d, d->sync_at, p, NULL, &g0, &dc0));
    float y[M17_SYMS_PER_FRAME], yb[M17_SYMS_PER_FRAME];
    float best_e = 1e30f, bg = g0, bdc = dc0, bf = 0;
    for (int step = -8; step <= 8; ++step) {
        float f = step / 8.0f, g = g0, dc = dc0;
        if (fabsf(g) < 1e-6f) {
            break;
        }
        for (int k = 0; k < M17_SYMS_PER_FRAME; ++k) {
            y[k] = Yf(d, d->sync_at - (uint64_t)(M17_SPS * 7) + (uint64_t)(M17_SPS * k), f);
        }
        for (int pass = 0; pass < 2; ++pass) {
            float sx = 0, sy = 0, sxx = 0, sxy = 0, ig = 1.0f / g;
            for (int k = 0; k < M17_SYMS_PER_FRAME; ++k) {
                float lv = k < 8 ? p[k] : slice_level((y[k] - dc) * ig);
                sx += lv;
                sy += y[k];
                sxx += lv * lv;
                sxy += lv * y[k];
            }
            float nn = M17_SYMS_PER_FRAME, den = nn * sxx - sx * sx;
            if (den > 1e-3f) {
                float gg = (nn * sxy - sx * sy) / den;
                if (gg * g > 0) {                      /* never flips the polarity */
                    g = gg;
                    dc = (sy - gg * sx) / nn;
                }
            }
        }
        float e = 0, ig2 = 1.0f / g;
        for (int k = 8; k < M17_SYMS_PER_FRAME; ++k) {
            float sv = (y[k] - dc) * ig2, lv = slice_level(sv);
            e += (sv - lv) * (sv - lv);
        }
        if (e < best_e) {
            best_e = e;
            bg = g;
            bdc = dc;
            bf = f;
            memcpy(yb, y, sizeof(yb));
        }
    }
    info->gain = bg;
    info->dc = bdc;
    info->timing = bf;
    float evm = 0, ibg = fabsf(bg) > 1e-6f ? 1.0f / bg : 0;
    for (int k = 0; k < M17_PAYLOAD_BITS / 2; ++k) {
        float s = (yb[8 + k] - bdc) * ibg;
        float lv = slice_level(s);
        evm += (s - lv) * (s - lv);
        float msb = 0.5f - 0.5f * s;                   /* +1/+3 -> 0, -1/-3 -> 1 */
        float lsb = 0.5f + 0.5f * (fabsf(s) - 2.0f);   /* inner -> 0, outer -> 1 */
        msb = msb < 0 ? 0 : (msb > 1 ? 1 : msb);
        lsb = lsb < 0 ? 0 : (lsb > 1 ? 1 : lsb);
        soft[2 * k] = (uint16_t)(msb * 65535.0f);
        soft[2 * k + 1] = (uint16_t)(lsb * 65535.0f);
    }
    info->evm = sqrtf(evm / (M17_PAYLOAD_BITS / 2));
}

/* Hands the frame at sync_at to the callback. A burst found by searching must decode (as LSF or as stream frame);
 * false: it did not (noise), nothing was emitted. */
static bool emit_frame(m17_demod_t *d, bool from_search)
{
    m17_sync_info_t info;
    uint16_t soft[M17_PAYLOAD_BITS];
    memset(&info, 0, sizeof(info));
    info.at = d->sync_at;
    if (d->type == M17_FR_EOT) {
        info.type = M17_FR_EOT;
        info.score = fabsf(corr(d, d->sync_at, P_EOT, NULL, &info.gain, &info.dc));
        d->cb(d->ctx, &info, NULL);
        return true;
    }
    if (from_search) {
        uint8_t lsf[M17_LSF_BYTES];
        read_frame(d, P_LSF, &info, soft);
        if (m17_rf_decode_lsf(soft, lsf, NULL)) {
            d->type = info.type = M17_FR_LSF;
        } else {
            m17_stream_rx_t r;
            read_frame(d, P_STR, &info, soft);
            m17_rf_decode_stream(soft, &r);
            /* 24 punctured bits cost 12 units by themselves; random data costs well over 100 */
            if (r.lich_cnt < 0 || r.golay_errors > 4 || r.viterbi_cost > 40u * 65535u) {
                return false;
            }
            d->type = info.type = M17_FR_STREAM;
        }
        d->polarity = info.gain > 0 ? 1 : -1;
    } else {
        read_frame(d, pattern(d->type), &info, soft);
        info.type = d->type;
    }
    d->sync_pos = (double)d->sync_at + info.timing;
    d->cb(d->ctx, &info, soft);
    return true;
}

void m17_demod_push(m17_demod_t *d, const int16_t *x, int n)
{
    for (int i = 0; i < n; ++i) {
        d->x[d->xpos] = x[i];
        float acc = 0;
        int p = d->xpos;
        for (int t = 0; t < M17_RRC_TAPS; ++t) {
            acc += d->h[t] * d->x[p];
            p = p ? p - 1 : M17_RRC_TAPS - 1;
        }
        d->xpos = (d->xpos + 1) % M17_RRC_TAPS;
        uint64_t now = d->n++;
        d->y[now & RB_MASK] = acc;
        if (now < 200) {
            continue;
        }
        switch (d->state) {
        case 0: {                                      /* search any sync burst */
            if (now < d->holdoff) {
                break;
            }
            m17_frame_t t = M17_FR_LSF;                /* LSF or stream: decided by decoding */
            float cov;
            float s = fabsf(corr(d, now, P_LSF, &cov, NULL, NULL));
            cov = fabsf(cov);
            float g = 0, dc = 0;
            if (s > d->thr_search) {
                corr(d, now, P_LSF, NULL, &g, &dc);
                if (fabsf(corr(d, now, P_EOT, NULL, NULL, NULL)) >= s) {
                    s = 0;                             /* inside an EoT: not a frame */
                }
            }
            if (s > d->thr_search && fabsf(g) >= d->min_gain && fabsf(dc) < 3.0f * fabsf(g)) {
                d->state = 1;
                d->best = cov;
                d->best_at = now;
                d->type = t;
            }
            break;
        }
        case 1: {                                      /* follow the peak (largest covariance) for one symbol */
            float cov;
            float s = fabsf(corr(d, now, pattern(d->type), &cov, NULL, NULL));
            cov = fabsf(cov);
            if (s > d->thr_search && cov > d->best) {
                d->best = cov;
                d->best_at = now;
            }
            if (now >= d->best_at + M17_SPS / 2 + 1) {
                d->sync_at = d->best_at;
                d->state = 2;
                d->from_search = true;
            }
            break;
        }
        case 2:                                        /* wait for the 184 payload symbols */
            if (now >= d->sync_at + (uint64_t)(M17_SPS * (M17_SYMS_PER_FRAME - M17_SYNC_SYMS)) + 3) {   /* +3: interpolation */
                d->state = emit_frame(d, d->from_search) ? 3 : 0;
                d->from_search = false;
            }
            break;
        case 3: {                                      /* next burst 192 symbols on (predicted), type from +-1 */
            double pos = d->sync_pos + M17_SPS * M17_SYMS_PER_FRAME;
            uint64_t exp = (uint64_t)floor(pos + 0.5);
            if (now < exp + 4) {
                break;
            }
            float best = 0;
            m17_frame_t bt = M17_FR_STREAM;
            for (uint64_t c = exp - 1; c <= exp + 1; ++c) {
                m17_frame_t t = M17_FR_STREAM;
                float s = classify_track(d, c, &t);
                if (s > best) {
                    best = s;
                    bt = t;
                }
            }
            uint64_t at = exp;
            if (best > d->thr_track) {
                d->sync_at = at;
                d->type = bt;
                if (bt == M17_FR_EOT) {
                    emit_frame(d, false);
                    d->state = 0;
                    d->holdoff = now + (uint64_t)(M17_SPS * M17_SYMS_PER_FRAME);   /* rest of the EoT */
                } else {
                    d->state = 2;
                }
            } else {
                m17_sync_info_t info;
                memset(&info, 0, sizeof(info));
                info.type = M17_FR_LOST;
                info.at = exp;
                d->cb(d->ctx, &info, NULL);
                d->state = 0;
            }
            break;
        }
        }
    }
}
