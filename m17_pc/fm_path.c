#include "fm_path.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- biquads from analog prototypes (bilinear) */
/* H(s) = (B0 s^2 + B1 s + B2) / (A0 s^2 + A1 s + A2), K = 2 fs or prewarped */
static void bq_analog(fmp_bq_t *q, double B0, double B1, double B2, double A0, double A1, double A2, double K)
{
    double K2 = K * K;
    double D = A0 * K2 + A1 * K + A2;
    q->b0 = (B0 * K2 + B1 * K + B2) / D;
    q->b1 = (2 * B2 - 2 * B0 * K2) / D;
    q->b2 = (B0 * K2 - B1 * K + B2) / D;
    q->a1 = (2 * A2 - 2 * A0 * K2) / D;
    q->a2 = (A0 * K2 - A1 * K + A2) / D;
    q->z1 = q->z2 = 0;
}

static double kwarp(double hz)            /* bilinear constant that keeps the corner at hz exact */
{
    double w = 2 * M_PI * hz;
    return w / tan(w / (2.0 * FMP_FS));
}

static inline double bq_run(fmp_bq_t *q, double x)
{
    double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static const double Q2 = 0.70710678118654752;          /* 2nd order Butterworth */
static const double Q4[2] = { 0.54119610014619698, 1.3065629648763766 };   /* 4th order Butterworth sections */

static int add_hp(fmp_bq_t *q, double hz, int order)
{
    double w = 2 * M_PI * hz, K = kwarp(hz);
    if (order <= 1) {
        bq_analog(q, 0, 1, 0, 0, 1, w, K);
    } else {
        bq_analog(q, 1, 0, 0, 1, w / Q2, w * w, K);
    }
    return 1;
}

static int add_lp4(fmp_bq_t *q, double hz)
{
    double w = 2 * M_PI * hz, K = kwarp(hz);
    for (int i = 0; i < 2; ++i) {
        bq_analog(q + i, 0, 0, w * w, 1, w / Q4[i], w * w, K);
    }
    return 2;
}

/* pre-emphasis (1 + s t1) / (1 + s t2); inverse: swapped */
static int add_emph(fmp_bq_t *q, const fmp_radio_t *r, bool inverse)
{
    double t1 = r->preemph_us * 1e-6, t2 = 1.0 / (2 * M_PI * (r->preemph_top_hz > 0 ? r->preemph_top_hz : 6000.0));
    if (inverse) {
        bq_analog(q, 0, t2, 1, 0, t1, 1, 2.0 * FMP_FS);
    } else {
        bq_analog(q, 0, t1, 1, 0, t2, 1, 2.0 * FMP_FS);
    }
    return 1;
}

/* inverse of the high-pass, regularized: what is left is a gentle high-pass at floor_hz.
 * 2nd order: (s^2 + w/Q s + w^2) / ((s + e)(s + f)) leaves s^2 / ((s + e)(s + f)): first order at floor_hz down to
 * e (FMP_PRE_EPS_HZ), which demodulators take much better than a second-order cut at the same corner. */
double fmp_pre_eps_hz = 5.0;

static int add_inv_hp(fmp_bq_t *q, double hz, int order, double floor_hz)
{
    double w = 2 * M_PI * hz, f = 2 * M_PI * floor_hz, e = 2 * M_PI * fmp_pre_eps_hz, K = kwarp(hz);
    if (order <= 1) {
        bq_analog(q, 0, 1, w, 0, 1, f, K);
    } else if (fmp_pre_eps_hz <= 0) {
        bq_analog(q, 1, w / Q2, w * w, 1, f / Q2, f * f, K);
    } else {
        bq_analog(q, 1, w / Q2, w * w, 1, e + f, e * f, K);
    }
    return 1;
}

void fmp_radio_ic705_guess(fmp_radio_t *r)
{
    memset(r, 0, sizeof(*r));
    r->hp_hz = 300;
    r->hp_order = 2;
    r->preemph_us = 0;                    /* FM-D (data mode): no pre-emphasis, as far as we know */
    r->preemph_top_hz = 6000;
    r->lp_hz = 3000;
}

/* ---------------------------------------------------------------- TX precompensation */
void fmp_pre_init(fmp_pre_t *p, const fmp_radio_t *r, double floor_hz, double gain)
{
    memset(p, 0, sizeof(*p));
    if (r->hp_hz > 0) {
        p->nq += add_inv_hp(p->q + p->nq, r->hp_hz, r->hp_order, floor_hz > 0 ? floor_hz : 15);
    }
    if (r->preemph_us > 0) {
        p->nq += add_emph(p->q + p->nq, r, true);
    }
    p->gain = gain;
}

void fmp_pre_run(fmp_pre_t *p, int16_t *x, int n)
{
    for (int i = 0; i < n; ++i) {
        double v = x[i];
        for (int k = 0; k < p->nq; ++k) {
            v = bq_run(&p->q[k], v);
        }
        v *= p->gain;
        if (v > 32767 || v < -32768) {
            p->clips++;
            v = v > 0 ? 32767 : -32768;
        }
        x[i] = (int16_t)lrint(v);
    }
}

double fmp_pre_peak_gain(const fmp_radio_t *r, double floor_hz)
{
    double g = 1;
    if (r->hp_hz > 0 && floor_hz > 0 && floor_hz < r->hp_hz) {
        g = pow(r->hp_hz / floor_hz, r->hp_order <= 1 ? 1 : 2);
    }
    return g;   /* the de-emphasis does not add gain (it is <= 1 above DC) */
}

/* ---------------------------------------------------------------- IF discriminator */
void fmp_ifdem_init(fmp_ifdem_t *d, double if_hz)
{
    memset(d, 0, sizeof(*d));
    d->dph = 2 * M_PI * if_hz / FMP_FS;
    add_lp4(d->li, 7000);
    add_lp4(d->lq, 7000);
    d->pi = 1;
}

void fmp_ifdem_run(fmp_ifdem_t *d, const int16_t *in, int16_t *out, int n)
{
    const double scale = FMP_FS / (2 * M_PI) / FMP_DEV_HZ * FMP_NOMINAL;
    for (int k = 0; k < n; ++k) {
        double x = in[k];
        double i = bq_run(&d->li[1], bq_run(&d->li[0], x * cos(d->ph)));
        double q = bq_run(&d->lq[1], bq_run(&d->lq[0], -x * sin(d->ph)));
        d->ph += d->dph;
        if (d->ph > 2 * M_PI) {
            d->ph -= 2 * M_PI;
        }
        double ph = atan2(q * d->pi - i * d->pq, i * d->pi + q * d->pq);
        d->pi = i;
        d->pq = q;
        double v = ph * scale;
        d->dc += (v - d->dc) * 0.0002;
        out[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : lrint(v)));
    }
}

