/* taikou-test1-1: two IC-705s, each into a dummy load, side by side (対向試験).
 * Radio A transmits (USB audio out + CI-V PTT), radio B receives (USB "IF" output = 12 kHz IF, FM-detected here).
 * Separate from m17_pc: it only reuses m17_modem/core (4FSK, frames), m17_pc/fm_path.c (IF discriminator,
 * precompensation, radio model) and m17_pc/m17.c (callsigns, CRC).
 *
 *   taikou-test1-1 list                                  sound devices and COM ports
 *   taikou-test1-1 civ COMx [--baud B] [--addr A4]       read frequency, mode, data mode, RF power (nothing is set)
 *   taikou-test1-1 ifcheck --in N [--sec S]              is the USB output of the receiver the 12 kHz IF?
 *   taikou-test1-1 tones-tx --out N --ptt COMx [--level dBFS]       A: tone steps 10 Hz..5 kHz + level steps (~40 s)
 *   taikou-test1-1 tones-rx --in N [--sec S] [--wav F]   B: record, then measure A's TX audio path, print settings
 *   taikou-test1-1 m17-tx --out N --ptt COMx --call CALL [precomp options]   A: M17 test bursts (known payload)
 *   taikou-test1-1 m17-rx --in N [--sec S] [--wav F]     B: demodulate, frames / EVM / bit errors per burst
 *   taikou-test1-1 analyze tones|m17 FILE.wav [--rx-af]  the same analysis from a saved recording
 *   taikou-test1-1 selftest                              everything through a model of the radios (no hardware)
 *
 * Common: --rx-af (B's USB output is audio, not IF), --baud 115200, --addr A4, --tx-delay MS (300), --max-tx S (45).
 * M17 bursts: B1 without precompensation, B2 with the guess (HP 300 Hz order 2, no pre-emphasis, floor 50 Hz,
 * -36 dB), B3 with --radio-hp/--radio-hp-order/--radio-preemph/--pre-floor/--pre-gain if any is given
 * (tones-rx prints them). --level dBFS: +3 symbols before the precompensation (-10). --frames N per burst (100).
 * The PTT is always released on exit, on Ctrl+C and after --max-tx seconds. Transmit only into dummy loads,
 * at the lowest power, within the licence. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fm_path.h"
#include "m17.h"
#include "m17_modem.h"
#include "m17_rf.h"
#include "taikou_eq.h"

#define FS 48000
#define VERSION "taikou-test1-1 (2026-11-17)"

/* ---------------------------------------------------------------- options */
typedef struct {
    int in, out;
    char port[16];
    unsigned baud, addr;
    unsigned tx_delay_ms;
    double max_tx_s;
    double level;              /* dBFS: tones / +3 symbols */
    double sec;
    char wav[260];
    char call[16];
    bool rx_af;
    int frames;
    fmp_radio_t radio;         /* B3 precompensation */
    double floor_hz, pre_gain;
    bool b3;
    char eq[260];              /* --eq FILE: equalize the receiver with it */
    char train_eq[260];        /* --train-eq FILE: learn the equalizer from burst --train-burst (3) and save it */
    char tx_eq[260];           /* --tx-eq FILE: m17-tx adds B4 = B3 + this equalizer */
    int train_burst;
} opt_t;

static volatile LONG s_stop;
static HANDLE s_com = INVALID_HANDLE_VALUE;
static unsigned s_addr = 0xA4;

