#include "radio_win.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m17.h"
#include "m17_modem.h"

#define BUF       960                      /* 20 ms at 48 kHz */
#define NIN       16
#define NOUT      8                        /* 160 ms handed to the sound card ahead */
#define FIFO_LEN  (48000 * 8)              /* modulated samples waiting to be played */
#define EVN       64
#define INJ_LEN   (48000 * 40)             /* loopback tests: another station's signal waiting to be mixed in */

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;

struct radio {
    radio_cfg_t c;
    radio_log_fn log;
    bool loop;
    HWAVEIN wi;
    WAVEHDR ih[NIN];
    int16_t ib[NIN][BUF];
    int inext;
    HWAVEOUT wo;
    WAVEHDR oh[NOUT];
    int16_t ob[NOUT][BUF];
    bool obusy[NOUT];
    int16_t *fifo;
    unsigned fhead, fcount;
    HANDLE com;
    m17_mod_t mod;
    m17_demod_t dem;
    bool keyed, ending;
    double tail_until;
    double t0;                             /* loopback: real-time pacing */
    uint64_t loop_done;
    uint32_t rnd;
    int16_t *inj;                          /* loopback: the other station */
    unsigned ihead, icount;
    m17_mod_t imod;
    biquad_t tx_bq[3], rx_bq[3];           /* loopback voice band: HP 300, LP 3000 (x2) */
    fmp_pre_t pre;                         /* tx_pre */
    fmp_ifdem_t ifd;                       /* rx_if */
    fmp_model_t model;                     /* loop_radio */
    teq_t *txq, *rxq;                      /* tx_eq, rx_eq (NULL: none) */
    double txw[TEQ_MAX], rxw[TEQ_MAX];
    int ntx, nrx;
    double tx_hp_b[3], tx_hp_a[3], tx_hp_s[4];   /* tx_eq_hp_hz: x1 x2 y1 y2 */
    double rx_dc;                          /* rx_if: slow DC block (the radios are ~1 kHz apart) */
    unsigned seed;
    /* receive */
    bool in_call, have_lsf;
    uint8_t lsf[M17_LSF_BYTES], lich[M17_LSF_BYTES];
    uint8_t lsf_tx[M17_LSF_BYTES];
    unsigned lich_mask;
    radio_evt_t ev[EVN];
    int nev;
    radio_stats_t st;
};

static double now_ms(void)
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

void radio_default_cfg(radio_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->in_device = c->out_device = RADIO_DEV_DEFAULT;
    c->ptt = RP_NONE;
    c->civ_addr = 0xA4;
    c->civ_baud = 115200;
    c->tx_delay_ms = 150;
    c->tx_tail_ms = 50;
    c->tx_dbfs = -10.0f;
    fmp_radio_ic705_guess(&c->pre_radio);
    c->pre_floor_hz = 50.0f;
    c->pre_gain_db = -36.0f;
    c->loop_limit = 1.5f;
    c->tx_eq_hp_hz = 25.0f;
    c->tx_eq_gain_db = -2.0f;
}

void radio_cfg_ic705(radio_cfg_t *c)
{
    if (c->ptt_port[0]) {                 /* the port comes from --ptt COMx:civ or the settings */
        c->ptt = RP_CIV;
    }
    c->civ_addr = 0xA4;
    c->civ_baud = 115200;
    c->rx_if = true;
    c->tx_pre = true;
    /* measured with taikou-test1-1 on 2026-11-18 (two IC-705s, FM-D, USB MOD Level 50 %) */
    c->pre_radio.hp_hz = 110;
    c->pre_radio.hp_order = 2;
    c->pre_radio.preemph_us = 5000;
    c->pre_gain_db = 30.0f;
    c->tx_dbfs = -47.5f;
    if (c->tx_delay_ms < 200) {
        c->tx_delay_ms = 200;
    }
}