double fmp_ifdem_offset_hz(const fmp_ifdem_t *d)
{
    return d->dc / FMP_NOMINAL * FMP_DEV_HZ;
}

/* ---------------------------------------------------------------- radio model */
void fmp_model_init(fmp_model_t *m, const fmp_radio_t *r, double limit, bool rx_if, double in_scale)
{
    memset(m, 0, sizeof(*m));
    m->r = *r;
    m->limit = limit;
    m->rx_if = rx_if;
    m->in_scale = in_scale;
    if (r->hp_hz > 0) {
        m->ntq += add_hp(m->tq + m->ntq, r->hp_hz, r->hp_order);
    }
    if (r->preemph_us > 0) {
        m->ntq += add_emph(m->tq + m->ntq, r, false);
    }
    /* the low-pass (splatter filter) comes after the limiter: kept apart in tq[4..5] */
    if (r->lp_hz > 0) {
        add_lp4(m->tq + 4, r->lp_hz);
    }
    if (!rx_if) {                        /* receiver AF path: de-emphasis, voice band */
        if (r->preemph_us > 0) {
            m->nrq += add_emph(m->rq + m->nrq, r, true);
        }
        if (r->hp_hz > 0) {
            m->nrq += add_hp(m->rq + m->nrq, r->hp_hz, r->hp_order);
        }
        if (r->lp_hz > 0) {
            m->nrq += add_lp4(m->rq + m->nrq, r->lp_hz);
        }
    }
}

static double gauss(unsigned *seed)
{
    double u1, u2;
    *seed = *seed * 1664525u + 1013904223u;
    u1 = ((*seed >> 8) + 1.0) / 16777217.0;
    *seed = *seed * 1664525u + 1013904223u;
    u2 = ((*seed >> 8) + 1.0) / 16777217.0;
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

void fmp_model_run(fmp_model_t *m, const int16_t *in, const float *other, int16_t *out, int n, double noise_rms,
                   unsigned *seed)
{
    for (int k = 0; k < n; ++k) {
        double v = in[k] * m->in_scale;               /* deviation units: 1 = +3 symbol */
        for (int i = 0; i < m->ntq; ++i) {
            v = bq_run(&m->tq[i], v);
        }
        if (m->limit > 0 && fabs(v) > m->limit) {
            v = v > 0 ? m->limit : -m->limit;
            m->limited++;
        }
        if (m->r.lp_hz > 0) {
            v = bq_run(&m->tq[5], bq_run(&m->tq[4], v));
        }
        if (other) {
            v += other[k];
        }
        double y;
        if (m->rx_if) {
            m->phase += 2 * M_PI * (FMP_IF_HZ + m->rx_offset_hz + v * FMP_DEV_HZ) / FMP_FS;
            if (m->phase > 2 * M_PI) {
                m->phase -= 2 * M_PI;
            }
            y = 8000.0 * cos(m->phase) + noise_rms * gauss(seed);
        } else {
            y = (v + m->rx_offset_hz / FMP_DEV_HZ) * FMP_NOMINAL + noise_rms * gauss(seed);
            for (int i = 0; i < m->nrq; ++i) {
                y = bq_run(&m->rq[i], y);
            }
        }
        out[k] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : lrint(y)));
    }
}
