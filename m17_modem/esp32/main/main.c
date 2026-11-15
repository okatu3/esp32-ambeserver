/* m17_modem on the ESP32-A1S audio kit (ES8388): an M17 voice modem for an FM radio's data port.
 *
 *   board mic (right ADC, RIN1) -> 8 kHz -> Codec 2 3200 -> M17 frames -> 4FSK 48 kHz -> left DAC (LOUT1 = PHONE tip)
 *   LINE IN tip (left ADC, LIN2) -> M17 demodulator -> frames -> Codec 2 -> 8 kHz -> right DAC (PHONE ring, speaker right)
 *
 * Transmit and receive run at the same time: with a cable from the headphone jack to the line-in jack the board hears
 * its own transmission (loopback test). PTT: KEY1 (GPIO36, hold) or the serial command "t". Serial commands (115200):
 *   t            toggle transmit              tx N     transmit N seconds (mic), compare what comes back
 *   c CALL       set the callsign (kept)      s        status
 *   lvl DB       TX level of +-3 symbols, dBFS (default -10)    inv   invert the TX polarity
 *   mic DB       mic gain 0..24 dB            vol N    voice volume 0..33 (30 = 0 dB)
 * Front panel (ui.c): a menu for a rotary encoder and a 21 x 8 character screen; until they are fitted, the serial
 * console drives it: r / l (turn), p (push), b (long push), "ui" prints the screen, "ui auto" on every change.
 * Tasks: audio (core 0: I2S, resampling, modulator output, demodulator), codec (core 1: Codec 2, framing), ui. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "codec2.h"
#include "es8388.h"
#include "m17.h"
#include "m17_modem.h"
#include "m17_rf.h"
#include "ui.h"

static const char *TAG = "m17";

#define BLK        480          /* 10 ms at 48 kHz */
#define BLK8       80           /* 10 ms at 8 kHz */
#define KEY_PTT    36           /* KEY1 of the audio kit, active low */
/* the key GPIOs of the ESP32-A1S audio kit (KEY1..KEY6 on the v2.2 board; versions differ), for "keys" */
static const int KEY_PINS[6] = { 36, 13, 19, 23, 18, 5 };
/* I2S words and codec channels, measured with a stereo cable from PHONE to LINE IN (2026-11-14, same on every
 * boot): the ESP32 swaps the two 16-bit words of a stereo frame both ways, so output word 0 -> right DAC,
 * word 1 -> left DAC; input word 0 <- left ADC, word 1 <- right ADC. The radio uses the tips only (a tip always
 * makes contact; the ring of a half-inserted plug may not): -20 dBFS out came back at -26 dBFS. */
#define OUT_MODEM  1            /* left DAC: LOUT1 = PHONE tip (to the radio); LOUT2 (speaker left) is off */
#define OUT_VOICE  0            /* right DAC: ROUT1 = PHONE ring, ROUT2 = speaker right */
#define IN_MIC     1            /* right ADC: RIN1 = board mic */
#define IN_LINE    0            /* left ADC: LIN2 = LINE IN tip (from the radio) */

/* ------------------------------------------------------------------ settings */
static char s_call[10] = "N0CALL";
static float s_tx_dbfs = -10.0f;
static bool s_tx_invert;
static int s_mic_db = 24, s_vol = 30;
static int s_can;                                 /* channel access number 0..15 (in the LSF type) */
static ui_mode_t s_mode = UI_MODE_DIRECT;
static char s_refl[256] = "M17-M17";              /* stored reflectors, comma separated */
static char s_relay_call[10];                     /* the relay that answers our commands (learned) */

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("m17", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_call);
        nvs_get_str(h, "call", s_call, &n);
        int32_t v;
        if (nvs_get_i32(h, "lvl10", &v) == ESP_OK) {
            s_tx_dbfs = v / 10.0f;
        }
        if (nvs_get_i32(h, "mic", &v) == ESP_OK) {
            s_mic_db = (int)v;
        }
        if (nvs_get_i32(h, "vol", &v) == ESP_OK) {
            s_vol = (int)v;
        }
        if (nvs_get_i32(h, "can", &v) == ESP_OK) {
            s_can = (int)v & 15;
        }
        if (nvs_get_i32(h, "mode", &v) == ESP_OK) {
            s_mode = (ui_mode_t)(v % 3);
        }
        n = sizeof(s_refl);
        nvs_get_str(h, "refl", s_refl, &n);
        n = sizeof(s_relay_call);
        nvs_get_str(h, "relay", s_relay_call, &n);
        nvs_close(h);
    }
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("m17", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "call", s_call);
        nvs_set_i32(h, "lvl10", (int32_t)(s_tx_dbfs * 10.0f));
        nvs_set_i32(h, "mic", s_mic_db);
        nvs_set_i32(h, "vol", s_vol);
        nvs_set_i32(h, "can", s_can);
        nvs_set_i32(h, "mode", (int32_t)s_mode);
        nvs_set_str(h, "refl", s_refl);
        nvs_set_str(h, "relay", s_relay_call);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ------------------------------------------------------------------ single-producer single-consumer rings */
typedef struct {
    int16_t *b;
    uint32_t size;
    volatile uint32_t w, r;
} ring_t;

static void ring_init(ring_t *q, uint32_t size)
{
    q->b = heap_caps_malloc(size * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!q->b) {
        q->b = malloc(size * sizeof(int16_t));
    }
    q->size = size;
    q->w = q->r = 0;
}

static uint32_t ring_count(const ring_t *q) { return q->w - q->r; }
static uint32_t ring_space(const ring_t *q) { return q->size - (q->w - q->r); }