/* ---------------------------------------------------------------- filters for the loopback test (RBJ biquads) */
static void bq_set(biquad_t *q, bool high, double fc, double qv)
{
    double w = 2 * M_PI * fc / 48000.0, cs = cos(w), al = sin(w) / (2 * qv), a0 = 1 + al;
    double b0 = high ? (1 + cs) / 2 : (1 - cs) / 2, b1 = high ? -(1 + cs) : 1 - cs;
    memset(q, 0, sizeof(*q));
    q->b0 = (float)(b0 / a0);
    q->b1 = (float)(b1 / a0);
    q->b2 = (float)(b0 / a0);
    q->a1 = (float)(-2 * cs / a0);
    q->a2 = (float)((1 - al) / a0);
}

static float bq_run(biquad_t *q, float x)
{
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static float gauss(radio_t *r)
{
    float s = 0;
    for (int i = 0; i < 12; ++i) {
        r->rnd = r->rnd * 1664525u + 1013904223u;
        s += (float)(r->rnd >> 8) / 16777216.0f;
    }
    return s - 6.0f;
}

/* ---------------------------------------------------------------- PTT */
static void ptt_set(radio_t *r, bool on)
{
    if (r->com == INVALID_HANDLE_VALUE) {
        return;
    }
    if (r->c.ptt == RP_RTS) {
        EscapeCommFunction(r->com, on ? SETRTS : CLRRTS);
    } else if (r->c.ptt == RP_DTR) {
        EscapeCommFunction(r->com, on ? SETDTR : CLRDTR);
    } else if (r->c.ptt == RP_CIV) {
        uint8_t m[8] = { 0xFE, 0xFE, (uint8_t)r->c.civ_addr, 0xE0, 0x1C, 0x00, (uint8_t)(on ? 1 : 0), 0xFD };
        DWORD w;
        PurgeComm(r->com, PURGE_RXCLEAR);
        WriteFile(r->com, m, sizeof(m), &w, NULL);
    }
}

static bool ptt_open(radio_t *r)
{
    r->com = INVALID_HANDLE_VALUE;
    if (r->c.ptt == RP_NONE) {
        return true;
    }
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\%s", r->c.ptt_port);
    r->com = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (r->com == INVALID_HANDLE_VALUE) {
        r->log("[E] cannot open %s for the PTT", r->c.ptt_port);
        return false;
    }
    DCB d;
    memset(&d, 0, sizeof(d));
    d.DCBlength = sizeof(d);
    GetCommState(r->com, &d);
    d.BaudRate = r->c.ptt == RP_CIV ? (r->c.civ_baud ? r->c.civ_baud : 19200) : 9600;
    d.ByteSize = 8;
    d.Parity = NOPARITY;
    d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE;
    d.fOutxCtsFlow = d.fOutxDsrFlow = FALSE;
    d.fDtrControl = DTR_CONTROL_DISABLE;
    d.fRtsControl = RTS_CONTROL_DISABLE;
    d.fOutX = d.fInX = FALSE;
    SetCommState(r->com, &d);
    COMMTIMEOUTS to = { MAXDWORD, 0, 0, 0, 200 };
    SetCommTimeouts(r->com, &to);
    ptt_set(r, false);
    static const char *names[] = { "none", "RTS", "DTR", "CI-V" };
    if (r->c.ptt == RP_CIV) {
        r->log("PTT: CI-V on %s, %u bps, address %02Xh", r->c.ptt_port, d.BaudRate, r->c.civ_addr);
    } else {
        r->log("PTT: %s of %s", names[r->c.ptt], r->c.ptt_port);
    }
    return true;
}

/* ---------------------------------------------------------------- receive */
static void emit(radio_t *r, const radio_evt_t *e)
{
    if (r->nev < EVN) {
        r->ev[r->nev++] = *e;
    }
}

static void end_call(radio_t *r, bool lost)
{
    if (r->in_call) {
        radio_evt_t e;
        memset(&e, 0, sizeof(e));
        e.kind = RE_END;
        e.signal_lost = lost;
        emit(r, &e);
    }
    r->in_call = r->have_lsf = false;
    r->lich_mask = 0;
}

static bool lsf_crc_ok(const uint8_t *l)
{
    return m17_crc(l, 28) == (uint16_t)(l[28] << 8 | l[29]);
}

static void on_frame(void *ctx, const m17_sync_info_t *info, const uint16_t *soft)
{
    radio_t *r = ctx;
    radio_evt_t e;
    memset(&e, 0, sizeof(e));
    e.evm = info->evm;
    e.gain = info->gain;
    if (info->type == M17_FR_EOT || info->type == M17_FR_LOST) {
        end_call(r, info->type == M17_FR_LOST);
        return;
    }
    if (!soft) {
        return;
    }
    if (info->type == M17_FR_LSF) {
        uint8_t l[M17_LSF_BYTES];
        m17_rx_info_t ri;
        if (m17_rf_decode_lsf(soft, l, &ri)) {
            if (r->in_call) {
                end_call(r, false);
            }
            r->in_call = r->have_lsf = true;
            memcpy(r->lsf, l, sizeof(l));
            e.kind = RE_LSF;
            e.have_lsf = true;
            memcpy(e.lsf, l, sizeof(l));
            emit(r, &e);
        }
        return;
    }
    if (info->type != M17_FR_STREAM) {
        return;
    }
    e.kind = RE_STREAM;
    m17_rf_decode_stream(soft, &e.s);
    r->st.frames_rx++;
    if (!r->in_call) {                    /* joined late: rebuild the LSF from the LICH chunks */
        r->in_call = true;
        r->have_lsf = false;
        r->lich_mask = 0;
    }
    if (!r->have_lsf && e.s.lich_cnt >= 0) {
        memcpy(r->lich + 5 * e.s.lich_cnt, e.s.lich, 5);
        r->lich_mask |= 1u << e.s.lich_cnt;
        if (r->lich_mask == 0x3F) {
            if (lsf_crc_ok(r->lich)) {
                memcpy(r->lsf, r->lich, sizeof(r->lsf));
                r->have_lsf = true;
                radio_evt_t l = e;
                l.kind = RE_LSF;
                l.have_lsf = l.lsf_from_lich = true;
                memcpy(l.lsf, r->lsf, sizeof(l.lsf));
                emit(r, &l);
            } else {
                r->lich_mask = 0;
            }
        }
    }
    e.have_lsf = r->have_lsf;
    memcpy(e.lsf, r->lsf, sizeof(e.lsf));
    emit(r, &e);
}

static void demod_push(radio_t *r, const int16_t *x, int n)
{
    float e = 0;
    for (int i = 0; i < n; ++i) {
        e += (float)x[i] * (float)x[i];
    }
    e /= (float)n;
    float db = e < 1 ? -99.0f : 10.0f * log10f(e / (32768.0f * 32768.0f));
    r->st.in_dbfs = r->st.in_dbfs < -98 ? db : 0.95f * r->st.in_dbfs + 0.05f * db;
    if (r->c.rx_if) {                     /* 12 kHz IF -> FM discriminator -> baseband */
        int16_t t[480];
        while (n > 0) {
            int k = n > 480 ? 480 : n;
            fmp_ifdem_run(&r->ifd, x, t, k);
            /* slow DC block (tau 0.1 s): two radios ~1 kHz apart made the demodulator refuse syncs when the
             * deviation was low (DC over 3x the signal) */
            for (int i = 0; i < k; ++i) {
                r->rx_dc += (t[i] - r->rx_dc) / 4800.0;
                double v = t[i] - r->rx_dc;
                t[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            if (r->rxq) {
                teq_run(r->rxq, t, k);
            }
            m17_demod_push(&r->dem, t, k);
            x += k;
            n -= k;
        }
        r->st.if_offset_hz = (float)fmp_ifdem_offset_hz(&r->ifd);
        return;
    }
    m17_demod_push(&r->dem, x, n);
}

/* ---------------------------------------------------------------- transmit */
static void fifo_put(radio_t *r, const int16_t *x, unsigned n)
{
    int16_t t[480];
    while (n > 0) {
        unsigned k = n > 480 ? 480 : n;
        memcpy(t, x, k * sizeof(int16_t));
        if (r->c.tx_pre) {
            fmp_pre_run(&r->pre, t, (int)k);
            r->st.pre_clips = r->pre.clips;
        }
        if (r->txq) {
            /* the precompensation ran 20 dB low (no clipping there); high-pass, equalizer, then the 20 dB back
             * plus tx_eq_gain_db, as taikou-test1-1 built the B4 that decoded without errors */
            double *b = r->tx_hp_b, *a = r->tx_hp_a, *s = r->tx_hp_s;
            for (unsigned i = 0; i < k; ++i) {
                double v = b[0] * t[i] + b[1] * s[0] + b[2] * s[1] - a[1] * s[2] - a[2] * s[3];
                s[1] = s[0]; s[0] = t[i]; s[3] = s[2]; s[2] = v;
                t[i] = (int16_t)lrint(v);
            }
            teq_run(r->txq, t, (int)k);
            double g = 10.0 * pow(10.0, r->c.tx_eq_gain_db / 20.0);
            for (unsigned i = 0; i < k; ++i) {
                double v = t[i] * g;
                if (v > 32767 || v < -32768) {
                    r->st.eq_clips++;
                    v = v > 0 ? 32767 : -32768;
                }
                t[i] = (int16_t)lrint(v);
            }
        }
        for (unsigned i = 0; i < k && r->fcount < FIFO_LEN; ++i) {
            r->fifo[(r->fhead + r->fcount++) % FIFO_LEN] = t[i];
        }
        x += k;
        n -= k;
    }
}

static int16_t fifo_get(radio_t *r)
{
    int16_t v = r->fifo[r->fhead];
    r->fhead = (r->fhead + 1) % FIFO_LEN;
    r->fcount--;
    return v;
}

static void put_syms(radio_t *r, const int8_t *sym, int n)
{
    static int16_t bb[M17_SYMS_PER_FRAME * M17_SPS];
    int k = m17_mod(&r->mod, sym, n, bb);
    if (r->c.tx_invert) {
        for (int i = 0; i < k; ++i) {
            bb[i] = (int16_t)(bb[i] == -32768 ? 32767 : -bb[i]);
        }
    }
    fifo_put(r, bb, (unsigned)k);
}

void radio_tx_begin(radio_t *r, const uint8_t lsf[M17_LSF_BYTES])
{
    radio_tx_begin_ex(r, lsf, 0);
}

void radio_tx_begin_ex(radio_t *r, const uint8_t lsf[M17_LSF_BYTES], unsigned extra_ms)
{
    memcpy(r->lsf_tx, lsf, M17_LSF_BYTES);
    if (!r->keyed) {
        m17_mod_init(&r->mod, 32767.0f * powf(10.0f, r->c.tx_dbfs / 20.0f));
        if (r->c.tx_pre) {
            unsigned clips = r->pre.clips;
            fmp_pre_init(&r->pre, &r->c.pre_radio, r->c.pre_floor_hz,
                         pow(10.0, (r->c.pre_gain_db - (r->txq ? 20.0 : 0.0)) / 20.0));
            r->pre.clips = clips;
        }
        if (r->txq) {
            teq_init(r->txq, r->txw, r->ntx);
            memset(r->tx_hp_s, 0, sizeof(r->tx_hp_s));
        }
        ptt_set(r, true);
        r->keyed = true;
        static const int16_t z[480];
        for (unsigned ms = 0; ms < r->c.tx_delay_ms + extra_ms; ms += 10) {
            fifo_put(r, z, 480);
        }
    }
    r->ending = false;
    r->tail_until = 0;
    int8_t sym[M17_SYMS_PER_FRAME];
    m17_rf_preamble(sym);
    put_syms(r, sym, M17_SYMS_PER_FRAME);
    m17_rf_lsf_frame(lsf, sym);
    put_syms(r, sym, M17_SYMS_PER_FRAME);
}

void radio_tx_frame(radio_t *r, uint16_t fn, bool last, const uint8_t payload[16])
{
    int8_t sym[M17_SYMS_PER_FRAME];
    m17_rf_stream_frame(r->lsf_tx, fn, last, payload, sym);
    put_syms(r, sym, M17_SYMS_PER_FRAME);
}

void radio_tx_end(radio_t *r)
{
    if (!r->keyed) {
        return;
    }
    int8_t sym[M17_SYMS_PER_FRAME];
    m17_rf_eot(sym);
    put_syms(r, sym, M17_SYMS_PER_FRAME);
    memset(sym, 0, 16);                   /* lets the filter ring out */
    put_syms(r, sym, 16);
    if (r->c.tx_pre) {                    /* and the precompensation's boosted lows */
        static const int16_t z[960];
        fifo_put(r, z, 960);
    }
    if (r->txq) {                         /* the equalizer delays by half its length */
        static const int16_t z[480];
        for (int done = 0; done < r->ntx * TEQ_STRIDE / 2 + 480; done += 480) {
            fifo_put(r, z, 480);
        }
    }
    r->ending = true;
    r->tail_until = 0;
}

bool radio_keyed(const radio_t *r)
{
    return r->keyed;
}

bool radio_full_duplex(const radio_t *r)
{
    return r->loop;
}

/* the end of a transmission: everything played -> tail -> unkey */
static void check_unkey(radio_t *r, bool sound_card_busy)
{
    if (!r->keyed || !r->ending || r->fcount || sound_card_busy) {
        return;
    }
    double t = now_ms();
    if (r->tail_until == 0) {
        r->tail_until = t + r->c.tx_tail_ms;
    } else if (t >= r->tail_until) {
        ptt_set(r, false);
        r->keyed = r->ending = false;
        if (!r->loop) {
            m17_demod_init(&r->dem, on_frame, r);   /* listen afresh after our own transmission */
        }
    }
}

/* ---------------------------------------------------------------- moving audio */
static void pump_loop(radio_t *r)
{
    uint64_t due = (uint64_t)((now_ms() - r->t0) * 48.0);
    static int16_t x[BUF];
    float nsig = r->c.loop_snr_db != 0.0f
                     ? 32767.0f * powf(10.0f, r->c.tx_dbfs / 20.0f) * 0.6f * powf(10.0f, -r->c.loop_snr_db / 20.0f)
                     : 0.0f;
    if (r->c.loop_radio) {
        static int16_t tx[BUF];
        static float other[BUF];
        double base = 32767.0 * pow(10.0, r->c.tx_dbfs / 20.0);
        double nr = r->c.loop_snr_db != 0.0f
                        ? (r->c.rx_if ? 8000.0 / sqrt(2.0) : FMP_NOMINAL * 0.6) * pow(10.0, -r->c.loop_snr_db / 20.0)
                        : 0.0;
        while (r->loop_done + BUF <= due) {
            for (int i = 0; i < BUF; ++i) {
                tx[i] = 0;
                if (r->keyed && r->fcount) {
                    tx[i] = fifo_get(r);
                } else if (r->keyed && !r->ending && i == 0) {
                    r->st.tx_underruns++;
                }
                other[i] = 0;
                if (r->icount) {                  /* another station with a flat (data port) transmitter */
                    other[i] = (float)(r->inj[r->ihead] / base);
                    r->ihead = (r->ihead + 1) % INJ_LEN;
                    r->icount--;
                }
            }
            fmp_model_run(&r->model, tx, other, x, BUF, nr, &r->seed);
            r->loop_done += BUF;
            demod_push(r, x, BUF);
            check_unkey(r, false);
        }
        return;
    }
    while (r->loop_done + BUF <= due) {
        for (int i = 0; i < BUF; ++i) {
            float v = 0;
            if (r->keyed && r->fcount) {
                v = fifo_get(r);
            } else if (r->keyed && !r->ending && i == 0) {
                r->st.tx_underruns++;
            }
            if (r->icount) {
                v += r->inj[r->ihead];
                r->ihead = (r->ihead + 1) % INJ_LEN;
                r->icount--;
            }
            if (r->c.loop_voice_band) {
                for (int k = 0; k < 3; ++k) {
                    v = bq_run(&r->tx_bq[k], v);
                }
            }
            if (nsig != 0.0f) {
                v += nsig * gauss(r);
            }
            if (r->c.loop_voice_band) {
                for (int k = 0; k < 3; ++k) {
                    v = bq_run(&r->rx_bq[k], v);
                }
            }
            x[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        r->loop_done += BUF;
        demod_push(r, x, BUF);
        check_unkey(r, false);
    }
}

static void pump_card(radio_t *r)
{
    for (int i = 0; i < NOUT; ++i) {
        if (r->obusy[i] && (r->oh[i].dwFlags & WHDR_DONE)) {
            r->obusy[i] = false;
        }
    }
    if (r->keyed) {
        for (int i = 0; i < NOUT; ++i) {
            if (r->obusy[i]) {
                continue;
            }
            if (r->ending && !r->fcount) {
                break;
            }
            for (int k = 0; k < BUF; ++k) {
                if (r->fcount) {
                    r->ob[i][k] = fifo_get(r);
                } else {
                    r->ob[i][k] = 0;
                    if (!r->ending && k == 0) {
                        r->st.tx_underruns++;
                    }
                }
            }
            r->oh[i].dwFlags &= ~WHDR_DONE;
            if (waveOutWrite(r->wo, &r->oh[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                r->obusy[i] = true;
            }
        }
    }
    bool busy = false;
    for (int i = 0; i < NOUT; ++i) {
        busy |= r->obusy[i];
    }
    check_unkey(r, busy);
    for (;;) {                            /* input, in order */
        WAVEHDR *h = &r->ih[r->inext];
        if (!(h->dwFlags & WHDR_DONE)) {
            break;
        }
        if (!r->keyed && h->dwBytesRecorded >= 2) {
            demod_push(r, r->ib[r->inext], (int)(h->dwBytesRecorded / 2));
        }
        h->dwFlags &= ~WHDR_DONE;
        h->dwBytesRecorded = 0;
        waveInAddBuffer(r->wi, h, sizeof(WAVEHDR));
        r->inext = (r->inext + 1) % NIN;
    }
}

int radio_poll(radio_t *r, radio_evt_t *ev, int max)
{
    if (r->loop) {
        pump_loop(r);
    } else {
        pump_card(r);
    }
    int n = r->nev < max ? r->nev : max;
    memcpy(ev, r->ev, sizeof(radio_evt_t) * (size_t)n);
    memmove(r->ev, r->ev + n, sizeof(radio_evt_t) * (size_t)(r->nev - n));
    r->nev -= n;
    return n;
}

void radio_loop_inject(radio_t *r, const int8_t *sym, int n)
{
    if (!r->loop) {
        return;
    }
    if (!r->inj) {
        r->inj = malloc(sizeof(int16_t) * INJ_LEN);
        m17_mod_init(&r->imod, 32767.0f * powf(10.0f, r->c.tx_dbfs / 20.0f));
    }
    static int16_t bb[M17_SYMS_PER_FRAME * M17_SPS];
    while (n > 0) {
        int k = n > M17_SYMS_PER_FRAME ? M17_SYMS_PER_FRAME : n;
        int m = m17_mod(&r->imod, sym, k, bb);
        for (int i = 0; i < m && r->icount < INJ_LEN; ++i) {
            r->inj[(r->ihead + r->icount++) % INJ_LEN] = bb[i];
        }
        sym += k;
        n -= k;
    }
}

const radio_stats_t *radio_stats(const radio_t *r)
{
    return &r->st;
}

/* ---------------------------------------------------------------- open / close */
static WAVEFORMATEX fmt48(void)
{
    WAVEFORMATEX f;
    memset(&f, 0, sizeof(f));
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = 48000;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = 96000;
    return f;
}

radio_t *radio_open(const radio_cfg_t *c, radio_log_fn log)
{
    static bool rf_init;
    if (!rf_init) {
        m17_rf_init();
        rf_init = true;
    }
    radio_t *r = calloc(1, sizeof(*r));
    r->c = *c;
    r->log = log;
    r->fifo = malloc(sizeof(int16_t) * FIFO_LEN);
    r->rnd = 12345;
    r->st.in_dbfs = -99;
    r->loop = c->in_device == RADIO_DEV_LOOP || c->out_device == RADIO_DEV_LOOP;
    m17_demod_init(&r->dem, on_frame, r);
    r->seed = 4321;
    if (c->rx_if) {
        fmp_ifdem_init(&r->ifd, FMP_IF_HZ);
    }
    if (c->tx_eq[0]) {
        r->ntx = teq_load(c->tx_eq, r->txw);
        if (!r->ntx) {
            log("[W] radio: cannot read the transmit equalizer %s: sending without it", c->tx_eq);
        } else {
            teq_unity_1k(r->txw, r->ntx);
            r->txq = malloc(sizeof(teq_t));
            teq_init(r->txq, r->txw, r->ntx);
            double w0 = tan(M_PI * c->tx_eq_hp_hz / 48000.0), k1 = M_SQRT2 * w0, k2 = w0 * w0, a0 = 1 + k1 + k2;
            r->tx_hp_b[0] = 1 / a0; r->tx_hp_b[1] = -2 / a0; r->tx_hp_b[2] = 1 / a0;
            r->tx_hp_a[0] = 1; r->tx_hp_a[1] = 2 * (k2 - 1) / a0; r->tx_hp_a[2] = (1 - k1 + k2) / a0;
            log("radio: transmit equalizer %s (%d taps), high-pass %.0f Hz, %+.1f dB", c->tx_eq, r->ntx, c->tx_eq_hp_hz,
                c->tx_eq_gain_db);
        }
    }
    if (c->rx_eq[0]) {
        r->nrx = teq_load(c->rx_eq, r->rxw);
        if (!r->nrx) {
            log("[W] radio: cannot read the receive equalizer %s: receiving without it", c->rx_eq);
        } else if (!c->rx_if) {
            log("[W] radio: the receive equalizer works with the 12 kHz IF input only: not used");
        } else {
            r->rxq = malloc(sizeof(teq_t));
            teq_init(r->rxq, r->rxw, r->nrx);
            log("radio: receive equalizer %s (%d taps)", c->rx_eq, r->nrx);
        }
    }
    if (c->tx_pre) {
        fmp_pre_init(&r->pre, &c->pre_radio, c->pre_floor_hz, pow(10.0, (c->pre_gain_db - (r->txq ? 20.0 : 0.0)) / 20.0));
    }
    if (!ptt_open(r)) {
        free(r->txq);
        free(r->rxq);
        free(r->fifo);
        free(r);
        return NULL;
    }
    if (c->rx_if) {
        log("radio: the input is the radio's 12 kHz IF (FM discriminator here)");
    }
    if (c->tx_pre) {
        log("radio: TX precompensation for HP %.0f Hz (order %d), pre-emphasis %.0f us: lows restored down to %.0f Hz, "
            "gain %.1f dB", c->pre_radio.hp_hz, c->pre_radio.hp_order, c->pre_radio.preemph_us, c->pre_floor_hz,
            c->pre_gain_db);
    }
    if (r->loop) {
        r->t0 = now_ms();
        bq_set(&r->tx_bq[0], true, 300, 0.707);
        bq_set(&r->tx_bq[1], false, 3000, 0.541);
        bq_set(&r->tx_bq[2], false, 3000, 1.307);
        memcpy(r->rx_bq, r->tx_bq, sizeof(r->rx_bq));
        log("radio: internal loopback (no sound card)%s, SNR %s", c->loop_voice_band ? ", voice band 300-3000 Hz" : "",
            c->loop_snr_db != 0.0f ? "set" : "no noise");
        if (c->loop_radio) {
            double base = 32767.0 * pow(10.0, c->tx_dbfs / 20.0) * (c->tx_pre ? pow(10.0, c->pre_gain_db / 20.0) : 1.0);
            fmp_model_init(&r->model, &c->pre_radio, c->loop_limit, c->rx_if, 1.0 / base);
            log("    through a radio model: HP %.0f Hz (order %d), pre-emphasis %.0f us, limiter %.2f, LP %.0f Hz, "
                "receiver %s", c->pre_radio.hp_hz, c->pre_radio.hp_order, c->pre_radio.preemph_us, c->loop_limit,
                c->pre_radio.lp_hz, c->rx_if ? "12 kHz IF" : "AF (de-emphasis, voice band)");
        }
        if (c->loop_snr_db != 0.0f) {
            log("    noise: SNR %.1f dB", c->loop_snr_db);
        }
        return r;
    }
    WAVEFORMATEX f = fmt48();
    if (waveOutOpen(&r->wo, c->out_device < 0 ? WAVE_MAPPER : (UINT)c->out_device, &f, 0, 0, CALLBACK_NULL) !=
        MMSYSERR_NOERROR) {
        log("[E] cannot open the sound output to the radio (device %d)", c->out_device);
        goto fail;
    }
    if (waveInOpen(&r->wi, c->in_device < 0 ? WAVE_MAPPER : (UINT)c->in_device, &f, 0, 0, CALLBACK_NULL) !=
        MMSYSERR_NOERROR) {
        log("[E] cannot open the sound input from the radio (device %d)", c->in_device);
        waveOutClose(r->wo);
        goto fail;
    }
    for (int i = 0; i < NOUT; ++i) {
        r->oh[i].lpData = (LPSTR)r->ob[i];
        r->oh[i].dwBufferLength = sizeof(r->ob[i]);
        waveOutPrepareHeader(r->wo, &r->oh[i], sizeof(WAVEHDR));
    }
    for (int i = 0; i < NIN; ++i) {
        r->ih[i].lpData = (LPSTR)r->ib[i];
        r->ih[i].dwBufferLength = sizeof(r->ib[i]);
        waveInPrepareHeader(r->wi, &r->ih[i], sizeof(WAVEHDR));
        waveInAddBuffer(r->wi, &r->ih[i], sizeof(WAVEHDR));
    }
    waveInStart(r->wi);
    {
        char in[64] = "Windows default", out[64] = "Windows default";
        if (c->in_device >= 0) {
            radio_in_name(c->in_device, in, sizeof(in));
        }
        if (c->out_device >= 0) {
            radio_out_name(c->out_device, out, sizeof(out));
        }
        log("radio: in \"%s\", out \"%s\", 48 kHz, TX level %.1f dBFS%s, TX delay %u ms", in, out, c->tx_dbfs,
            c->tx_invert ? " (inverted)" : "", c->tx_delay_ms);
    }
    return r;
fail:
    if (r->com != INVALID_HANDLE_VALUE) {
        CloseHandle(r->com);
    }
    free(r->txq);
    free(r->rxq);
    free(r->fifo);
    free(r);
    return NULL;
}

void radio_close(radio_t *r)
{
    if (!r) {
        return;
    }
    if (r->keyed) {
        ptt_set(r, false);
    }
    if (!r->loop) {
        waveOutReset(r->wo);
        for (int i = 0; i < NOUT; ++i) {
            waveOutUnprepareHeader(r->wo, &r->oh[i], sizeof(WAVEHDR));
        }
        waveOutClose(r->wo);
        waveInReset(r->wi);
        for (int i = 0; i < NIN; ++i) {
            waveInUnprepareHeader(r->wi, &r->ih[i], sizeof(WAVEHDR));
        }
        waveInClose(r->wi);
    }
    if (r->com != INVALID_HANDLE_VALUE) {
        CloseHandle(r->com);
    }
    free(r->inj);
    free(r->txq);
    free(r->rxq);
    free(r->fifo);
    free(r);
}

int radio_in_count(void)
{
    return (int)waveInGetNumDevs();
}

int radio_out_count(void)
{
    return (int)waveOutGetNumDevs();
}

bool radio_in_name(int i, char *name, int size)
{
    WAVEINCAPSA caps;
    if (waveInGetDevCapsA((UINT)i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
        return false;
    }
    snprintf(name, (size_t)size, "%s", caps.szPname);
    return true;
}

bool radio_out_name(int i, char *name, int size)
{
    WAVEOUTCAPSA caps;
    if (waveOutGetDevCapsA((UINT)i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
        return false;
    }
    snprintf(name, (size_t)size, "%s", caps.szPname);
    return true;
}