/* ---------------------------------------------------------------- CI-V */
static bool civ_open(const char *port, unsigned baud)
{
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\%s", port);
    s_com = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (s_com == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open %s\n", port);
        return false;
    }
    DCB d;
    memset(&d, 0, sizeof(d));
    d.DCBlength = sizeof(d);
    GetCommState(s_com, &d);
    d.BaudRate = baud;
    d.ByteSize = 8;
    d.Parity = NOPARITY;
    d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE;
    d.fDtrControl = DTR_CONTROL_DISABLE;   /* the IC-705 can key on DTR/RTS (USB SEND/Keying): keep them off */
    d.fRtsControl = RTS_CONTROL_DISABLE;
    d.fOutxCtsFlow = d.fOutxDsrFlow = FALSE;
    d.fOutX = d.fInX = FALSE;
    SetCommState(s_com, &d);
    COMMTIMEOUTS t = { 0 };
    t.ReadIntervalTimeout = 20;
    t.ReadTotalTimeoutConstant = 300;
    t.WriteTotalTimeoutConstant = 300;
    SetCommTimeouts(s_com, &t);
    PurgeComm(s_com, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

static void civ_send(const uint8_t *cmd, int n)
{
    uint8_t f[32];
    int k = 0;
    f[k++] = 0xFE;
    f[k++] = 0xFE;
    f[k++] = (uint8_t)s_addr;
    f[k++] = 0xE0;
    for (int i = 0; i < n; ++i) {
        f[k++] = cmd[i];
    }
    f[k++] = 0xFD;
    DWORD w;
    WriteFile(s_com, f, (DWORD)k, &w, NULL);
}

/* the radio's answer to cmd[0] (cmd[1] sub when n > 1): body after the command bytes; length or -1 */
static int civ_query(const uint8_t *cmd, int n, uint8_t *body, int max)
{
    PurgeComm(s_com, PURGE_RXCLEAR);
    civ_send(cmd, n);
    uint8_t buf[256];
    int have = 0;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 600 && have < (int)sizeof(buf)) {
        DWORD r = 0;
        if (!ReadFile(s_com, buf + have, (DWORD)(sizeof(buf) - have), &r, NULL)) {
            break;
        }
        have += (int)r;
        for (int i = 0; i + 4 < have; ++i) {       /* FE FE E0 addr cmd ... FD */
            if (buf[i] != 0xFE || buf[i + 1] != 0xFE || buf[i + 2] != 0xE0 || buf[i + 3] != (uint8_t)s_addr) {
                continue;
            }
            int e = i + 4;
            while (e < have && buf[e] != 0xFD) {
                ++e;
            }
            if (e >= have) {
                break;
            }
            if (buf[i + 4] == 0xFA) {
                return -1;                          /* NG */
            }
            if (memcmp(buf + i + 4, cmd, (size_t)n) == 0) {
                int len = e - (i + 4 + n);
                if (len > max) {
                    len = max;
                }
                memcpy(body, buf + i + 4 + n, (size_t)len);
                return len;
            }
        }
    }
    return -1;
}

static void ptt(bool on)
{
    if (s_com == INVALID_HANDLE_VALUE) {
        return;
    }
    uint8_t c[3] = { 0x1C, 0x00, (uint8_t)(on ? 1 : 0) };
    civ_send(c, 3);
    if (!on) {
        Sleep(30);
        civ_send(c, 3);                             /* twice: never leave it keyed */
    }
}

static BOOL WINAPI on_ctrl(DWORD ev)
{
    (void)ev;
    InterlockedExchange(&s_stop, 1);
    ptt(false);
    return TRUE;
}

static void at_exit(void)
{
    ptt(false);
}

/* taikou-gui: the window cannot send Ctrl+C to a hidden console, so it sets a named event instead */
static DWORD WINAPI stop_watch(LPVOID h)
{
    WaitForSingleObject((HANDLE)h, INFINITE);
    on_ctrl(0);
    return 0;
}

static unsigned bcd(uint8_t b)
{
    return (b >> 4) * 10u + (b & 15u);
}

static int cmd_civ(const opt_t *o)
{
    if (!civ_open(o->port, o->baud)) {
        return 1;
    }
    uint8_t b[32];
    uint8_t c03[1] = { 0x03 }, c04[1] = { 0x04 }, c1a06[2] = { 0x1A, 0x06 }, c140a[2] = { 0x14, 0x0A };
    uint8_t c1c[2] = { 0x1C, 0x00 };
    int n = civ_query(c03, 1, b, sizeof(b));
    if (n < 5) {
        printf("%s: no answer from CI-V address %02X at %u bps (USB CI-V baud, \"CI-V USB Echo Back\", address?)\n",
               o->port, o->addr, o->baud);
        return 1;
    }
    unsigned long long hz = 0, mul = 1;
    for (int i = 0; i < 5; ++i) {
        hz += (unsigned long long)(bcd(b[i])) * mul;
        mul *= 100;
    }
    printf("%s: frequency %llu.%06llu MHz\n", o->port, hz / 1000000ull, hz % 1000000ull);
    n = civ_query(c04, 1, b, sizeof(b));
    static const char *mn[0x18] = { "LSB", "USB", "AM", "CW", "RTTY", "FM", "WFM", "CW-R", "RTTY-R", 0, 0, 0, 0, 0, 0,
                                    0, 0, "DV" };
    if (n >= 1) {
        printf("    mode %s (%02X), filter %d\n", b[0] < 0x18 && mn[b[0]] ? mn[b[0]] : "?", b[0], n > 1 ? b[1] : 0);
    }
    bool fm = n >= 1 && b[0] == 0x05;
    n = civ_query(c1a06, 2, b, sizeof(b));
    if (n >= 1) {
        printf("    data mode %s\n", b[0] ? "ON (-D)" : "off");
    }
    if (!fm || n < 1 || !b[0]) {
        printf("    [!] not FM-D: for this test press FM, then DATA (FM-D). tones-tx / m17-tx refuse other modes.\n");
    }
    n = civ_query(c140a, 2, b, sizeof(b));
    if (n >= 2) {
        unsigned v = bcd(b[0]) * 100u + bcd(b[1]);
        printf("    RF power setting %u / 255 (%.0f %%)%s\n", v, v / 2.55, v > 30 ? "  <- lower it for the dummy-load test" : "");
    }
    n = civ_query(c1c, 2, b, sizeof(b));
    if (n >= 1) {
        printf("    transmitting: %s\n", b[0] ? "YES" : "no");
    }
    printf("    nothing was changed on the radio\n");
    return 0;
}

/* ---------------------------------------------------------------- sound */
static WAVEFORMATEX fmt48(void)
{
    WAVEFORMATEX f;
    memset(&f, 0, sizeof(f));
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = FS;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = FS * 2;
    return f;
}

static int cmd_list(void)
{
    printf("sound inputs (--in N):\n");
    for (UINT i = 0; i < waveInGetNumDevs(); ++i) {
        WAVEINCAPSA c;
        if (waveInGetDevCapsA(i, &c, sizeof(c)) == MMSYSERR_NOERROR) {
            printf("  %u  %s\n", i, c.szPname);
        }
    }
    printf("sound outputs (--out N):\n");
    for (UINT i = 0; i < waveOutGetNumDevs(); ++i) {
        WAVEOUTCAPSA c;
        if (waveOutGetDevCapsA(i, &c, sizeof(c)) == MMSYSERR_NOERROR) {
            printf("  %u  %s\n", i, c.szPname);
        }
    }
    printf("COM ports:\n");
    char names[16384];
    DWORD n = QueryDosDeviceA(NULL, names, sizeof(names));
    for (char *p = names; n && *p; p += strlen(p) + 1) {
        if (!strncmp(p, "COM", 3)) {
            printf("  %s\n", p);
        }
    }
    printf("Two IC-705s show two \"USB Audio CODEC\" entries: try `civ COMx` and `ifcheck --in N` to tell them apart.\n");
    return 0;
}

/* plays x (n samples) on device out; PTT on before (tx_delay of silence first), off after. false if stopped. */
static bool transmit(const opt_t *o, const int16_t *x, int n)
{
    if ((double)n / FS > o->max_tx_s) {
        fprintf(stderr, "refusing: %.1f s is longer than --max-tx %.0f s\n", (double)n / FS, o->max_tx_s);
        return false;
    }
    int pre = (int)(o->tx_delay_ms * (FS / 1000)), post = FS / 10;
    int total = pre + n + post;
    int16_t *b = calloc((size_t)total, 2);
    memcpy(b + pre, x, (size_t)n * 2);
    HWAVEOUT wo;
    WAVEFORMATEX f = fmt48();
    if (waveOutOpen(&wo, (UINT)o->out, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "cannot open sound output %d\n", o->out);
        free(b);
        return false;
    }
    WAVEHDR h;
    memset(&h, 0, sizeof(h));
    h.lpData = (LPSTR)b;
    h.dwBufferLength = (DWORD)total * 2;
    waveOutPrepareHeader(wo, &h, sizeof(h));
    ptt(true);
    waveOutWrite(wo, &h, sizeof(h));
    DWORD t0 = GetTickCount();
    while (!(h.dwFlags & WHDR_DONE) && !s_stop && GetTickCount() - t0 < (DWORD)(o->max_tx_s * 1000 + 2000)) {
        Sleep(10);
    }
    ptt(false);
    waveOutReset(wo);
    waveOutUnprepareHeader(wo, &h, sizeof(h));
    waveOutClose(wo);
    free(b);
    return !s_stop;
}

/* records up to sec seconds (Ctrl+C ends early); block callback every 20 ms; returns the samples (caller frees) */
typedef void (*block_fn)(void *ctx, const int16_t *x, int n);

static int16_t *record(const opt_t *o, double sec, int *n_out, block_fn cb, void *ctx)
{
    enum { NB = 32, BL = 960 };
    static int16_t ib[NB][BL];
    WAVEHDR h[NB];
    HWAVEIN wi;
    WAVEFORMATEX f = fmt48();
    if (waveInOpen(&wi, (UINT)o->in, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "cannot open sound input %d\n", o->in);
        return NULL;
    }
    int cap = (int)(sec * FS) + BL;
    int16_t *all = malloc((size_t)cap * 2);
    int have = 0;
    for (int i = 0; i < NB; ++i) {
        memset(&h[i], 0, sizeof(h[i]));
        h[i].lpData = (LPSTR)ib[i];
        h[i].dwBufferLength = sizeof(ib[i]);
        waveInPrepareHeader(wi, &h[i], sizeof(h[i]));
        waveInAddBuffer(wi, &h[i], sizeof(h[i]));
    }
    waveInStart(wi);
    int next = 0;
    double shown = 0;
    while (!s_stop && have + BL <= cap) {
        if (!(h[next].dwFlags & WHDR_DONE)) {
            Sleep(5);
            continue;
        }
        int k = (int)(h[next].dwBytesRecorded / 2);
        memcpy(all + have, ib[next], (size_t)k * 2);
        if (cb) {
            cb(ctx, all + have, k);
        }
        have += k;
        h[next].dwFlags &= ~WHDR_DONE;
        waveInAddBuffer(wi, &h[next], sizeof(h[next]));
        next = (next + 1) % NB;
        if ((double)have / FS - shown >= 5) {
            shown = (double)have / FS;
            fprintf(stderr, "  ... %.0f s\n", shown);
        }
    }
    waveInReset(wi);
    for (int i = 0; i < NB; ++i) {
        waveInUnprepareHeader(wi, &h[i], sizeof(h[i]));
    }
    waveInClose(wi);
    *n_out = have;
    return all;
}

static void write_wav(const char *path, const int16_t *x, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    unsigned data = (unsigned)n * 2;
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                      0x80, 0xBB, 0, 0, 0, 0x77, 1, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
    unsigned riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &data, 4);
    fwrite(h, 1, 44, f);
    fwrite(x, 2, (size_t)n, f);
    fclose(f);
    printf("saved %s (%.1f s)\n", path, (double)n / FS);
}

static int16_t *read_wav(const char *path, int *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4)) {
        fclose(f);
        return NULL;
    }
    int16_t *d = NULL;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) {
            break;
        }
        unsigned sz = c[4] | c[5] << 8 | c[6] << 16 | (unsigned)c[7] << 24;
        if (!memcmp(c, "data", 4)) {
            d = malloc(sz);
            *n = (int)fread(d, 2, sz / 2, f);
            break;
        }
        fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
    }
    fclose(f);
    return d;
}

/* the receiver's output -> FM discriminator output (deviation: FMP_NOMINAL = 2.4 kHz) */
static int16_t *to_baseband(const int16_t *x, int n, bool rx_af)
{
    int16_t *y = malloc((size_t)n * 2);
    if (rx_af) {
        memcpy(y, x, (size_t)n * 2);
    } else {
        fmp_ifdem_t d;
        fmp_ifdem_init(&d, FMP_IF_HZ);
        fmp_ifdem_run(&d, x, y, n);
    }
    return y;
}

