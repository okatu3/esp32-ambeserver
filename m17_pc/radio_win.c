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
    m17_demod_push(&r->dem, x, n);
}

/* ---------------------------------------------------------------- transmit */
static void fifo_put(radio_t *r, const int16_t *x, unsigned n)
{
    for (unsigned i = 0; i < n && r->fcount < FIFO_LEN; ++i) {
        r->fifo[(r->fhead + r->fcount++) % FIFO_LEN] = x[i];
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
    if (!ptt_open(r)) {
        free(r->fifo);
        free(r);
        return NULL;
    }
    if (r->loop) {
        r->t0 = now_ms();
        bq_set(&r->tx_bq[0], true, 300, 0.707);
        bq_set(&r->tx_bq[1], false, 3000, 0.541);
        bq_set(&r->tx_bq[2], false, 3000, 1.307);
        memcpy(r->rx_bq, r->tx_bq, sizeof(r->rx_bq));
        log("radio: internal loopback (no sound card)%s, SNR %s", c->loop_voice_band ? ", voice band 300-3000 Hz" : "",
            c->loop_snr_db != 0.0f ? "set" : "no noise");
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