static void ring_put(ring_t *q, const int16_t *x, uint32_t n)
{
    for (uint32_t i = 0; i < n && ring_space(q); ++i) {
        q->b[q->w % q->size] = x[i];
        q->w++;
    }
}

static uint32_t ring_get(ring_t *q, int16_t *x, uint32_t n)
{
    uint32_t k = 0;
    while (k < n && ring_count(q)) {
        x[k++] = q->b[q->r % q->size];
        q->r++;
    }
    return k;
}

static void ring_clear(ring_t *q) { q->r = q->w; }

static ring_t s_mic8, s_spk8, s_txbb, s_line48;

/* ------------------------------------------------------------------ 48 kHz <-> 8 kHz (61-tap low-pass at 3.6 kHz) */
#define LP_TAPS 61
static float s_lp[LP_TAPS];

static void lp_init(void)
{
    double sum = 0;
    for (int i = 0; i < LP_TAPS; ++i) {
        double t = i - (LP_TAPS - 1) / 2.0, fc = 3600.0 / 48000.0;
        double v = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        v *= 0.54 - 0.46 * cos(2 * M_PI * i / (LP_TAPS - 1));
        s_lp[i] = (float)v;
        sum += v;
    }
    for (int i = 0; i < LP_TAPS; ++i) {
        s_lp[i] = (float)(s_lp[i] / sum);
    }
}

typedef struct { float h[LP_TAPS]; int pos, phase; } decim_t;
typedef struct { float h[11]; int pos; } interp_t;   /* 8 kHz history: 61 taps / 6 phases -> 11 */

static int decimate(decim_t *d, const int16_t *x, int stride, int n, int16_t *out)
{
    int k = 0;
    for (int i = 0; i < n; ++i) {
        d->h[d->pos] = x[i * stride];
        d->pos = (d->pos + 1) % LP_TAPS;
        if (++d->phase == 6) {
            d->phase = 0;
            float a = 0;
            int p = d->pos;
            for (int t = 0; t < LP_TAPS; ++t) {
                p = p ? p - 1 : LP_TAPS - 1;
                a += s_lp[t] * d->h[p];
            }
            out[k++] = (int16_t)(a > 32767 ? 32767 : (a < -32768 ? -32768 : a));
        }
    }
    return k;
}

static void interpolate(interp_t *s, const int16_t *x, int n, int16_t *out, int stride)
{
    int k = 0;
    for (int i = 0; i < n; ++i) {
        memmove(s->h + 1, s->h, sizeof(float) * 10);
        s->h[0] = x[i];
        for (int j = 0; j < 6; ++j) {
            float a = 0;
            for (int m = 0; m < 11; ++m) {
                int t = j + 6 * m;
                if (t < LP_TAPS) {
                    a += s_lp[t] * s->h[m];
                }
            }
            a *= 6;
            out[(k++) * stride] = (int16_t)(a > 32767 ? 32767 : (a < -32768 ? -32768 : a));
        }
    }
}

/* ------------------------------------------------------------------ shared state */
typedef struct {
    m17_sync_info_t info;
    uint16_t soft[M17_PAYLOAD_BITS];
    bool has_soft;
} frame_msg_t;

static QueueHandle_t s_frames;
static m17_demod_t s_demod;
static volatile bool s_ptt_req, s_tx_active, s_dig_loop, s_tx_tone, s_tx_silent;
/* destination of our voice calls, and of a command being sent */
static char s_dst[10] = "@ALL", s_dst_prev[10] = "@ALL", s_cmd_dst[10];
static volatile bool s_dst_once;
/* answers of the relay, stations heard */
static char s_relay_state[10];
static int64_t s_relay_us, s_cmd_us;
static volatile bool s_cmd_pending;
static char s_heard[UI_HEARD_N][10];
static int64_t s_heard_us[UI_HEARD_N];
static int s_heard_n;
static float s_rx_evm;
static volatile int s_diag_ch;                    /* 0 off, 1 tone on left, 2 tone on right */
static int16_t *s_cap_buf;                        /* "cap": line-in samples for the PC (PSRAM) */
static volatile uint32_t s_cap_n, s_cap_len;
static volatile uint32_t s_audio_us_max;          /* longest audio-task block (us) since the last status */
static volatile float s_mic_dbfs = -99, s_line_dbfs = -99;
static volatile uint32_t s_frames_dropped;
static i2s_chan_handle_t s_i2s_tx, s_i2s_rx;

static void on_demod_frame(void *ctx, const m17_sync_info_t *info, const uint16_t *soft)
{
    frame_msg_t m;
    m.info = *info;
    m.has_soft = soft != NULL;
    if (soft) {
        memcpy(m.soft, soft, sizeof(m.soft));
    }
    if (xQueueSend(s_frames, &m, 0) != pdTRUE) {
        s_frames_dropped++;
    }
}

static float dbfs_of(const int16_t *x, int n, int stride)
{
    float e = 0;
    for (int i = 0; i < n; ++i) {
        float v = x[i * stride];
        e += v * v;
    }
    e /= (float)n;
    return e < 1 ? -99.0f : 10.0f * log10f(e * (1.0f / (32768.0f * 32768.0f)));
}

/* ------------------------------------------------------------------ demodulator task (core 0, below the audio task) */
static volatile uint32_t s_demod_backlog_max;    /* most line-in samples waiting (48 kHz) */