/* ---------------------------------------------------------------- IF check */
static double band_power(const int16_t *x, int n, double f0, double f1)
{
    /* sum of Goertzel powers on a 100 Hz grid over 1 s blocks, normalized */
    double p = 0;
    int blocks = 0;
    for (int s = 0; s + FS <= n && blocks < 10; s += FS, ++blocks) {
        for (double f = f0; f < f1; f += 100) {
            double w = 2 * M_PI * f / FS, c = 2 * cos(w), s1 = 0, s2 = 0;
            for (int i = 0; i < FS; ++i) {
                double s0 = x[s + i] + c * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            p += s1 * s1 + s2 * s2 - c * s1 * s2;
        }
    }
    return blocks ? p / blocks / ((double)FS * FS) : 0;
}

static int cmd_ifcheck(const opt_t *o)
{
    printf("recording %.0f s from input %d. Let the other radio transmit a carrier (or any FM signal) meanwhile,\n"
           "or listen to an empty channel for the noise.\n", o->sec, o->in);
    int n = 0;
    int16_t *x = record(o, o->sec, &n, NULL, NULL);
    if (!x || n < FS) {
        return 1;
    }
    double e = 0;
    int pk = 0;
    for (int i = 0; i < n; ++i) {
        e += (double)x[i] * x[i];
        pk = abs(x[i]) > pk ? abs(x[i]) : pk;
    }
    printf("level: RMS %.1f dBFS, peak %d\n", 10 * log10(e / n / 1073741824.0 + 1e-12), pk);
    double lo = band_power(x, n, 100, 3500), mid = band_power(x, n, 6000, 18000), hi = band_power(x, n, 19000, 23500);
    printf("power 0.1-3.5 kHz %.1f dB, 6-18 kHz %.1f dB, 19-23.5 kHz %.1f dB\n", 10 * log10(lo + 1e-12),
           10 * log10(mid + 1e-12), 10 * log10(hi + 1e-12));
    if (mid > 4 * lo && mid > 4 * hi) {
        printf("=> looks like the 12 kHz IF (most of the power is around 12 kHz).\n");
        int16_t *b = to_baseband(x, n, false);
        double m = 0;
        for (int i = n / 2; i < n; ++i) {
            m += b[i];
        }
        m /= (n - n / 2);
        printf("   FM detected here: mean %+.0f Hz (frequency error between the radios)\n", m / FMP_NOMINAL * FMP_DEV_HZ);
        free(b);
    } else if (lo > 4 * mid) {
        printf("=> looks like AUDIO, not IF: set USB AF/IF Output > Output Select = IF on this radio.\n");
    } else {
        printf("=> unclear: transmit a carrier from the other radio during the check.\n");
    }
    if (o->wav[0]) {
        write_wav(o->wav, x, n);
    }
    free(x);
    return 0;
}

/* ---------------------------------------------------------------- tone steps */
static const double TONES[] = { 10, 15, 20, 30, 40, 50, 70, 100, 150, 200, 300, 400, 500, 700, 1000,
                                1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000 };
#define NTONES ((int)(sizeof(TONES) / sizeof(TONES[0])))
static const double LEVELS[] = { -24, -18, -12, -6, 0, 6 };      /* 1 kHz, relative to --level */
#define NLEVELS ((int)(sizeof(LEVELS) / sizeof(LEVELS[0])))
#define SEG_S 1.0
#define GAP_S 0.2
#define REF_S 2.0

/* segment k: 0 = reference 1 kHz (REF_S), 1..NTONES tones, then the levels; start times from the reference start */
static void seg(int k, double *t0, double *len, double *f, double *rel_db)
{
    if (k == 0) {
        *t0 = 0;
        *len = REF_S;
        *f = 1000;
        *rel_db = 0;
        return;
    }
    *t0 = REF_S + GAP_S + (k - 1) * (SEG_S + GAP_S);
    *len = SEG_S;
    if (k <= NTONES) {
        *f = TONES[k - 1];
        *rel_db = 0;
    } else {
        *f = 1000;
        *rel_db = LEVELS[k - 1 - NTONES];
    }
}
#define NSEG (1 + NTONES + NLEVELS)

static int16_t *make_tones(double level, int *n_out)
{
    double t0, len, f, rel;
    seg(NSEG - 1, &t0, &len, &f, &rel);
    int n = (int)((t0 + len + 0.3) * FS);
    int16_t *x = calloc((size_t)n, 2);
    for (int k = 0; k < NSEG; ++k) {
        seg(k, &t0, &len, &f, &rel);
        double a = 32767.0 * pow(10, (level + rel) / 20);
        if (a > 32000) {
            a = 32000;
        }
        int s0 = (int)(t0 * FS), m = (int)(len * FS), ramp = FS / 100;
        for (int i = 0; i < m; ++i) {
            double env = i < ramp ? (double)i / ramp : (i > m - ramp ? (double)(m - i) / ramp : 1);
            x[s0 + i] = (int16_t)lrint(a * env * sin(2 * M_PI * f * i / FS));
        }
    }
    *n_out = n;
    return x;
}

/* amplitude of a tone at f in y[s0 .. s0+m) (Hann window), in y units */
static double tone_amp(const int16_t *y, int s0, int m, double f)
{
    double re = 0, im = 0, ws = 0;
    for (int i = 0; i < m; ++i) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / (m - 1));
        double v = y[s0 + i] * w;
        re += v * cos(2 * M_PI * f * i / FS);
        im -= v * sin(2 * M_PI * f * i / FS);
        ws += w;
    }
    return 2 * sqrt(re * re + im * im) / ws;
}

/* model of the radio's TX audio path magnitude (analog), for the fit */
static int s_lp_pow = 8;   /* 8: a gentle low-pass, 32: a wall like the IC-705 (both are tried in the fit) */

static double model_db(double f, double hp, int order, double pre_us, double lp)
{
    double g = 1;
    if (hp > 0) {
        double r = pow(f / hp, order);
        g *= r / sqrt(1 + r * r);
    }
    if (pre_us > 0) {
        double w = 2 * M_PI * f, t1 = pre_us * 1e-6, t2 = 1 / (2 * M_PI * 6000.0);
        g *= sqrt(1 + w * w * t1 * t1) / sqrt(1 + w * w * t2 * t2);
    }
    if (lp > 0) {
        g /= sqrt(1 + pow(f / lp, s_lp_pow));      /* the IC-705 cuts like a wall: 3 kHz +8 dB, 3.5 kHz -33 dB (2026-11-18) */
    }
    return 20 * log10(g + 1e-12);
}

