/* taikou_eq: see taikou_eq.h. */
#include "taikou_eq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000
#define K TEQ_TAPS
#define ST TEQ_STRIDE

/* ---------------------------------------------------------------- FFT (radix 2, in place) */
static void fft(double *re, double *im, int n, int inv)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j ^= b;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = 2 * M_PI / len * (inv ? 1 : -1), wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; ++k) {
                int p = i + k, q = i + k + len / 2;
                double xr = re[q] * cr - im[q] * ci, xi = re[q] * ci + im[q] * cr;
                re[q] = re[p] - xr; im[q] = im[p] - xi;
                re[p] += xr; im[p] += xi;
                double t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

/* coarse position of ref in rec: cross-correlation at 12 kHz (4-sample means) by FFT; returns the 48 kHz lag */
static int coarse_find(const int16_t *rec, int nrec, const int16_t *ref, int nref, double *peak_rel)
{
    int a = nrec / 4, b = nref / 4, n = 1;
    while (n < a + b) n <<= 1;
    double *xr = calloc((size_t)n, sizeof(double)), *xi = calloc((size_t)n, sizeof(double));
    double *yr = calloc((size_t)n, sizeof(double)), *yi = calloc((size_t)n, sizeof(double));
    double er = 0;
    for (int i = 0; i < a; ++i) xr[i] = (rec[4 * i] + rec[4 * i + 1] + rec[4 * i + 2] + rec[4 * i + 3]) * 0.25;
    for (int i = 0; i < b; ++i) {
        yr[i] = (ref[4 * i] + ref[4 * i + 1] + ref[4 * i + 2] + ref[4 * i + 3]) * 0.25;
        er += yr[i] * yr[i];
    }
    fft(xr, xi, n, 0);
    fft(yr, yi, n, 0);
    for (int i = 0; i < n; ++i) {                       /* X * conj(Y) */
        double r = xr[i] * yr[i] + xi[i] * yi[i], m = xi[i] * yr[i] - xr[i] * yi[i];
        xr[i] = r; xi[i] = m;
    }
    fft(xr, xi, n, 1);
    int best = 0;
    double bv = -1e300;
    for (int l = 0; l + b <= a; ++l) {
        if (xr[l] > bv) { bv = xr[l]; best = l; }
    }
    /* normalized: correlation / sqrt(E_ref * E_rec segment) */
    double es = 0;
    for (int i = 0; i < b; ++i) es += (double)(rec[4 * (best + i)]) * rec[4 * (best + i)];
    *peak_rel = bv / n / sqrt(er * es + 1e-9);
    free(xr); free(xi); free(yr); free(yi);
    return best * 4;
}

static double corr_at(const float *a, int na, const float *b, int nb, int a0, int len, double lag)
{
    double s = 0;
    for (int i = 0; i < len; ++i) {
        double t = a0 + i + lag;
        int j = (int)floor(t);
        double fr = t - j;
        if (j < 0 || j + 1 >= nb || a0 + i >= na) continue;
        s += a[a0 + i] * (b[j] * (1 - fr) + b[j + 1] * fr);
    }
    return s;
}

static double peak_lag(const float *a, int na, const float *b, int nb, int a0, int len, double c, int span)
{
    double best = -1e300, bl = c;
    for (int l = (int)c - span; l <= (int)c + span; ++l) {
        double v = corr_at(a, na, b, nb, a0, len, l);
        if (v > best) { best = v; bl = l; }
    }
    double ym = corr_at(a, na, b, nb, a0, len, bl - 1), yp = corr_at(a, na, b, nb, a0, len, bl + 1);
    double d = ym - 2 * best + yp;
    return d < 0 ? bl + 0.5 * (ym - yp) / d : bl;
}

static double cub(const float *x, int n, double t)
{
    int i = (int)floor(t);
    double f = t - i;
    if (i < 1 || i + 2 >= n) return 0;
    double a = x[i - 1], b = x[i], c = x[i + 1], d = x[i + 2];
    return b + 0.5 * f * (c - a + f * (2 * a - 5 * b + 4 * c - d + f * (3 * (b - c) + d - a)));
}

static void ls_fir(const float *x, const float *y, int n0, int n1, double *w)
{
    static double R[K][K + 1];
    memset(R, 0, sizeof(R));
    for (int n = n0; n < n1; n += 3) {
        double v[K];
        for (int k = 0; k < K; ++k) v[k] = x[n - ST * (k - K / 2)];
        for (int i = 0; i < K; ++i) {
            for (int j = i; j < K; ++j) R[i][j] += v[i] * v[j];
            R[i][K] += v[i] * y[n];
        }
    }
    double tr = 0;
    for (int i = 0; i < K; ++i) {
        for (int j = 0; j < i; ++j) R[i][j] = R[j][i];
        tr += R[i][i];
    }
    for (int i = 0; i < K; ++i) R[i][i] += 1e-4 * tr / K;            /* ridge: keeps the taps tame */
    for (int c = 0; c < K; ++c) {
        int p = c;
        for (int r = c + 1; r < K; ++r) if (fabs(R[r][c]) > fabs(R[p][c])) p = r;
        for (int j = 0; j <= K; ++j) { double t = R[c][j]; R[c][j] = R[p][j]; R[p][j] = t; }
        for (int r = 0; r < K; ++r) {
            if (r == c) continue;
            double f = R[r][c] / R[c][c];
            for (int j = c; j <= K; ++j) R[r][j] -= f * R[c][j];
        }
    }
    for (int i = 0; i < K; ++i) w[i] = R[i][K] / R[i][i];
}

static double nmse(const float *x, const float *y, int n0, int n1, const double *w)
{
    double e = 0, p = 0;
    for (int n = n0; n < n1; ++n) {
        double s = 0;
        for (int k = 0; k < K; ++k) s += w[k] * x[n - ST * (k - K / 2)];
        e += (y[n] - s) * (y[n] - s);
        p += (double)y[n] * y[n];
    }
    return e / (p + 1e-9);
}

bool teq_train(const int16_t *rec, int nrec, const int16_t *ref16, int nref, double *w, teq_report_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    if (nrec < nref + FS) {
        return false;
    }
    int c = coarse_find(rec, nrec, ref16, nref, &rep->peak);
    rep->pos_s = (double)c / FS;
    if (rep->peak < 0.2) {
        return false;                                  /* the burst is not in the recording */
    }
    float *ref = calloc((size_t)nref, sizeof(float)), *x = calloc((size_t)nrec, sizeof(float));
    for (int i = 0; i < nref; ++i) ref[i] = ref16[i];
    for (int i = 0; i < nrec; ++i) x[i] = rec[i];
    int len = FS / 2, a0 = FS / 10, b0 = nref - FS / 2 - FS / 10;
    double la = peak_lag(ref, nref, x, nrec, a0, len, c, 8);
    double lb = peak_lag(ref, nref, x, nrec, b0, len, la, 12);
    double drift = (lb - la) / (double)(b0 - a0);
    rep->drift_ppm = drift * 1e6;
    float *al = calloc((size_t)nref, sizeof(float));
    for (int i = 0; i < nref; ++i) al[i] = (float)cub(x, nrec, i + la + (i - a0) * drift);
    int n0 = ST * K, n1 = nref - ST * K;
    double xy = 0, xx = 0, e = 0, p = 0;
    for (int n = n0; n < n1; ++n) { xy += al[n] * ref[n]; xx += (double)al[n] * al[n]; }
    double g = xy / (xx + 1e-9);
    for (int n = n0; n < n1; ++n) { double d = ref[n] - g * al[n]; e += d * d; p += (double)ref[n] * ref[n]; }
    rep->nmse_gain_db = 10 * log10(e / p + 1e-12);
    double w2[K];
    int mid = (n0 + n1) / 2;
    ls_fir(al, ref, n0, mid, w2);
    rep->nmse_half_db = 10 * log10(nmse(al, ref, mid, n1, w2) + 1e-12);
    ls_fir(al, ref, n0, n1, w);
    rep->nmse_eq_db = 10 * log10(nmse(al, ref, n0, n1, w) + 1e-12);
    free(ref); free(x); free(al);
    return true;
}

static void resp(const double *w, double f, double *mag, double *gd_ms)
{
    double re = 0, im = 0, re2 = 0, im2 = 0, df = 5;
    for (int k = 0; k < K; ++k) {
        double t = (double)ST * (k - K / 2) / FS;
        re += w[k] * cos(2 * M_PI * f * t); im -= w[k] * sin(2 * M_PI * f * t);
        re2 += w[k] * cos(2 * M_PI * (f + df) * t); im2 -= w[k] * sin(2 * M_PI * (f + df) * t);
    }
    double dp = atan2(im2, re2) - atan2(im, re);
    while (dp > M_PI) dp -= 2 * M_PI;
    while (dp < -M_PI) dp += 2 * M_PI;
    *mag = hypot(re, im);
    *gd_ms = -dp / (2 * M_PI * df) * 1e3;
}

void teq_print_response(const double *w)
{
    static const double F[] = { 20, 30, 50, 70, 100, 150, 200, 300, 500, 1000, 2000, 2400, 2800, 3200 };
    double m1, g1;
    resp(w, 1000, &m1, &g1);
    printf("equalizer (what it adds; the radio's path is the opposite), relative to 1 kHz:\n");
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); ++i) {
        double m, g;
        resp(w, F[i], &m, &g);
        printf("  %5.0f Hz  %+6.1f dB  delay %+6.2f ms\n", F[i], 20 * log10(m / m1 + 1e-12), g - g1);
    }
}