static void demod_task(void *arg)
{
    static int16_t x[BLK];
    for (;;) {
        uint32_t c = ring_count(&s_line48);
        if (c > s_demod_backlog_max) {
            s_demod_backlog_max = c;
        }
        uint32_t n = ring_get(&s_line48, x, BLK);
        if (n) {
            m17_demod_push(&s_demod, x, (int)n);
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

/* ------------------------------------------------------------------ audio task (core 0) */
static void audio_task(void *arg)
{
    static int16_t in[BLK * 2], out[BLK * 2], line[BLK], mic8[BLK8 + 2], spk8[BLK8], bb[BLK];
    static decim_t dec;
    static interp_t itp;
    for (;;) {
        size_t got = 0;
        i2s_channel_read(s_i2s_rx, in, sizeof(in), &got, portMAX_DELAY);
        int64_t t_start = esp_timer_get_time();
        int n = (int)(got / 4);
        /* left: mic -> 8 kHz ring; right: line in -> demodulator */
        int k = decimate(&dec, in + IN_MIC, 2, n, mic8);
        ring_put(&s_mic8, mic8, (uint32_t)k);
        for (int i = 0; i < n; ++i) {
            line[i] = s_dig_loop ? out[2 * i + OUT_MODEM] : in[2 * i + IN_LINE];   /* digital loopback: last block */
        }
        ring_put(&s_line48, line, (uint32_t)n);       /* the demodulator task takes it from here */
        if (s_cap_buf && s_cap_n < s_cap_len) {       /* "cap": record the line input for the PC */
            for (int i = 0; i < n && s_cap_n < s_cap_len; ++i) {
                s_cap_buf[s_cap_n++] = line[i];
            }
        }
        s_mic_dbfs = 0.9f * s_mic_dbfs + 0.1f * dbfs_of(in + IN_MIC, n, 2);
        s_line_dbfs = 0.9f * s_line_dbfs + 0.1f * dbfs_of(in + IN_LINE, n, 2);
        /* received voice and modulator to their output words */
        uint32_t g = ring_get(&s_spk8, spk8, BLK8);
        if (g < BLK8) {
            memset(spk8 + g, 0, (BLK8 - g) * sizeof(int16_t));
        }
        interpolate(&itp, spk8, BLK8, out + OUT_VOICE, 2);
        g = ring_get(&s_txbb, bb, (uint32_t)n);
        if (g < (uint32_t)n) {
            memset(bb + g, 0, ((uint32_t)n - g) * sizeof(int16_t));
        }
        for (int i = 0; i < n; ++i) {
            out[2 * i + OUT_MODEM] = s_tx_invert ? (int16_t)(-bb[i]) : bb[i];
        }
        if (s_diag_ch) {                              /* diagnostic: 1 kHz at -20 dBFS on one channel only */
            static float ph;
            for (int i = 0; i < n; ++i) {
                int16_t v = (int16_t)(3277.0f * sinf(ph));
                ph += 2.0f * (float)M_PI * 1000.0f / 48000.0f;
                if (ph > 2.0f * (float)M_PI) {
                    ph -= 2.0f * (float)M_PI;
                }
                out[2 * i + OUT_VOICE] = s_diag_ch == 1 ? v : 0;
                out[2 * i + OUT_MODEM] = s_diag_ch == 2 ? v : 0;
            }
        }
        uint32_t us = (uint32_t)(esp_timer_get_time() - t_start);
        if (us > s_audio_us_max) {
            s_audio_us_max = us;
        }
        size_t w;
        i2s_channel_write(s_i2s_tx, out, (size_t)n * 4, &w, portMAX_DELAY);
    }
}

/* ------------------------------------------------------------------ codec task (core 1): TX and RX framing */
static struct CODEC2 *s_enc, *s_dec32, *s_dec16;
static m17_mod_t s_mod;
static uint8_t s_lsf[M17_LSF_BYTES];
static uint16_t s_fn;
static int s_tx_test_frames = -1;                 /* tx N: frames left, -1 = PTT mode */
static uint8_t s_tx_hist[64][16];                 /* payloads sent, by fn % 64, for the loopback compare */
static uint32_t s_tx_frames_total;
static int64_t s_tx_end_us;                       /* when our last transmission ended */
static volatile uint32_t s_enc_us_max;            /* longest pair of Codec 2 encodes (40 ms of voice) */

static struct {
    bool active, lsf_ok, voice1600, self;   /* self: started during our own transmission (loopback) */
    char src[10], dst[10];
    uint16_t type;
    uint32_t frames, golay_err, bad_lich, cmp_frames, cmp_bad, cmp_bits;
    double cost;
    float evm_max;
    int64_t t0;
} R;

static void build_lsf(void)
{
    memset(s_lsf, 0, sizeof(s_lsf));
    m17_encode_call(s_cmd_dst[0] ? s_cmd_dst : s_dst, s_lsf);
    m17_encode_call(s_call, s_lsf + 6);
    uint16_t type = (uint16_t)(M17_TYPE_V3200 | (s_can & 15) << 7);
    s_lsf[12] = (uint8_t)(type >> 8);
    s_lsf[13] = (uint8_t)type;
    uint16_t crc = m17_crc(s_lsf, 28);
    s_lsf[28] = (uint8_t)(crc >> 8);
    s_lsf[29] = (uint8_t)crc;
}

static void tx_symbols(const int8_t *sym, int n)
{
    static int16_t bb[M17_SYMS_PER_FRAME * M17_SPS];
    int k = m17_mod(&s_mod, sym, n, bb);
    ring_put(&s_txbb, bb, (uint32_t)k);
}

static void tx_start(void)
{
    build_lsf();
    m17_mod_init(&s_mod, 32767.0f * powf(10.0f, s_tx_dbfs / 20.0f));
    ring_clear(&s_mic8);
    s_fn = 0;
    int8_t sym[M17_SYMS_PER_FRAME];
    m17_rf_preamble(sym);
    tx_symbols(sym, M17_SYMS_PER_FRAME);
    m17_rf_lsf_frame(s_lsf, sym);
    tx_symbols(sym, M17_SYMS_PER_FRAME);
    s_tx_active = true;
    printf("TX  %s -> %s  Codec 2 3200%s\n", s_call, s_cmd_dst[0] ? s_cmd_dst : s_dst,
           s_cmd_dst[0] ? "  (command)" : (s_tx_test_frames >= 0 ? "  (test)" : ""));
}

static void tx_frame(const int16_t *pcm320, bool last)
{
    uint8_t pl[16];
    int64_t t0 = esp_timer_get_time();
    codec2_encode(s_enc, pl, (short *)pcm320);
    codec2_encode(s_enc, pl + 8, (short *)pcm320 + 160);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_enc_us_max) {
        s_enc_us_max = us;
    }
    memcpy(s_tx_hist[s_fn % 64], pl, 16);
    int8_t sym[M17_SYMS_PER_FRAME];
    m17_rf_stream_frame(s_lsf, s_fn, last, pl, sym);
    tx_symbols(sym, M17_SYMS_PER_FRAME);
    s_fn = (uint16_t)((s_fn + 1) & 0x7FFF);
    s_tx_frames_total++;
}

static void tx_finish(void)
{
    int16_t pcm[320] = { 0 };
    ring_get(&s_mic8, pcm, 320);                  /* what is left, padded with silence */
    tx_frame(pcm, true);
    int8_t sym[M17_SYMS_PER_FRAME];
    m17_rf_eot(sym);
    tx_symbols(sym, M17_SYMS_PER_FRAME);
    s_tx_active = false;
    s_tx_end_us = esp_timer_get_time();
    printf("    end of TX: %u frames (%.2f s)\n", (unsigned)s_fn, s_fn * 0.04);
    if (s_dst_once) {
        snprintf(s_dst, sizeof(s_dst), "%s", s_dst_prev);
        s_dst_once = false;
    }
}

/* the destination of an answer from a relay (m17_pc relay mode, M17Gateway-like) */
static bool is_answer(const char *d)
{
    size_t n = strlen(d);
    return !strcmp(d, "UNLINKED") || !strcmp(d, "LINK FAIL") || !strcmp(d, "NOT FOUND") ||
           (n == 9 && !strncmp(d, "M17-", 4) && d[7] == ' ');
}

static void note_station(const char *src, const char *dst)
{
    int64_t now = esp_timer_get_time();
    if (is_answer(dst) || !strcmp(dst, "ECHO")) {
        bool ours = s_relay_call[0] ? !strcmp(src, s_relay_call) : (s_cmd_pending && now - s_cmd_us < 20000000);
        if (ours) {
            if (strcmp(dst, "ECHO")) {
                snprintf(s_relay_state, sizeof(s_relay_state), "%s", dst);
                s_relay_us = now;
            }
            s_cmd_pending = false;
            if (strcmp(s_relay_call, src)) {
                snprintf(s_relay_call, sizeof(s_relay_call), "%s", src);
                settings_save();
                printf("relay: answers come from %s (kept)\n", src);
            }
            printf("relay: %s\n", dst);
            return;
        }
    }
    int k = 0;                                    /* move to the top of the heard list */
    while (k < s_heard_n && strcmp(s_heard[k], src)) {
        ++k;
    }
    if (k == s_heard_n && s_heard_n < UI_HEARD_N) {
        s_heard_n++;
    }
    if (k == UI_HEARD_N) {
        k = UI_HEARD_N - 1;
    }
    memmove(s_heard[1], s_heard[0], sizeof(s_heard[0]) * (size_t)k);
    memmove(&s_heard_us[1], &s_heard_us[0], sizeof(s_heard_us[0]) * (size_t)k);
    snprintf(s_heard[0], sizeof(s_heard[0]), "%s", src);
    s_heard_us[0] = now;
}

static void rx_end(const char *why)
{
    if (!R.active) {
        return;
    }
    R.active = false;
    float secs = R.frames * 0.04f;
    printf("    end of RX%s: %.2f s, %u frames, avg Viterbi %.1f bits, Golay fixes %u, bad LICH %u, worst EVM %.3f\n", why,
           secs, (unsigned)R.frames, R.frames ? R.cost / R.frames / 65535.0 : 0, (unsigned)R.golay_err,
           (unsigned)R.bad_lich, R.evm_max);
    if (R.cmp_frames) {
        printf("    loopback compare: %u frames, %u differ, %u bit errors -> %s\n", (unsigned)R.cmp_frames,
               (unsigned)R.cmp_bad, (unsigned)R.cmp_bits, R.cmp_bad ? "NG" : "OK (bit-exact)");
    }
}

static void rx_frame(const frame_msg_t *m)
{
    const m17_sync_info_t *in = &m->info;
    if (in->type == M17_FR_EOT) {
        rx_end("");
        return;
    }
    if (in->type == M17_FR_LOST) {
        rx_end(" (signal lost)");
        return;
    }
    if (!m->has_soft) {
        return;
    }
    if (in->type == M17_FR_LSF) {
        uint8_t l[M17_LSF_BYTES];
        m17_rx_info_t ri;
        if (m17_rf_decode_lsf(m->soft, l, &ri)) {
            rx_end(" (new LSF)");
            memset(&R, 0, sizeof(R));
            R.active = R.lsf_ok = true;
            R.self = s_tx_active || esp_timer_get_time() - s_tx_end_us < 500000;
            R.t0 = esp_timer_get_time();
            m17_decode_call(l, R.dst);
            m17_decode_call(l + 6, R.src);
            R.type = (uint16_t)(l[12] << 8 | l[13]);
            R.voice1600 = ((R.type >> 1) & 3) == 3;
            if (!R.self) {
                note_station(R.src, R.dst);
            }
            printf("RX  %s -> %s  type %04X (%s)  gain %.0f%s\n", R.src, R.dst, R.type, m17_type_name(R.type),
                   in->gain, in->gain < 0 ? " inverted" : "");
        }
        return;
    }
    if (in->type != M17_FR_STREAM) {
        return;
    }
    m17_stream_rx_t s;
    m17_rf_decode_stream(m->soft, &s);
    if (!R.active) {                              /* joined late: no LSF */
        memset(&R, 0, sizeof(R));
        R.active = true;
        R.t0 = esp_timer_get_time();
        strcpy(R.src, "?");
        strcpy(R.dst, "?");
        R.type = M17_TYPE_V3200;
        printf("RX  (late join, FN %u)\n", s.fn);
    }
    R.frames++;
    R.cost += s.viterbi_cost;
    R.golay_err += (uint32_t)(s.golay_errors > 0 ? s.golay_errors : 0);
    R.bad_lich += s.lich_cnt < 0;
    if (in->evm > R.evm_max) {
        R.evm_max = in->evm;
    }
    s_rx_evm = in->evm;
    if (R.self) {                                 /* our own frames coming back (loopback)? */
        if (s_tx_frames_total && R.lsf_ok && !strcmp(R.src, s_call)) {
            int be = 0;
            for (int i = 0; i < 16; ++i) {
                be += __builtin_popcount(s.payload[i] ^ s_tx_hist[s.fn % 64][i]);
            }
            R.cmp_frames++;
            R.cmp_bad += be != 0;
            R.cmp_bits += (uint32_t)be;
        }
    }
    int16_t pcm[320];
    if (R.voice1600) {
        codec2_decode(s_dec16, pcm, s.payload);
    } else {
        codec2_decode(s_dec32, pcm, s.payload);
        codec2_decode(s_dec32, pcm + 160, s.payload + 8);
    }
    ring_put(&s_spk8, pcm, 320);
    if (s.last) {
        rx_end("");
    }
}

static void codec_task(void *arg)
{
    frame_msg_t *m = malloc(sizeof(frame_msg_t));
    static int16_t pcm[320];
    for (;;) {
        while (xQueueReceive(s_frames, m, 0) == pdTRUE) {
            rx_frame(m);
        }
        bool key = gpio_get_level(KEY_PTT) == 0;
        bool want = s_ptt_req || key || s_tx_test_frames > 0;
        if (want && !s_tx_active) {
            tx_start();
        }
        if (s_tx_active) {
            while (ring_count(&s_mic8) >= 320 && (s_ptt_req || key || s_tx_test_frames > 0)) {
                ring_get(&s_mic8, pcm, 320);
                if (s_tx_silent && s_tx_test_frames > 0) {      /* a relay command: a short, silent call */
                    memset(pcm, 0, sizeof(pcm));
                } else if (s_tx_tone && s_tx_test_frames > 0) {    /* "tx N tone": 600 Hz, 1 s on / 1 s off */
                    static uint32_t ph;
                    for (int i = 0; i < 320; ++i, ++ph) {
                        bool on = (ph / 8000) % 2 == 0;
                        pcm[i] = on ? (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * 600.0f * (float)ph / 8000.0f)) : 0;
                    }
                }
                bool last = s_tx_test_frames == 1;
                tx_frame(pcm, last);
                if (s_tx_test_frames > 0 && --s_tx_test_frames == 0) {
                    int8_t sym[M17_SYMS_PER_FRAME];
                    m17_rf_eot(sym);
                    tx_symbols(sym, M17_SYMS_PER_FRAME);
                    s_tx_active = false;
                    s_tx_end_us = esp_timer_get_time();
                    s_tx_test_frames = -1;
                    printf("    end of TX %s: %u frames\n", s_cmd_dst[0] ? "command" : "test", (unsigned)s_fn);
                    s_cmd_dst[0] = 0;
                    s_tx_silent = false;
                }
            }
            if (s_tx_active && !(s_ptt_req || key || s_tx_test_frames > 0)) {
                tx_finish();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* ------------------------------------------------------------------ front panel (ui.c) */
static SemaphoreHandle_t s_ui_mux;
static bool s_ui_auto;

void app_status(ui_status_t *u)
{
    memset(u, 0, sizeof(*u));
    int64_t now = esp_timer_get_time();
    snprintf(u->call, sizeof(u->call), "%s", s_call);
    snprintf(u->dst, sizeof(u->dst), "%s", s_dst);
    u->dst_once = s_dst_once;
    u->mode = s_mode;
    u->tx = s_tx_active;
    u->rx = R.active && !R.self;
    snprintf(u->rx_src, sizeof(u->rx_src), "%s", R.src);
    snprintf(u->rx_dst, sizeof(u->rx_dst), "%s", R.dst);
    u->rx_evm = s_rx_evm;
    snprintf(u->relay_state, sizeof(u->relay_state), "%s", s_relay_state);
    snprintf(u->relay_call, sizeof(u->relay_call), "%s", s_relay_call);
    u->relay_age_s = s_relay_us ? (uint32_t)((now - s_relay_us) / 1000000) : 0;
    if (s_cmd_pending && now - s_cmd_us > 20000000) {
        s_cmd_pending = false;                    /* no answer within 20 s */
    }
    u->cmd_pending = s_cmd_pending;
    u->line_dbfs = s_line_dbfs;
    u->tx_dbfs = s_tx_dbfs;
    u->mic_db = s_mic_db;
    u->vol = s_vol;
    u->can = s_can;
    u->heard_n = s_heard_n;
    for (int i = 0; i < s_heard_n; ++i) {
        memcpy(u->heard[i], s_heard[i], sizeof(u->heard[i]));
        u->heard[i][sizeof(u->heard[i]) - 1] = 0;
        u->heard_age_s[i] = (uint32_t)((now - s_heard_us[i]) / 1000000);
    }
    char tmp[sizeof(s_refl)];
    snprintf(tmp, sizeof(tmp), "%s", s_refl);
    for (char *t = strtok(tmp, ","); t && u->refl_n < UI_REFL_N; t = strtok(NULL, ",")) {
        snprintf(u->refl[u->refl_n++], sizeof(u->refl[0]), "%.7s", t);
    }
}

void app_set_mode(ui_mode_t m)
{
    s_mode = m;
    settings_save();
}

void app_set_dst(const char *dst, bool once)
{
    if (once) {
        if (!s_dst_once) {
            snprintf(s_dst_prev, sizeof(s_dst_prev), "%s", s_dst);
        }
    } else {
        snprintf(s_dst_prev, sizeof(s_dst_prev), "%s", dst);
    }
    snprintf(s_dst, sizeof(s_dst), "%s", dst);
    s_dst_once = once;
    printf("destination %s%s\n", s_dst, once ? " (next transmission only)" : "");
}

void app_send_command(const char *dst)
{
    if (s_tx_active || s_tx_test_frames > 0) {
        printf("command %s not sent: transmitting\n", dst);
        return;
    }
    snprintf(s_cmd_dst, sizeof(s_cmd_dst), "%s", dst);
    s_tx_silent = true;
    s_tx_tone = false;
    s_cmd_us = esp_timer_get_time();
    s_cmd_pending = true;
    s_tx_test_frames = 13;                        /* 0.5 s */
    printf("command: a short call to \"%s\"\n", dst);
}

void app_set_call(const char *call)
{
    snprintf(s_call, sizeof(s_call), "%s", call);
    settings_save();
    printf("callsign %s (saved)\n", s_call);
}

void app_set_tx_level(float dbfs)
{
    s_tx_dbfs = dbfs;
    settings_save();
}

void app_set_mic(int db)
{
    s_mic_db = db;
    es8388_set_gains(0, s_mic_db);
    settings_save();
}

void app_set_vol(int vol)
{
    s_vol = vol;
    es8388_set_out(30, s_vol, 0, s_vol);
    settings_save();
}

void app_set_can(int can)
{
    s_can = can & 15;
    settings_save();
}

static void ui_print(void)
{
    char rows[UI_ROWS][UI_COLS + 1];
    xSemaphoreTake(s_ui_mux, portMAX_DELAY);
    ui_render(rows);
    xSemaphoreGive(s_ui_mux);
    printf("+---------------------+\n");
    for (int r = 0; r < UI_ROWS; ++r) {
        printf("|%s|\n", rows[r]);
    }
    printf("+---------------------+\n");
}

static void ui_input(const char *keys)
{
    for (const char *k = keys; *k; ++k) {
        ui_event_t e = *k == 'r' ? UI_RIGHT : *k == 'l' ? UI_LEFT : *k == 'p' ? UI_PUSH : UI_BACK;
        xSemaphoreTake(s_ui_mux, portMAX_DELAY);
        ui_event(e);
        xSemaphoreGive(s_ui_mux);
    }
    ui_print();
}

/* "ui auto": print the screen when something on it changes (not for the level bar) */
static void ui_task(void *arg)
{
    uint32_t last = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!s_ui_auto) {
            continue;
        }
        uint32_t h = (uint32_t)s_tx_active | (uint32_t)(R.active && !R.self) << 1 | (uint32_t)s_cmd_pending << 2 |
                     (uint32_t)s_heard_n << 3;
        for (const char *c = s_relay_state; *c; ++c) {
            h = h * 31u + (uint8_t)*c;
        }
        for (const char *c = R.src; *c; ++c) {
            h = h * 31u + (uint8_t)*c;
        }
        if (h != last) {
            last = h;
            ui_print();
        }
    }
}

/* ------------------------------------------------------------------ serial commands */
static void status(void)
{
    printf("callsign %s, TX level %.1f dBFS%s, mic gain %d dB, volume %d, CAN %d\n", s_call, s_tx_dbfs,
           s_tx_invert ? " (inverted)" : "", s_mic_db, s_vol, s_can);
    printf("destination %s%s, relay %s answers %s, reflectors %s\n", s_dst, s_dst_once ? " (next only)" : "",
           s_relay_call[0] ? s_relay_call : "(not learned)", s_relay_state[0] ? s_relay_state : "-", s_refl);
    printf("mic %.1f dBFS, line in %.1f dBFS, TX %s, RX %s, polarity learned %+d, frames dropped %u\n", s_mic_dbfs,
           s_line_dbfs, s_tx_active ? "on" : "off", R.active ? "on" : "off", s_demod.polarity,
           (unsigned)s_frames_dropped);
    printf("free heap %u, PSRAM free %u, audio block max %u us of 10000, Codec 2 encode (40 ms) max %u us\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)s_audio_us_max, (unsigned)s_enc_us_max);
    printf("demodulator backlog max %u samples (%.1f ms)\n", (unsigned)s_demod_backlog_max,
           s_demod_backlog_max / 48.0f);
    s_audio_us_max = 0;
    s_demod_backlog_max = 0;
}

static void console_task(void *arg)
{
    char line[64];
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "t")) {
            s_ptt_req = !s_ptt_req;
            printf("PTT %s\n", s_ptt_req ? "on" : "off");
        } else if (!strncmp(line, "tx ", 3)) {
            int secs = atoi(line + 3);
            if (secs > 0 && secs < 300 && !s_tx_active) {
                s_tx_tone = strstr(line + 3, "tone") != NULL;
                s_tx_test_frames = secs * 25;
            }
        } else if (!strncmp(line, "c ", 2)) {
            char c[10] = { 0 };
            for (int i = 0; i < 9 && line[2 + i]; ++i) {
                char ch = line[2 + i];
                c[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
            }
            if (c[0]) {
                strcpy(s_call, c);
                settings_save();
                printf("callsign %s (saved)\n", s_call);
            }
        } else if (!strncmp(line, "lvl ", 4)) {
            app_set_tx_level((float)atof(line + 4));
            printf("TX level %.1f dBFS (from the next transmission)\n", s_tx_dbfs);
        } else if (!strcmp(line, "diag")) {
            /* which output reaches which input: tone on L, then on R; both ADCs on the line-in jack, 0 dB */
            static const char *const nm[3] = { "none", "voice (ring)", "modem (tip)" };
            for (int sel = 0; sel < 2; ++sel) {
                es8388_set_input(sel ? ES_IN_MIC : ES_IN_LINE, 0);
                for (int ch = 0; ch <= 2; ++ch) {
                    s_diag_ch = ch;
                    vTaskDelay(pdMS_TO_TICKS(800));
                    printf("diag: ADC %s, tone on %-12s -> mic word %.1f dBFS, line word %.1f dBFS\n",
                           sel ? "LIN1/RIN1" : "LIN2/RIN2", nm[ch], s_mic_dbfs, s_line_dbfs);
                }
            }
            s_diag_ch = 0;
            es8388_set_input(ES_IN_SPLIT, 0);
            es8388_set_gains(0, s_mic_db);
        } else if (!strncmp(line, "cap ", 4)) {
            /* cap SECONDS [TX]: record the line input (with TX: transmit at the same time), then print it in hex */
            float secs = (float)atof(line + 4);
            if (secs <= 0 || secs > 4) {
                printf("cap: 0.1 .. 4 seconds\n");
                continue;
            }
            if (!s_cap_buf) {
                s_cap_buf = heap_caps_malloc(4 * 48000 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            }
            if (!s_cap_buf) {
                printf("cap: no memory\n");
                continue;
            }
            bool with_tx = strstr(line + 4, "tx") != NULL;
            s_cap_len = 0;
            s_cap_n = 0;
            s_cap_len = (uint32_t)(secs * 48000);
            if (with_tx && !s_tx_active) {
                s_tx_test_frames = (int)(secs * 25) + 1;
            }
            while (s_cap_n < s_cap_len) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            uint32_t total = s_cap_n;
            s_cap_len = 0;
            printf("CAP BEGIN %u\n", (unsigned)total);
            char hex[4 * 64 + 2];
            for (uint32_t i = 0; i < total; i += 64) {
                int k = 0;
                for (uint32_t j = i; j < i + 64 && j < total; ++j) {
                    k += sprintf(hex + k, "%04x", (uint16_t)s_cap_buf[j]);
                }
                hex[k] = 0;
                puts(hex);
            }
            printf("CAP END\n");
        } else if (!strcmp(line, "keys")) {
            /* 15 s: print the key GPIOs whenever one changes (press the keys one by one) */
            for (int i = 0; i < 6; ++i) {
                gpio_config_t k = { .pin_bit_mask = 1ULL << KEY_PINS[i], .mode = GPIO_MODE_INPUT,
                                    .pull_up_en = KEY_PINS[i] < 34 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE };
                gpio_config(&k);
            }
            int last = -1;
            printf("keys: press KEY1..KEY6 one at a time (15 s)\n");
            for (int t = 0; t < 15 * 50; ++t) {
                int v = 0;
                for (int i = 0; i < 6; ++i) {
                    v |= gpio_get_level(KEY_PINS[i]) << i;
                }
                if (v != last) {
                    printf("keys: GPIO36=%d 13=%d 19=%d 23=%d 18=%d 5=%d\n", v & 1, (v >> 1) & 1, (v >> 2) & 1,
                           (v >> 3) & 1, (v >> 4) & 1, (v >> 5) & 1);
                    last = v;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            printf("keys: done\n");
        } else if (!strcmp(line, "dl")) {
            s_dig_loop = !s_dig_loop;
            printf("digital loopback %s (demodulator input = %s)\n", s_dig_loop ? "on" : "off",
                   s_dig_loop ? "our own modulator" : "line in");
        } else if (!strcmp(line, "inv")) {
            s_tx_invert = !s_tx_invert;
            printf("TX polarity %s\n", s_tx_invert ? "inverted" : "normal");
        } else if (!strncmp(line, "mic ", 4)) {
            app_set_mic(atoi(line + 4));
            printf("mic gain %d dB\n", s_mic_db);
        } else if (!strncmp(line, "vol ", 4)) {
            app_set_vol(atoi(line + 4));
            printf("volume %d\n", s_vol);
        } else if (!strcmp(line, "s")) {
            status();
        } else if (line[0] && strspn(line, "rlpb") == strlen(line)) {
            ui_input(line);                       /* the encoder: r/l turn, p push, b long push */
        } else if (!strcmp(line, "ui")) {
            ui_print();
        } else if (!strcmp(line, "ui auto")) {
            s_ui_auto = !s_ui_auto;
            printf("screen printed on changes: %s\n", s_ui_auto ? "on" : "off");
        } else if (!strncmp(line, "dst ", 4)) {
            char d[10] = { 0 };
            for (int i = 0; i < 9 && line[4 + i]; ++i) {
                char ch = line[4 + i];
                d[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
            }
            app_set_dst(d, false);
        } else if (!strcmp(line, "refl")) {
            printf("reflectors: %s\n", s_refl);
        } else if (!strncmp(line, "refl add ", 9) || !strncmp(line, "refl del ", 9)) {
            char name[8] = { 0 };
            for (int i = 0; i < 7 && line[9 + i]; ++i) {
                char ch = line[9 + i];
                name[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
            }
            char out[sizeof(s_refl)] = "", tmp[sizeof(s_refl)];
            snprintf(tmp, sizeof(tmp), "%s", s_refl);
            int n = 0;
            for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
                if (strcmp(t, name) && n < UI_REFL_N) {
                    snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s", out[0] ? "," : "", t);
                    n++;
                }
            }
            if (line[5] == 'a' && name[0] && n < UI_REFL_N) {
                snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s", out[0] ? "," : "", name);
            }
            snprintf(s_refl, sizeof(s_refl), "%s", out);
            settings_save();
            printf("reflectors: %s\n", s_refl);
        } else if (!strcmp(line, "relay forget")) {
            s_relay_call[0] = 0;
            s_relay_state[0] = 0;
            settings_save();
            printf("relay forgotten: the next answer to a command teaches it again\n");
        } else if (line[0]) {
            printf("commands: t | tx N [tone] | c CALL | s | lvl DB | inv | mic DB | vol N | dl | diag | cap S [tx] | keys\n"
                   "panel:    r l p b (turn right/left, push, long push) | ui | ui auto | dst CALL | refl [add|del NAME]\n"
                   "          | relay forget\n");
        }
    }
}

/* ------------------------------------------------------------------ start */
static void i2s_start(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_i2s_tx, &s_i2s_rx));
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = A1S_I2S_MCLK, .bclk = A1S_I2S_BCLK, .ws = A1S_I2S_LRCK,
            .dout = A1S_I2S_DOUT, .din = A1S_I2S_DIN,
        },
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_rx));
}

void app_main(void)
{
    if (nvs_flash_init() != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    ESP_ERROR_CHECK(es_i2c_init());
    uint8_t found[8];
    int n = es_i2c_scan(found, 8);
    bool es = false;
    for (int i = 0; i < n; ++i) {
        es |= found[i] == ES8388_ADDR;
    }
    if (!es) {
        ESP_LOGE(TAG, "no ES8388 codec at 0x10: this firmware is for the ES8388 version of the ESP32-A1S kit");
        return;
    }
    gpio_set_direction(A1S_PA_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(A1S_PA_EN, 1);
    gpio_config_t key = { .pin_bit_mask = 1ULL << KEY_PTT, .mode = GPIO_MODE_INPUT };
    gpio_config(&key);

    i2s_start();
    ESP_ERROR_CHECK(es8388_init(ES_IN_SPLIT, 0));          /* left ADC = LIN2 (line), right ADC = RIN1 (mic) */
    es8388_set_gains(0, s_mic_db);
    es8388_set_out(30, s_vol, 0, s_vol);          /* LOUT1 modem, ROUT1 voice, LOUT2 off, ROUT2 speaker voice */

    lp_init();
    m17_rf_init();
    ring_init(&s_mic8, 8000);
    ring_init(&s_spk8, 8000);
    ring_init(&s_txbb, 48000);
    ring_init(&s_line48, 48000);
    s_enc = codec2_create(CODEC2_MODE_3200);
    s_dec32 = codec2_create(CODEC2_MODE_3200);
    s_dec16 = codec2_create(CODEC2_MODE_1600);
    s_frames = xQueueCreate(6, sizeof(frame_msg_t));
    m17_demod_init(&s_demod, on_demod_frame, NULL);
    if (!s_enc || !s_dec32 || !s_dec16 || !s_frames || !s_txbb.b) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }

    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    esp_vfs_dev_uart_use_driver(UART_NUM_0);
    setvbuf(stdin, NULL, _IONBF, 0);

    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 20, NULL, 0);
    xTaskCreatePinnedToCore(demod_task, "demod", 8192, NULL, 15, NULL, 0);
    xTaskCreatePinnedToCore(codec_task, "codec", 32768, NULL, 10, NULL, 1);
    s_ui_mux = xSemaphoreCreateMutex();
    ui_init();
    xTaskCreatePinnedToCore(console_task, "console", 6144, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(ui_task, "ui", 4096, NULL, 4, NULL, 1);
    printf("\nm17_modem ready. callsign %s. PTT: KEY1 or \"t\"; \"tx 5\" sends 5 s; \"s\" status.\n", s_call);
    printf("PHONE tip = M17 modulation (to the radio), PHONE ring + speaker = received voice; LINE IN tip = M17 input\n");
    printf("front panel on the serial port: \"ui\" shows the screen, r / l / p / b turn and push (b = long push)\n");
}