static int analyze_tones(const int16_t *raw, int n, bool rx_af, double level)
{
    int16_t *y = to_baseband(raw, n, rx_af);
    /* the reference: first 0.1 s window where 1 kHz dominates and is strong */
    int win = FS / 10, start = -1;
    for (int s = 0; s + win <= n; s += win / 2) {
        /* means removed: the two radios are ~1 kHz apart (2026-11-18: -960 Hz), which is a DC on the deviation */
        double a = tone_amp(y, s, win, 1000), e = 0, mean = 0;
        for (int i = 0; i < win; ++i) {
            mean += y[s + i];
        }
        mean /= win;
        for (int i = 0; i < win; ++i) {
            e += (y[s + i] - mean) * (y[s + i] - mean);
        }
        double rms = sqrt(e / win);
        if (a > 300 && a / sqrt(2.0) > 0.7 * rms) {
            start = s;
            break;
        }
    }
    if (start < 0) {
        printf("no 1 kHz reference found: was the other radio transmitting tones-tx, and is the level high enough?\n");
        free(y);
        return 1;
    }
    /* refine: step back while 1 kHz is still present */
    while (start > win / 4 && tone_amp(y, start - win / 4, win / 4, 1000) > 0.5 * tone_amp(y, start, win / 4, 1000)) {
        start -= win / 4;
    }
    double dev_per_unit = FMP_DEV_HZ / FMP_NOMINAL;
    double t0, len, f, rel;
    double amp[NSEG];
    for (int k = 0; k < NSEG; ++k) {
        seg(k, &t0, &len, &f, &rel);
        int s0 = start + (int)((t0 + 0.2) * FS), m = (int)((len - 0.4) * FS);
        if (s0 + m > n) {
            amp[k] = -1;
            continue;
        }
        amp[k] = tone_amp(y, s0, m, f) * dev_per_unit;     /* Hz of deviation (peak) */
    }
    double ref = amp[0];
    printf("reference 1 kHz at %.1f dBFS: deviation %.0f Hz peak (start found at %.2f s)\n", level, ref,
           (double)start / FS);
    printf("\nfrequency response of the transmitter's audio path (relative to 1 kHz):\n");
    double meas[NTONES];
    int use[NTONES];
    for (int i = 0; i < NTONES; ++i) {
        double a = amp[1 + i];
        meas[i] = a > 0 ? 20 * log10(a / ref + 1e-9) : -99;
        use[i] = a > 0 && meas[i] > -40 && !(TONES[i] > 3000 && meas[i] < -20);   /* the wall above 3 kHz: not fitted */
        printf("  %6.0f Hz  %7.1f dB  (%5.0f Hz dev)%s\n", TONES[i], meas[i], a, use[i] ? "" : "  (too weak, not fitted)");
    }
    printf("\nlevel steps at 1 kHz (deviation should grow 6 dB per step until the limiter):\n");
    double dmax = 0;
    for (int i = 0; i < NLEVELS; ++i) {
        double a = amp[1 + NTONES + i];
        double expect = ref * pow(10, LEVELS[i] / 20);
        printf("  %+5.0f dB (%.1f dBFS%s)  %6.0f Hz  (linear would be %6.0f Hz, %+.1f dB)\n", LEVELS[i], level + LEVELS[i],
               level + LEVELS[i] > -0.3 ? ", clipped at the sound card" : "", a, expect,
               a > 0 ? 20 * log10(a / expect) : -99);
        if (a > dmax) {
            dmax = a;
        }
    }
    /* deviation per full scale from the lowest step (the linear part), not from the reference: on 2026-11-18 the
     * reference was 14 dB into the IC-705's deviation limiter, which flattened the whole response table */
    double low = amp[1 + NTONES], low_db = level + LEVELS[0];
    double dev_fs = low > 0 ? low / pow(10, low_db / 20) : ref / pow(10, level / 20);
    double ex = low > 0 ? 20 * log10(low / (ref * pow(10, LEVELS[0] / 20))) : 0;
    if (ex > 2) {
        double l_ok = 20 * log10(1000.0 / dev_fs);     /* reference at about 1 kHz deviation */
        printf("\n[!] LIMITER: the reference is already %.0f dB into the transmitter's deviation limiter (about %.0f Hz peak).\n"
               "    The frequency response above is flattened by the limiter and is NOT valid, so no filter is fitted.\n"
               "    Deviation per full scale (from the lowest step): %.0f Hz.\n",
               ex, dmax, dev_fs);
        printf("for tones-tx: --level %.1f\n", l_ok);
        double m = 20 * log10(2400.0 / dev_fs);
        printf("for m17-tx (levels): --level %.1f --pre-gain 0\n", m);
        printf("    (repeat tones-tx / tones-rx with that level to measure the filters)\n");
        free(y);
        return 0;
    }
    /* fit */
    double best = 1e30, bh = 0, bp = 0, bl = 0, bo = 0;
    int border = 0;
    /* up to 5000 us: the IC-705 FM-D rises ~6 dB/octave from 100 Hz to 3 kHz (2026-11-18) */
    static const double PRE[] = { 0, 50, 75, 150, 300, 530, 750, 1000, 1500, 2000, 3000, 5000 };
    const int NPRE = (int)(sizeof(PRE) / sizeof(PRE[0]));
    int blp = 8;
    for (int lpw = 8; lpw <= 32; lpw += 24) {
    s_lp_pow = lpw;
    for (int order = 1; order <= 2; ++order) {
        for (double hp = 0; hp <= 800; hp += (hp < 100 ? 5 : 10)) {
            for (int pi = 0; pi < NPRE; ++pi) {
                for (double lp = 2500; lp <= 6000; lp += 100) {
                    double sum = 0, cnt = 0, d[NTONES];
                    for (int i = 0; i < NTONES; ++i) {
                        d[i] = meas[i] - (model_db(TONES[i], hp, order, PRE[pi], lp) - model_db(1000, hp, order, PRE[pi], lp));
                        if (use[i]) {
                            sum += d[i];
                            cnt++;
                        }
                    }
                    double off = sum / (cnt > 0 ? cnt : 1), e = 0;
                    for (int i = 0; i < NTONES; ++i) {
                        if (use[i]) {
                            e += (d[i] - off) * (d[i] - off);
                        }
                    }
                    if (e < best) {
                        best = e;
                        bh = hp;
                        border = order;
                        bp = PRE[pi];
                        bl = lp;
                        bo = off;
                        blp = lpw;
                    }
                }
            }
        }
    }
    }
    s_lp_pow = blp;
    int nu = 0;
    for (int i = 0; i < NTONES; ++i) {
        nu += use[i];
    }
    char pe[32] = "none";
    if (bp > 0) {
        snprintf(pe, sizeof(pe), "%.0f us", bp);
    }
    printf("\nbest fit: high-pass %.0f Hz order %d, pre-emphasis %s, low-pass %.0f Hz (RMS error %.1f dB over %d tones)\n",
           bh, border, pe, bl, sqrt(best / (nu ? nu : 1)), nu);
    if (sqrt(best / (nu ? nu : 1)) > 2) {
        printf("    the fit is poor (> 2 dB): look at the table above; the path may not be a simple filter\n");
    }
    (void)bo;
    printf("\nfor m17_pc ([radio] in m17_pc.ini): radio_hp_hz=%.0f  radio_hp_order=%d  radio_preemph_us=%.0f\n", bh,
           border, bp);
    printf("for m17-tx (burst B3):               --radio-hp %.0f --radio-hp-order %d --radio-preemph %.0f\n", bh, border,
           bp);
    /* level advice: deviation per full scale (linear part) */
    printf("\ndeviation per full scale (1 kHz): %.0f Hz, largest step %.0f Hz. M17 wants 2400 Hz for a +3 symbol.\n",
           dev_fs, dmax);
    double need_db = 20 * log10(2400.0 / dev_fs);
    /* B1 uses --level as it is, B2/B3 add --pre-gain. The de-emphasis in the precompensation is 1 at DC, so with a
     * pre-emphasis it takes away its gain at 1 kHz (IC-705 FM-D, 2026-11-18: ~30 dB), which --pre-gain gives back. */
    double emph_db = model_db(1000, 0, 1, bp, 0);
    double boost = bh > 50 ? 20 * border * log10(bh / 50.0) : 0;   /* precompensation at the 50 Hz floor */
    double lv = need_db, top = -4 - emph_db - boost;             /* keep 4 dB for the RRC overshoot */
    if (lv > top) {
        lv = top;
    }
    printf("  B3: +3 symbols at %.1f dBFS, --pre-gain %.0f dB (gives back the pre-emphasis), low tones lifted by up to %.0f dB",
           lv, emph_db, boost);
    if (lv < need_db - 0.5) {
        printf("\n      that is %.1f dB below the 2400 Hz B1 needs (sound-card headroom): raise USB MOD Level on the transmitter for more room", need_db - lv);
    }
    printf("\nfor m17-tx (levels): --level %.1f --pre-gain %.0f\n", lv, emph_db);
    free(y);
    return 0;
}

static int cmd_tones_tx(const opt_t *o)
{
    int n;
    int16_t *x = make_tones(o->level, &n);
    printf("tone steps: %.1f s, 1 kHz reference at %.1f dBFS, then %d tones 10 Hz..5 kHz, then levels %+.0f..%+.0f dB\n",
           (double)n / FS, o->level, NTONES, LEVELS[0], LEVELS[NLEVELS - 1]);
    printf("start tones-rx on the other radio first. Transmitting in 3 s (Ctrl+C stops and unkeys)...\n");
    Sleep(3000);
    bool ok = !s_stop && transmit(o, x, n);
    printf(ok ? "done, unkeyed\n" : "stopped, unkeyed\n");
    free(x);
    return ok ? 0 : 1;
}