void teq_unity_1k(double *w)
{
    double m, g;
    resp(w, 1000, &m, &g);
    for (int k = 0; k < K; ++k) w[k] /= m;
}

bool teq_save(const char *path, const double *w, const char *note)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# taikou-test1-1 equalizer: %d taps, every %d samples at 48 kHz (%s)\n", K, ST, note);
    for (int k = 0; k < K; ++k) fprintf(f, "%.9g\n", w[k]);
    fclose(f);
    return true;
}

bool teq_load(const char *path, double *w)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    int k = 0;
    while (k < K && fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        w[k++] = atof(line);
    }
    fclose(f);
    return k == K;
}

void teq_init(teq_t *q, const double *w)
{
    memset(q, 0, sizeof(*q));
    memcpy(q->w, w, sizeof(q->w));
}

/* y[n] = sum_k w_k x[n - ST*(k - K/2)]: non-causal by K/2*ST samples, so the output is delayed by that much */
void teq_run(teq_t *q, int16_t *x, int n)
{
    const int H = K * ST;
    for (int i = 0; i < n; ++i) {
        q->hist[q->hpos] = x[i];
        double s = 0;
        for (int k = 0; k < K; ++k) {
            /* delayed by D = ST*K/2: y[n-D] = sum_k w_k x[n-D - ST*(k-K/2)] = sum_k w_k x[n - ST*k] */
            int j = q->hpos - ST * k;
            if (j < 0) j += H;
            s += q->w[k] * q->hist[j];
        }
        q->hpos = (q->hpos + 1) % H;
        x[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : lrint(s));
    }
}