static int cmd_tones_rx(const opt_t *o)
{
    printf("recording %.0f s from input %d: start tones-tx on the other radio now (Ctrl+C ends early)\n", o->sec, o->in);
    int n = 0;
    int16_t *x = record(o, o->sec, &n, NULL, NULL);
    if (!x) {
        return 1;
    }
    char path[260];
    snprintf(path, sizeof(path), "%s", o->wav[0] ? o->wav : "tones_rx.wav");
    write_wav(path, x, n);
    int r = analyze_tones(x, n, o->rx_af, o->level);
    free(x);
    return r;
}

/* ---------------------------------------------------------------- M17 bursts */
static void pattern(int burst, uint16_t fn, uint8_t out[16])
{
    uint32_t s = 0x9E3779B9u * (uint32_t)(burst + 1) ^ (uint32_t)fn * 2654435761u;
    for (int i = 0; i < 16; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        out[i] = (uint8_t)(s >> 24);
    }
}

static void make_lsf(uint8_t lsf[30], const char *src, int burst)
{
    char dst[10];
    snprintf(dst, sizeof(dst), "TAIKOU-B%d", burst);
    memset(lsf, 0, 30);
    m17_encode_call(dst, lsf);
    m17_encode_call(src, lsf + 6);
    lsf[12] = 0x00;
    lsf[13] = 0x05;                                   /* stream, voice 3200 (payload is a test pattern) */
    uint16_t c = m17_crc(lsf, 28);
    lsf[28] = (uint8_t)(c >> 8);
    lsf[29] = (uint8_t)c;
}

/* one burst as audio: preamble, LSF, frames, EoT, with or without precompensation */
static int16_t *make_burst(const char *call, int burst, int frames, double level, const fmp_radio_t *r, double floor_hz,
                           double pre_gain, int *n_out)
{
    uint8_t lsf[30];
    make_lsf(lsf, call, burst);
    int nsym = M17_SYMS_PER_FRAME * (frames + 3) + 200;
    int8_t *sym = calloc((size_t)nsym, 1);
    int k = 0;
    m17_rf_preamble(sym + k);
    k += M17_SYMS_PER_FRAME;
    m17_rf_lsf_frame(lsf, sym + k);
    k += M17_SYMS_PER_FRAME;
    for (int f = 0; f < frames; ++f) {
        uint8_t p[16];
        pattern(burst, (uint16_t)f, p);
        m17_rf_stream_frame(lsf, (uint16_t)f, f == frames - 1, p, sym + k);
        k += M17_SYMS_PER_FRAME;
    }
    m17_rf_eot(sym + k);
    k += M17_SYMS_PER_FRAME;
    k += 100;                                          /* zeros: the filters ring out */
    int n = k * M17_SPS + FS / 20;
    int16_t *x = calloc((size_t)n, 2);
    m17_mod_t mod;
    m17_mod_init(&mod, (float)(32767.0 * pow(10, level / 20)));
    m17_mod(&mod, sym, k, x);
    if (r) {
        fmp_pre_t pre;
        fmp_pre_init(&pre, r, floor_hz, pow(10, pre_gain / 20));
        fmp_pre_run(&pre, x, n);
        if (pre.clips) {
            printf("  [W] burst %d: the precompensation clipped %u samples (lower --pre-gain or --level)\n", burst,
                   pre.clips);
        }
    }
    free(sym);
    *n_out = n;
    return x;
}

typedef struct {
    int nb;
    struct {
        int fr0, frames, lsf_ok, bits, bad_frames, lost;
        double evm_sum, evm_max;
        int last_fn;
    } b[10];
    int cur;                       /* burst of the LSF in progress, -1 unknown */
    int in_burst;
    bool quiet;
} rxst_t;

static void rx_end(rxst_t *s, const char *why)
{
    if (!s->in_burst) {
        return;
    }
    s->in_burst = 0;
    if (s->cur >= 1 && s->cur <= 9) {
        typeof(s->b[0]) *b = &s->b[s->cur];
        printf("  B%d end%s: %d frames, %d with bit errors, %d payload bit errors, EVM avg %.3f max %.3f%s\n", s->cur, why,
               b->frames, b->bad_frames, b->bits, b->frames ? b->evm_sum / b->frames : 0, b->evm_max,
               b->lsf_ok ? "" : " (no LSF)");
    }
}

static void on_frame(void *ctx, const m17_sync_info_t *in, const uint16_t *soft)
{
    rxst_t *s = ctx;
    if (in->type == M17_FR_EOT) {
        rx_end(s, " (EoT)");
        return;
    }
    if (in->type == M17_FR_LOST) {
        rx_end(s, " (signal lost)");
        return;
    }
    if (in->type == M17_FR_LSF) {
        uint8_t l[30];
        m17_rx_info_t info;
        bool ok = m17_rf_decode_lsf(soft, l, &info);
        char dst[10], src[10];
        m17_decode_call(l, dst);
        m17_decode_call(l + 6, src);
        int b = (ok && !strncmp(dst, "TAIKOU-B", 8)) ? dst[8] - '0' : -1;
        rx_end(s, "");
        s->cur = b;
        s->in_burst = 1;
        printf("LSF %s: %s -> %s  (EVM %.3f, %s polarity)\n", ok ? "ok" : "CRC BAD", src, dst, in->evm,
               in->gain < 0 ? "inverted" : "normal");
        if (b >= 1 && b <= 9) {
            s->b[b].lsf_ok++;
            if (b > s->nb) {
                s->nb = b;
            }
        }
        return;
    }
    if (in->type != M17_FR_STREAM) {
        return;
    }
    m17_stream_rx_t r;
    m17_rf_decode_stream(soft, &r);
    if (!s->in_burst) {
        s->in_burst = 1;
        s->cur = -1;
        printf("stream without LSF (joined late)\n");
    }
    if (s->cur < 1 || s->cur > 9) {
        /* no LSF (a steep cut below ~60 Hz loses it first, 2026-11-18): find the burst from the known payload */
        int best = -1, bbe = 129;
        for (int c = 1; c <= 4; ++c) {
            uint8_t q[16];
            pattern(c, r.fn, q);
            int e = 0;
            for (int i = 0; i < 16; ++i) {
                e += __builtin_popcount((unsigned)(q[i] ^ r.payload[i]));
            }
            if (e < bbe) {
                bbe = e;
                best = c;
            }
        }
        if (bbe > 40) {
            return;                                  /* not one of ours, or too broken to tell */
        }
        s->cur = best;
        printf("  -> burst B%d found from the payload (frame %u, no LSF)\n", best, r.fn);
        if (best > s->nb) {
            s->nb = best;
        }
    }
    typeof(s->b[0]) *b = &s->b[s->cur];
    uint8_t p[16];
    pattern(s->cur, r.fn, p);
    int be = 0;
    for (int i = 0; i < 16; ++i) {
        be += __builtin_popcount((unsigned)(p[i] ^ r.payload[i]));
    }
    b->frames++;
    b->bits += be;
    b->bad_frames += be != 0;
    b->evm_sum += in->evm;
    b->evm_max = in->evm > b->evm_max ? in->evm : b->evm_max;
}

typedef struct {
    m17_demod_t dem;
    fmp_ifdem_t ifd;
    bool rx_af;
    double dc;                 /* slow DC block: the two radios are ~1 kHz apart (2026-11-18: -950 Hz) */
    bool use_eq;
    teq_t eq;                  /* --eq FILE: the equalizer learned by --train-eq */
} rxpipe_t;

static rxst_t s_rx;
static double s_rx_gain = 1;   /* --rx-gain DB: scales the demodulated signal (low deviation, 2026-11-18) */
static int16_t *s_bb; static int s_bb_n, s_bb_cap;   /* the demodulated baseband: --bb FILE, and for --train-eq */
static const char *s_bb_path;
static bool s_bb_on;

static void bb_keep(const int16_t *t, int k)
{
    if (!s_bb_on) {
        return;
    }
    if (s_bb_n + k > s_bb_cap) {
        s_bb_cap = (s_bb_n + k) * 2;
        s_bb = realloc(s_bb, (size_t)s_bb_cap * 2);
    }
    memcpy(s_bb + s_bb_n, t, (size_t)k * 2);
    s_bb_n += k;
}

static void rx_block(void *ctx, const int16_t *x, int n)
{
    rxpipe_t *p = ctx;
    int16_t t[960];
    while (n > 0) {
        int k = n > 960 ? 960 : n;
        if (p->rx_af) {
            memcpy(t, x, (size_t)k * 2);
        } else {
            fmp_ifdem_run(&p->ifd, x, t, k);
            /* the demodulator refuses a sync when the DC is over 3x the signal: with a low deviation the offset
             * between the radios alone did that. tau 0.1 s (1.6 Hz) is far below what M17 needs. */
            for (int i = 0; i < k; ++i) {
                p->dc += (t[i] - p->dc) / 4800.0;
                double v = (t[i] - p->dc) * s_rx_gain;
                t[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        bb_keep(t, k);
        if (p->use_eq) {
            teq_run(&p->eq, t, k);
        }
        m17_demod_push(&p->dem, t, k);
        x += k;
        n -= k;
    }
}

static void rx_summary(void)
{
    if (s_bb_path && s_bb_n) {
        write_wav(s_bb_path, s_bb, s_bb_n);
    }
    rx_end(&s_rx, "");
    printf("\nsummary (B1 = no precompensation, B2 = guess, B3 = your settings, B4 = B3 + equalizer):\n");
    for (int b = 1; b <= 9; ++b) {
        typeof(s_rx.b[0]) *r = &s_rx.b[b];
        if (!r->lsf_ok && !r->frames) {
            continue;
        }
        printf("  B%d: LSF %d, frames %d, frames with errors %d, payload bit errors %d (BER %.2e), EVM avg %.3f\n", b,
               r->lsf_ok, r->frames, r->bad_frames, r->bits, r->frames ? r->bits / (128.0 * r->frames) : 0,
               r->frames ? r->evm_sum / r->frames : 0);
    }
    if (!s_rx.nb) {
        printf("  nothing decoded\n");
    }
    printf("  (a burst that is not listed was not decoded at all; B1 is expected to fail on a radio with a high-pass)\n");
}

static rxpipe_t *rx_pipe(bool rx_af, const double *eq)
{
    static rxpipe_t p;
    memset(&p, 0, sizeof(p));
    memset(&s_rx, 0, sizeof(s_rx));
    s_rx.cur = -1;
    p.rx_af = rx_af;
    m17_demod_init(&p.dem, on_frame, &s_rx);
    fmp_ifdem_init(&p.ifd, FMP_IF_HZ);
    if (eq) {
        p.use_eq = true;
        teq_init(&p.eq, eq);
    }
    return &p;
}

/* the receiver's equalizer for this run (--eq), or NULL; with --train-eq the first pass runs without one */
static const double *rx_eq(const opt_t *o)
{
    static double w[TEQ_TAPS];
    if (!o->eq[0] || o->train_eq[0]) {
        return NULL;
    }
    if (!teq_load(o->eq, w)) {
        fprintf(stderr, "cannot read the equalizer %s: receiving without it\n", o->eq);
        return NULL;
    }
    printf("receiver equalizer: %s\n", o->eq);
    return w;
}

/* --train-eq: learn from the burst --train-burst in the baseband kept by the first pass, save, decode again with it */
static void train_and_redo(const opt_t *o, const int16_t *x, int n)
{
    if (!o->call[0]) {
        printf("[!] --train-eq needs --call (the transmitter's callsign: it is part of the burst)\n");
        return;
    }
    int nref;
    int16_t *ref = make_burst(o->call, o->train_burst, o->frames, -10, NULL, 0, 1, &nref);
    double w[TEQ_TAPS];
    teq_report_t rp;
    printf("\n=== learning the equalizer from B%d (%s, %d frames)\n", o->train_burst, o->call, o->frames);
    bool ok = teq_train(s_bb, s_bb_n, ref, nref, w, &rp);
    free(ref);
    printf("  burst found at %.2f s (match %.2f), clock difference %+.1f ppm\n", rp.pos_s, rp.peak, rp.drift_ppm);
    if (!ok) {
        printf("[!] EQ: B%d was not found in the recording (match below 0.2): no equalizer saved\n", o->train_burst);
        return;
    }
    printf("  error: gain only %.1f dB, with the equalizer %.1f dB (learned on half, other half: %.1f dB)\n",
           rp.nmse_gain_db, rp.nmse_eq_db, rp.nmse_half_db);
    teq_print_response(w);
    char note[160];
    snprintf(note, sizeof(note), "from B%d, error %.1f dB -> %.1f dB", o->train_burst, rp.nmse_gain_db, rp.nmse_eq_db);
    if (!teq_save(o->train_eq, w, note)) {
        printf("[!] cannot write %s\n", o->train_eq);
        return;
    }
    printf("equalizer saved: %s\n", o->train_eq);
    printf("\n=== the same recording again, with the equalizer\n");
    s_bb_on = false;
    rxpipe_t *p = rx_pipe(o->rx_af, w);
    for (int i = 0; i < n; i += 960) {
        rx_block(p, x + i, n - i < 960 ? n - i : 960);
    }
    rx_summary();
}

/* B4 = the B3 burst (your precompensation) through the equalizer txw (0 dB at 1 kHz).
 * 2026-11-18: the equalizer lifts the lows and 3.2 kHz, and B4 clipped 62k samples. So the burst is built 20 dB below
 * --level (the precompensation cannot clip), the peak after the equalizer is measured, and the burst is scaled to
 * -1 dBFS peak, at most back to --level. The receiver follows the level. */
static int16_t *make_b4(const opt_t *o, const fmp_radio_t *r, const double *txw, int *n_out)
{
    int n;
    int16_t *x = make_burst(o->call, 4, o->frames, o->level - 20, r, o->floor_hz, o->pre_gain, &n);
    int m = n + TEQ_TAPS * TEQ_STRIDE / 2;             /* the equalizer delays by half its length: let it ring out */
    int16_t *y = calloc((size_t)m, 2);
    /* high-pass 25 Hz (2nd order Butterworth) first: below 20 Hz the precompensation lifts a lot, the equalizer adds
     * +13 dB more (the receiver's DC block never let it see those), and the radio throws it all away anyway.
     * That was what used the headroom (B4 at full scale even 20 dB low). */
    double w0 = tan(M_PI * 25.0 / FS), k1 = M_SQRT2 * w0, k2 = w0 * w0, a0 = 1 + k1 + k2;
    double b0 = 1 / a0, b1 = -2 / a0, b2 = 1 / a0, a1 = 2 * (k2 - 1) / a0, a2 = (1 - k1 + k2) / a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; ++i) {
        double v = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x[i]; y2 = y1; y1 = v;
        y[i] = (int16_t)lrint(v);
    }
    free(x);
    teq_t q;
    teq_init(&q, txw);
    teq_run(&q, y, m);
    int pk = 1;
    for (int i = 0; i < m; ++i) {
        pk = abs(y[i]) > pk ? abs(y[i]) : pk;
    }
    double sc = pk < 32767 ? 0.89 * 32767.0 / pk : 1;  /* (a peak at full scale: it clipped already) */
    if (sc > 10) {
        sc = 10;
    }
    int clip = 0;
    for (int i = 0; i < m; ++i) {
        double v = y[i] * sc;
        clip += v > 32767 || v < -32768;
        y[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
    }
    if (sc < 10) {
        printf("  B4 is %.1f dB below --level so the equalizer does not clip (raise USB MOD Level to get it back)\n",
               -20 * log10(sc / 10));
    }
    if (clip) {
        printf("  [W] burst 4: the equalizer clipped %d samples (lower --level)\n", clip);
    }
    *n_out = m;
    return y;
}

static int cmd_m17_tx(const opt_t *o)
{
    if (!o->call[0]) {
        fprintf(stderr, "--call CALLSIGN is needed (the source of the test bursts)\n");
        return 2;
    }
    fmp_radio_t guess;
    fmp_radio_ic705_guess(&guess);
    struct {
        const fmp_radio_t *r;
        double floor_hz, gain;
        const char *what;
    } plan[3] = { { NULL, 0, 0, "no precompensation" },
                  { &guess, 50, -36, "precompensation, guess (HP 300 Hz order 2, no pre-emphasis, 50 Hz, -36 dB)" },
                  { &o->radio, o->floor_hz, o->pre_gain, "precompensation, your settings" } };
    int nb = o->b3 ? 3 : 2;
    double txw[TEQ_TAPS];
    if (o->tx_eq[0]) {
        if (!o->b3) {
            printf("[!] --tx-eq needs the B3 settings (--radio-hp ...): no B4\n");
        } else if (!teq_load(o->tx_eq, txw)) {
            printf("[!] cannot read the equalizer %s: no B4\n", o->tx_eq);
        } else {
            teq_unity_1k(txw);
            nb = 4;
        }
    }
    printf("M17 test bursts from %s, %d frames (%.1f s) each, +3 symbols at %.1f dBFS before precompensation:\n",
           o->call, o->frames, o->frames * 0.04, o->level);
    for (int b = 0; b < nb; ++b) {
        if (b == 3) {
            printf("  B4: B3 + the equalizer %s (0 dB at 1 kHz)\n", o->tx_eq);
            continue;
        }
        printf("  B%d: %s", b + 1, plan[b].what);
        if (b == 2) {
            printf(" (HP %.0f Hz order %d, pre-emphasis %.0f us, floor %.0f Hz, %.0f dB)", o->radio.hp_hz,
                   o->radio.hp_order, o->radio.preemph_us, o->floor_hz, o->pre_gain);
        }
        printf("\n");
    }
    printf("start m17-rx on the other radio first. Transmitting in 3 s (Ctrl+C stops and unkeys)...\n");
    Sleep(3000);
    for (int b = 0; b < nb && !s_stop; ++b) {
        int n;
        int16_t *x = b == 3 ? make_b4(o, &o->radio, txw, &n)
                            : make_burst(o->call, b + 1, o->frames, o->level, plan[b].r, plan[b].floor_hz, plan[b].gain, &n);
        printf("B%d ...\n", b + 1);
        transmit(o, x, n);
        free(x);
        Sleep(1500);
    }
    printf(s_stop ? "stopped, unkeyed\n" : "done, unkeyed\n");
    return 0;
}

/* m17-ref: the bursts as m17-tx builds them, before any precompensation (for channel estimation offline) */
static int cmd_m17_ref(const opt_t *o)
{
    for (int b = 1; b <= 3; ++b) {
        int n;
        int16_t *x = make_burst(o->call[0] ? o->call : "N0CALL", b, o->frames, -10, NULL, 0, 1, &n);
        char path[64];
        snprintf(path, sizeof(path), "ref_B%d.wav", b);
        write_wav(path, x, n);
        printf("wrote %s (%.2f s, +3 symbols at -10 dBFS)\n", path, (double)n / FS);
        free(x);
    }
    double txw[TEQ_TAPS];
    if (o->tx_eq[0] && o->b3 && teq_load(o->tx_eq, txw)) {          /* the B4 audio exactly as m17-tx sends it */
        teq_unity_1k(txw);
        int n;
        int16_t *x = make_b4(o, &o->radio, txw, &n);
        write_wav("tx_B4.wav", x, n);
        printf("wrote tx_B4.wav (B4 as sent, --level %.1f)\n", o->level);
        free(x);
    }
    return 0;
}

static int cmd_m17_rx(const opt_t *o)
{
    printf("receiving %.0f s from input %d (%s): start m17-tx on the other radio (Ctrl+C ends early)\n", o->sec, o->in,
           o->rx_af ? "audio" : "12 kHz IF");
    s_bb_on = s_bb_path || o->train_eq[0];
    rxpipe_t *p = rx_pipe(o->rx_af, rx_eq(o));
    int n = 0;
    int16_t *x = record(o, o->sec, &n, rx_block, p);
    if (!x) {
        return 1;
    }
    rx_summary();
    write_wav(o->wav[0] ? o->wav : "m17_rx.wav", x, n);
    if (o->train_eq[0]) {
        train_and_redo(o, x, n);
    }
    free(x);
    return 0;
}

static int cmd_analyze(const char *what, const char *path, const opt_t *o)
{
    int n = 0;
    int16_t *x = read_wav(path, &n);
    if (!x) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    int r = 0;
    if (!strcmp(what, "tones")) {
        r = analyze_tones(x, n, o->rx_af, o->level);
    } else {
        s_bb_on = s_bb_path || o->train_eq[0];
        rxpipe_t *p = rx_pipe(o->rx_af, rx_eq(o));
        for (int i = 0; i < n; i += 960) {
            rx_block(p, x + i, n - i < 960 ? n - i : 960);
        }
        rx_summary();
        if (o->train_eq[0]) {
            train_and_redo(o, x, n);
        }
    }
    free(x);
    return r;
}

/* ---------------------------------------------------------------- self test through the radio model */
static int16_t *through_model(const int16_t *x, int n, const fmp_radio_t *truth, double in_dbfs_for_2400, bool rx_if,
                              double cnr, double offset_hz)
{
    fmp_model_t m;
    fmp_model_init(&m, truth, 1.5, rx_if, 1.0 / (32767.0 * pow(10, in_dbfs_for_2400 / 20)));
    m.rx_offset_hz = offset_hz;
    int16_t *y = malloc((size_t)n * 2);
    unsigned seed = 99;
    double nr = cnr < 90 ? (rx_if ? 8000.0 / sqrt(2.0) : FMP_NOMINAL * 0.6) * pow(10, -cnr / 20) : 0;
    fmp_model_run(&m, x, NULL, y, n, nr, &seed);
    return y;
}

static int cmd_selftest(opt_t *o)
{
    fmp_radio_t truth;
    fmp_radio_ic705_guess(&truth);
    truth.hp_hz = 250;                 /* a "radio" a bit different from the guess, to see the fit find it */
    truth.hp_order = 2;
    truth.preemph_us = 0;
    truth.lp_hz = 3000;
    double cal = -20;                  /* this radio: 2400 Hz deviation for a -20 dBFS 1 kHz tone peak */
    printf("=== self test: tones through a model radio (HP %.0f Hz order %d, no pre-emphasis, LP %.0f Hz), IF, CNR 30 dB\n",
           truth.hp_hz, truth.hp_order, truth.lp_hz);
    int n;
    o->level = -30;
    int16_t *x = make_tones(o->level, &n);
    int pad = FS;                      /* 1 s of nothing first, like a recording started early */
    int16_t *xx = calloc((size_t)(n + 2 * pad), 2);
    memcpy(xx + pad, x, (size_t)n * 2);
    int16_t *y = through_model(xx, n + 2 * pad, &truth, cal, true, 30, 150);
    analyze_tones(y, n + 2 * pad, false, o->level);
    free(x);
    free(xx);
    free(y);

    printf("\n=== self test: M17 bursts through the same model radio, IF, CNR 20 dB\n");
    fmp_radio_t guess, mine = truth;
    fmp_radio_ic705_guess(&guess);
    struct {
        const fmp_radio_t *r;
        double fl, g;
    } plan[3] = { { NULL, 0, 0 }, { &guess, 50, -36 }, { &mine, 50, -36 } };
    rxpipe_t *p = rx_pipe(false, NULL);
    for (int b = 0; b < 3; ++b) {
        int nb;
        int16_t *bx = make_burst("N0CALL", b + 1, 60, -10, plan[b].r, plan[b].fl, plan[b].g, &nb);
        /* the radio's MOD level set so that the burst's +3 symbols give 2400 Hz */
        double lv = -10 + (plan[b].r ? plan[b].g : 0);
        int16_t *by = through_model(bx, nb, &truth, lv, true, 20, 150);
        for (int i = 0; i < nb; i += 960) {
            rx_block(p, by + i, nb - i < 960 ? nb - i : 960);
        }
        int16_t z[960] = { 0 };
        int16_t *zz = through_model(z, 960, &truth, lv, true, 20, 150);
        for (int i = 0; i < 20; ++i) {
            rx_block(p, zz, 960);
        }
        free(zz);
        free(bx);
        free(by);
    }
    rx_summary();
    return 0;
}

/* ---------------------------------------------------------------- main */
static void usage(void)
{
    printf("%s\n"
           "  list | civ COMx | ifcheck --in N | tones-tx --out N --ptt COMx | tones-rx --in N\n"
           "  m17-tx --out N --ptt COMx --call CALL | m17-rx --in N | analyze tones|m17 FILE.wav | selftest\n"
           "  options: --baud 115200 --addr A4 --level dBFS --sec S --wav FILE --rx-af --tx-delay MS --max-tx S\n"
           "           --frames N --radio-hp HZ --radio-hp-order N --radio-preemph US --pre-floor HZ --pre-gain DB\n"
           "  Only into dummy loads, lowest power, within the licence.\n",
           VERSION);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);              /* show progress at once when the output is a pipe (taikou-gui) */
    m17_rf_init();
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    atexit(at_exit);
    const char *stop_ev = getenv("TAIKOU_STOP_EVENT");
    if (stop_ev && *stop_ev) {
        HANDLE h = OpenEventA(SYNCHRONIZE, FALSE, stop_ev);
        if (h) {
            CloseHandle(CreateThread(NULL, 0, stop_watch, h, 0, NULL));
        }
    }
    opt_t o;
    memset(&o, 0, sizeof(o));
    o.in = o.out = -1;
    o.baud = 115200;
    o.addr = 0xA4;
    o.tx_delay_ms = 300;
    o.max_tx_s = 45;
    o.level = 999;
    o.sec = 60;
    o.frames = 100;
    o.train_burst = 3;
    fmp_radio_ic705_guess(&o.radio);
    o.floor_hz = 50;
    o.pre_gain = -36;
    const char *cmd = argv[1];
    int i0 = 2;
    const char *a1 = NULL, *a2 = NULL;
    if (!strcmp(cmd, "civ") && argc > 2) {
        snprintf(o.port, sizeof(o.port), "%s", argv[2]);
        i0 = 3;
    }
    if (!strcmp(cmd, "analyze") && argc > 3) {
        a1 = argv[2];
        a2 = argv[3];
        i0 = 4;
    }
    for (int i = i0; i < argc; ++i) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--in")) { o.in = atoi(v); ++i; }
        else if (!strcmp(a, "--out")) { o.out = atoi(v); ++i; }
        else if (!strcmp(a, "--ptt")) { snprintf(o.port, sizeof(o.port), "%s", v); ++i; }
        else if (!strcmp(a, "--baud")) { o.baud = (unsigned)atoi(v); ++i; }
        else if (!strcmp(a, "--addr")) { o.addr = (unsigned)strtoul(v, NULL, 16); ++i; }
        else if (!strcmp(a, "--level")) { o.level = atof(v); ++i; }
        else if (!strcmp(a, "--sec")) { o.sec = atof(v); ++i; }
        else if (!strcmp(a, "--wav")) { snprintf(o.wav, sizeof(o.wav), "%s", v); ++i; }
        else if (!strcmp(a, "--call")) { snprintf(o.call, sizeof(o.call), "%s", v); ++i; }
        else if (!strcmp(a, "--rx-af")) { o.rx_af = true; }
        else if (!strcmp(a, "--bb")) { s_bb_path = v; ++i; }
        else if (!strcmp(a, "--eq")) { snprintf(o.eq, sizeof(o.eq), "%s", v); ++i; }
        else if (!strcmp(a, "--train-eq")) { snprintf(o.train_eq, sizeof(o.train_eq), "%s", v); ++i; }
        else if (!strcmp(a, "--tx-eq")) { snprintf(o.tx_eq, sizeof(o.tx_eq), "%s", v); ++i; }
        else if (!strcmp(a, "--train-burst")) { o.train_burst = atoi(v); ++i; }
        else if (!strcmp(a, "--rx-gain")) { s_rx_gain = pow(10, atof(v) / 20); ++i; }
        else if (!strcmp(a, "--tx-delay")) { o.tx_delay_ms = (unsigned)atoi(v); ++i; }
        else if (!strcmp(a, "--max-tx")) { o.max_tx_s = atof(v); ++i; }
        else if (!strcmp(a, "--frames")) { o.frames = atoi(v); ++i; }
        else if (!strcmp(a, "--radio-hp")) { o.radio.hp_hz = atof(v); o.b3 = true; ++i; }
        else if (!strcmp(a, "--radio-hp-order")) { o.radio.hp_order = atoi(v); o.b3 = true; ++i; }
        else if (!strcmp(a, "--radio-preemph")) { o.radio.preemph_us = atof(v); o.b3 = true; ++i; }
        else if (!strcmp(a, "--pre-floor")) { o.floor_hz = atof(v); o.b3 = true; ++i; }
        else if (!strcmp(a, "--pre-gain")) { o.pre_gain = atof(v); o.b3 = true; ++i; }
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }
    s_addr = o.addr;
    for (char *p = o.call; *p; ++p) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 32);
        }
    }
    printf("%s\n", VERSION);
    bool needs_in = !strcmp(cmd, "ifcheck") || !strcmp(cmd, "tones-rx") || !strcmp(cmd, "m17-rx");
    bool needs_tx = !strcmp(cmd, "tones-tx") || !strcmp(cmd, "m17-tx");
    if (needs_in && o.in < 0) {
        fprintf(stderr, "--in N is needed (see list)\n");
        return 2;
    }
    if (needs_tx && getenv("TAIKOU_NO_RADIO")) {     /* developer check of the sound output only: no PTT */
        if (o.out < 0) {
            fprintf(stderr, "--out N is needed\n");
            return 2;
        }
        printf("TAIKOU_NO_RADIO: sound only, no CI-V, no PTT\n");
    } else if (needs_tx) {
        if (o.out < 0 || !o.port[0]) {
            fprintf(stderr, "--out N and --ptt COMx are needed (see list)\n");
            return 2;
        }
        if (!civ_open(o.port, o.baud)) {
            return 1;
        }
        uint8_t b[8], c03[1] = { 0x03 };
        if (civ_query(c03, 1, b, sizeof(b)) < 5) {
            fprintf(stderr, "no CI-V answer on %s (address %02X, %u bps): not transmitting\n", o.port, o.addr, o.baud);
            return 1;
        }
        uint8_t c04[1] = { 0x04 }, c1a06[2] = { 0x1A, 0x06 };   /* the test is for FM-D only */
        int nm = civ_query(c04, 1, b, sizeof(b));
        bool fm = nm >= 1 && b[0] == 0x05;
        int nd = civ_query(c1a06, 2, b, sizeof(b));
        bool data = nd >= 1 && b[0];
        if (!fm || !data) {
            fprintf(stderr, "the radio on %s is not in FM-D (%s%s): press FM, then DATA. not transmitting\n", o.port,
                    fm ? "FM" : "not FM", data ? ", data ON" : ", data off");
            return 1;
        }
    }
    if (o.level > 900) {
        o.level = !strcmp(cmd, "m17-tx") ? -10 : -20;
    }
    if (!strcmp(cmd, "list")) return cmd_list();
    if (!strcmp(cmd, "civ")) return cmd_civ(&o);
    if (!strcmp(cmd, "ifcheck")) { if (o.sec > 30) o.sec = 10; return cmd_ifcheck(&o); }
    if (!strcmp(cmd, "tones-tx")) return cmd_tones_tx(&o);
    if (!strcmp(cmd, "tones-rx")) return cmd_tones_rx(&o);
    if (!strcmp(cmd, "m17-tx")) return cmd_m17_tx(&o);
    if (!strcmp(cmd, "m17-rx")) return cmd_m17_rx(&o);
    if (!strcmp(cmd, "m17-ref")) return cmd_m17_ref(&o);
    if (!strcmp(cmd, "analyze") && a1) return cmd_analyze(a1, a2, &o);
    if (!strcmp(cmd, "selftest")) return cmd_selftest(&o);
    usage();
    return 2;
}
