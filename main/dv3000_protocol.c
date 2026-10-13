#include "dv3000_protocol.h"

#include <math.h>
#include <string.h>
#include "ambe_codec.h"
#include "dv3000_log.h"
#include "noise_reduce.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#elif !defined(CONFIG_AMBE_NOISE_GATE)
#define CONFIG_AMBE_NOISE_GATE 1
#endif

#define START_BYTE        0x61
#define TYPE_CONTROL      0x00
#define TYPE_AMBE         0x01
#define TYPE_AUDIO        0x02

#define CTRL_RATET        0x09
#define CTRL_RATEP        0x0A
#define CTRL_GAIN         0x4B   /* PKT_GAIN: int8 input gain, int8 output gain, in dB (BlueDV sends 4B 05 05) */
#define CTRL_PRODID       0x30
#define CTRL_VERSTRING    0x31
#define CTRL_RESET        0x33   /* PKT_RESET        */
#define CTRL_RESETSOFTCFG 0x34   /* PKT_RESETSOFTCFG (what AMBETools sends with -r) */
#define CTRL_READY        0x39
#define CTRL_DBG_SET      0xF0   /* private (tuning/diagnostics), see handle_control */
#define CTRL_DBG_FRAME    0xF1
#define CTRL_DBG_PARAM    0xF2   /* F2 <id> <int16 value, big endian>: one tuning parameter (see handle_control) */
#define CTRL_DBG_DEC      0xF3   /* decoded-frame parameters (F2 id 12 = 1) */
static bool s_dbg_dec;

/* High-shelf pre-emphasis (+dB above ~1.5 kHz) for muffled microphones; 0 dB = off. */
static float s_shelf_gain;           /* linear extra gain of the high band, 10^(dB/20) - 1; +6 dB set in dv3000_protocol_init (F2 id 4) */
static float s_shelf_hp, s_shelf_x1;
#define SHELF_A 0.3085f           /* exp(-2*pi*1500/8000): 1st-order high-pass corner ~1.5 kHz */

/* Run-time switches for A/B tests (private 0xF0 command). Defaults = normal operation. */
static bool s_hpf_on = true;
static bool s_gate_on = true;
static bool s_nr_on = true;       /* noise reduction (noise_reduce.c) */
static bool s_dbg_frames;

#ifdef ESP_PLATFORM
extern float mbe_ambe2400_enc_track_strength, mbe_ambe2400_enc_max_jump, mbe_ambe2400_enc_cont_weight;
extern float mbe_ambe2400_enc_dbg_lag, mbe_ambe2400_enc_dbg_strength;
extern int mbe_ambe2400_enc_dbg_b0, mbe_ambe2400_enc_dbg_vstate, mbe_ambe2400_enc_dbg_b1;
extern int mbe_ambe2400_enc_vhyst;
extern float mbe_ambe2400_enc_agc_alpha;
extern int mbe_ambe2400_enc_band_fix;
extern float mbe_ambe2400_enc_smooth;
extern int mbe_ambe2400_enc_mag_mode;
extern float mbe_ambe2400_enc_v_hi, mbe_ambe2400_enc_v_lo;
extern int mbe_ambe2400_enc_vcut_hi, mbe_ambe2400_enc_vcut_lo, mbe_ambe2400_enc_vcut_force;
extern float mbe_ambe2400_enc_octave_ratio, mbe_ambe2400_enc_octave_min, mbe_ambe2400_enc_uv_below;
extern const short* mbe_ambe2400_enc_pitch_pcm;
#endif

#define DSTAR_BLOCK       9
#define AUDIO_SAMPLES     160
#define AUDIO_BYTES       320
#define MAX_PACKET        400
#define LENGTH_MASK       0x0F

/* RATEP payload (after the command byte) that selects D-STAR 2400+1200 FEC. */
static const uint8_t DSTAR_FEC_RATEP[12] = {
    0x01, 0x30, 0x07, 0x63, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48
};

/* Standard D-STAR "null" AMBE frame; returned if encoding fails so the host never stalls. */
static const uint8_t DSTAR_SILENCE[DSTAR_BLOCK] = {
    0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8
};

static dv3000_tx_fn s_tx;
static float s_in_gain = 1.0f, s_out_gain = 1.0f;   /* linear, set by PKT_GAIN */

static float db_to_lin(int8_t db)
{
    if (db < -90) db = -90;
    if (db > 90) db = 90;
    return powf(10.0f, (float)db / 20.0f);
}

/* 150 Hz high-pass in front of the encoder (2nd-order Butterworth, fs 8 kHz). Mains hum / rumble below
 * 100 Hz throws the encoder's pitch tracker off (octave jumps heard as a "gyuu-gyuu" warble); the real
 * AMBE-3000 also high-passes its input. On a captured BlueDV stream this cut pitch jumps from 66 to 20. */
#define HPF_B0  0.920066f
#define HPF_B1 -1.840132f
#define HPF_B2  0.920066f
#define HPF_A1 -1.833733f
#define HPF_A2  0.846532f
static float s_hpf_x1, s_hpf_x2, s_hpf_y1, s_hpf_y2;

static int16_t hpf(int16_t in)
{
    float x = (float)in;
    float y = HPF_B0 * x + HPF_B1 * s_hpf_x1 + HPF_B2 * s_hpf_x2 - HPF_A1 * s_hpf_y1 - HPF_A2 * s_hpf_y2;
    s_hpf_x2 = s_hpf_x1;
    s_hpf_x1 = x;
    s_hpf_y2 = s_hpf_y1;
    s_hpf_y1 = y;
    if (y > 32767.0f) return 32767;
    if (y < -32768.0f) return -32768;
    return (int16_t)y;
}

/* Noise gate in front of the encoder. mbelib-neo has no noise suppression: background noise louder than its
 * fixed silence threshold is encoded as voice and comes out as a buzzing/warbling tone between words.
 * The gate tracks the noise floor (falls fast, rises slowly) and passes a frame only when it is clearly
 * above it; a hang-over keeps word endings. Closed frames are fed as zeros (-> AMBE silence frames). */
static float s_gate_ratio = 2.0f; /* open when frame RMS > 2 x noise floor (+6 dB); F2 id 8 */
static float s_pre_gain = 1.0f;     /* fixed gain right before the encoder, replaces its AGC; +12 dB set in dv3000_protocol_init (F2 id 7) */
#define GATE_RATIO     s_gate_ratio
#define GATE_MIN_RMS   60.0f      /* never open below this (digital silence / dither) */
static int s_gate_hang_frames = 10;  /* frames kept open after the last loud frame (F2 id 10) */
static bool s_gate_direct = true;          /* closed gate -> send the AMBE silence frame at once (F2 id 11) */
static bool s_gate_prev_open;        /* gate decision of the previous input frame (NR delay alignment) */
static float s_lim_gain;             /* current limiter gain (0 = not started) */
static bool s_pitch_pre_nr = true;   /* search the pitch before noise reduction (F2 id 19) */
static bool s_eq_enable = true;      /* automatic equaliser inside the NR (F2 id 20) */
static float s_eq_limit = 8.0f;      /* its max correction per band, dB (F2 id 21) */
static int16_t s_pre_prev[AUDIO_SAMPLES], s_pitch_src[AUDIO_SAMPLES];
#define GATE_HANG      s_gate_hang_frames
static float s_floor;              /* 0 = not initialised */
static int s_gate_hang;

#if CONFIG_AMBE_NOISE_GATE
static bool gate_open(const int16_t pcm[], int n)
{
    float e = 0.0f;
    for (int i = 0; i < n; ++i) {
        e += (float)pcm[i] * (float)pcm[i];
    }
    float rms = sqrtf(e / (float)n);
    if (s_floor <= 0.0f) {
        /* Start LOW: learning the floor from the first frame mutes a stream that starts with speech
         * (the 1 kHz test tone at the start of a stream was gated away completely). */
        s_floor = GATE_MIN_RMS;
    }
    if (rms < s_floor) {
        s_floor = s_floor * 0.9f + rms * 0.1f;        /* fall quickly towards quieter frames */
    } else {
        s_floor *= 1.01f;                            /* rise slowly (+4 dB/s): a vowel lifts it < 1.5 dB */
    }
    if (rms > s_floor * GATE_RATIO && rms > GATE_MIN_RMS) {
        s_gate_hang = GATE_HANG;
    } else if (s_gate_hang > 0) {
        s_gate_hang--;
    }
    return s_gate_hang > 0;
}
#endif

static int16_t scale_sample(int16_t v, float g)
{
    float f = (float)v * g;
    if (f > 32767.0f) return 32767;
    if (f < -32768.0f) return -32768;
    return (int16_t)f;
}
static uint8_t s_packet[MAX_PACKET];
static size_t  s_packet_len;
static uint8_t s_txbuf[MAX_PACKET];

static void tx(size_t len)
{
    if (s_tx && !s_tx(s_txbuf, len)) {
        DV_LOGE("TX failed (%u bytes dropped)", (unsigned)len);
    }
}

static void send_control(uint8_t command, const uint8_t *payload, size_t payload_len)
{
    size_t n = 1 + payload_len;
    if (n > 255) {
        return;
    }
    s_txbuf[0] = START_BYTE;
    s_txbuf[1] = (uint8_t)(n >> 8);
    s_txbuf[2] = (uint8_t)(n & 0xff);
    s_txbuf[3] = TYPE_CONTROL;
    s_txbuf[4] = command;
    if (payload_len) {
        memcpy(&s_txbuf[5], payload, payload_len);
    }
    tx(4 + n);
}

static void send_audio(const int16_t pcm[AUDIO_SAMPLES])
{
    s_txbuf[0] = START_BYTE;
    s_txbuf[1] = 0x01;
    s_txbuf[2] = 0x42;               /* 322 = 2 + 320 */
    s_txbuf[3] = TYPE_AUDIO;
    s_txbuf[4] = 0x00;
    s_txbuf[5] = 0xA0;               /* 160 samples */
    for (int i = 0; i < AUDIO_SAMPLES; ++i) {
        uint16_t v = (uint16_t)pcm[i];
        s_txbuf[6 + i * 2] = (uint8_t)(v >> 8);
        s_txbuf[7 + i * 2] = (uint8_t)(v & 0xff);
    }
    tx(6 + AUDIO_BYTES);
}

static void send_ambe(const uint8_t ambe[DSTAR_BLOCK])
{
    s_txbuf[0] = START_BYTE;
    s_txbuf[1] = 0x00;
    s_txbuf[2] = 0x0B;               /* 2 + 9 */
    s_txbuf[3] = TYPE_AMBE;
    s_txbuf[4] = 0x01;
    s_txbuf[5] = 0x48;               /* 72 bits */
    memcpy(&s_txbuf[6], ambe, DSTAR_BLOCK);
    tx(6 + DSTAR_BLOCK);
}

static void handle_control(const uint8_t *p, size_t n)
{
    if (n < 1) {
        return;
    }

    switch (p[0]) {
    case CTRL_PRODID: {
        static const uint8_t id[] = "AMBE3000R";           /* incl. NUL, like the chip */
        send_control(CTRL_PRODID, id, sizeof(id));
        break;
    }
    case CTRL_VERSTRING: {
        static const uint8_t ver[] = "ESP32 AMBE3000 compatible 0.2";
        send_control(CTRL_VERSTRING, ver, sizeof(ver));
        break;
    }
    case CTRL_GAIN:
        if (n == 3) {
            s_in_gain = db_to_lin((int8_t)p[1]);
            s_out_gain = db_to_lin((int8_t)p[2]);
            static const uint8_t ok = 0x00;
            send_control(CTRL_GAIN, &ok, 1);
            DV_LOGI("gain: input %d dB, output %d dB", (int8_t)p[1], (int8_t)p[2]);
        }
        break;

#ifdef ESP_PLATFORM
    case CTRL_DBG_SET:
        /* F0 <track_strength x100> <max_jump x100> <cont_weight x100>
         *    <flags: 1=HPF 2=gate 4=frame info 8=noise reduction>
         *    [<voicing hysteresis 0/1> [<NR gain floor, -dB> <NR noise over-estimation x10>
         *    [<encoder AGC smoothing x100, 100 = AGC off> [<band fix 0/1> [<spectral smoothing x100>]]]]] */
        if (n >= 5 && n <= 11 && n != 7) {
            if (n >= 10) {
                mbe_ambe2400_enc_band_fix = p[9];
            }
            if (n == 11) {
                mbe_ambe2400_enc_smooth = (float)p[10] / 100.0f;
            }
            if (n >= 6) {
                mbe_ambe2400_enc_vhyst = p[5];
            }
            if (n >= 8) {
                nr_set_params(-(float)p[6], (float)p[7] / 10.0f);
            }
            if (n >= 9) {
                mbe_ambe2400_enc_agc_alpha = (float)p[8] / 100.0f;
            }
            s_nr_on = (p[4] & 8) != 0;
            mbe_ambe2400_enc_track_strength = (float)p[1] / 100.0f;
            mbe_ambe2400_enc_max_jump = (float)p[2] / 100.0f;
            mbe_ambe2400_enc_cont_weight = (float)p[3] / 100.0f;
            s_hpf_on = (p[4] & 1) != 0;
            s_gate_on = (p[4] & 2) != 0;
            s_dbg_frames = (p[4] & 4) != 0;
            static const uint8_t ok = 0x00;
            send_control(CTRL_DBG_SET, &ok, 1);
        }
        break;
    case CTRL_DBG_PARAM:
        /* 1 = magnitude mode (0 band / 1 main lobe)   2 = voicing cutoff high x100   3 = voicing cutoff low x100
         * 4 = high-shelf pre-emphasis dB              5 = NR gain smoothing across bins (0/1)
         * 6 = spectral smoothing x100 */
        if (n == 4) {
            int v = (int16_t)(((uint16_t)p[2] << 8) | p[3]);
            switch (p[1]) {
            case 1: mbe_ambe2400_enc_mag_mode = v; break;
            case 2: mbe_ambe2400_enc_v_hi = (float)v / 100.0f; break;
            case 3: mbe_ambe2400_enc_v_lo = (float)v / 100.0f; break;
            case 4: s_shelf_gain = powf(10.0f, (float)v / 20.0f) - 1.0f; break;
            case 5: nr_set_freq_smooth(v != 0); break;
            case 6: mbe_ambe2400_enc_smooth = (float)v / 100.0f; break;
            case 7: s_pre_gain = powf(10.0f, (float)v / 20.0f); break;
            case 8: s_gate_ratio = (float)v / 10.0f; break;
            case 9: mbe_ambe2400_enc_agc_alpha = (float)v / 100.0f; break;
            case 10: s_gate_hang_frames = v; break;
            case 11: s_gate_direct = (v != 0); break;
            case 12: s_dbg_dec = (v != 0); break;
            case 13: mbe_ambe2400_enc_vcut_hi = v; break;
            case 14: mbe_ambe2400_enc_vcut_lo = v; break;
            case 15: mbe_ambe2400_enc_vcut_force = v; break;
            case 16: mbe_ambe2400_enc_octave_ratio = (float)v / 100.0f; break;
            case 17: mbe_ambe2400_enc_uv_below = (float)v / 100.0f; break;
            case 18: mbe_ambe2400_enc_octave_min = (float)v / 100.0f; break;
            case 19: s_pitch_pre_nr = (v != 0); break;
            case 20: s_eq_enable = (v != 0); nr_set_auto_eq(s_eq_enable, s_eq_limit); break;
            case 21: s_eq_limit = (float)v; nr_set_auto_eq(s_eq_enable, s_eq_limit); break;
            case 22: nr_set_eq_blend((float)v / 100.0f); break;   /* EQ target: 0 natural .. 100 others */
            default: break;
            }
            static const uint8_t ok = 0x00;
            send_control(CTRL_DBG_PARAM, &ok, 1);
        }
        break;
#endif

    case CTRL_RESET:
    case CTRL_RESETSOFTCFG:
        s_in_gain = s_out_gain = 1.0f;          /* (0xF0 test switches are deliberately kept across resets) */
        s_floor = 0.0f;
        s_gate_hang = 0;
        s_gate_prev_open = false;
        s_shelf_hp = s_shelf_x1 = 0.0f;
        s_lim_gain = 0.0f;
        memset(s_pre_prev, 0, sizeof(s_pre_prev));
        s_hpf_x1 = s_hpf_x2 = s_hpf_y1 = s_hpf_y2 = 0.0f;
        nr_reset();
        ambe_codec_reset();
        send_control(CTRL_READY, NULL, 0);
        break;

    case CTRL_RATEP:
        /* Real chip answers 61 00 02 00 0A 00 (status 0 = OK). */
        if (n - 1 == sizeof(DSTAR_FEC_RATEP) &&
            memcmp(p + 1, DSTAR_FEC_RATEP, sizeof(DSTAR_FEC_RATEP)) == 0) {
            static const uint8_t ok = 0x00;
            send_control(CTRL_RATEP, &ok, 1);
        } else {
            /* Unsupported mode (DMR/P25/NXDN...): stay silent instead of lying. */
            DV_LOGW("RATEP for unsupported mode ignored");
        }
        break;

    case CTRL_RATET:
        DV_LOGW("RATET (no-FEC / DMR / ...) not supported; use AMBETools -f 1");
        break;

    default:
        DV_LOGW("Unknown control 0x%02X", p[0]);
        break;
    }
}

/* Processing-time statistics (PCM -> AMBE), shown by the console 'status' command. */
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#define NOW_US() esp_timer_get_time()
#else
#define NOW_US() 0
#endif
static struct { int64_t sum_tot, sum_nr, sum_enc; int32_t max_tot; uint32_t n; } s_pt;

void dv3000_get_proc_stats(uint32_t *frames, uint32_t *avg_us, uint32_t *max_us, uint32_t *nr_us, uint32_t *enc_us)
{
    uint32_t n = s_pt.n ? s_pt.n : 1;
    *frames = s_pt.n;
    *avg_us = (uint32_t)(s_pt.sum_tot / n);
    *max_us = (uint32_t)s_pt.max_tot;
    *nr_us = (uint32_t)(s_pt.sum_nr / n);
    *enc_us = (uint32_t)(s_pt.sum_enc / n);
    memset(&s_pt, 0, sizeof(s_pt));
}

static void pt_add(int64_t t0, int64_t nr_us, int64_t enc_us)
{
    int32_t tot = (int32_t)(NOW_US() - t0);
    s_pt.sum_tot += tot;
    s_pt.sum_nr += nr_us;
    s_pt.sum_enc += enc_us;
    if (tot > s_pt.max_tot) s_pt.max_tot = tot;
    s_pt.n++;
}

static void handle_audio(const uint8_t *p, size_t n)
{
    if (n != 2 + AUDIO_BYTES || p[0] != 0x00 || p[1] != 0xA0) {
        DV_LOGW("Bad AUDIO packet length=%u", (unsigned)n);
        return;
    }
    int64_t t0 = NOW_US(), t_nr = 0, t_enc;

    int16_t pcm[AUDIO_SAMPLES];
    for (int i = 0; i < AUDIO_SAMPLES; ++i) {
        int16_t v = scale_sample((int16_t)(((uint16_t)p[2 + i * 2] << 8) | p[3 + i * 2]), s_in_gain);
        pcm[i] = s_hpf_on ? hpf(v) : v;
    }
#if CONFIG_AMBE_NOISE_GATE
    /* Gate decision on the signal BEFORE noise reduction: there the background noise is steady, while after
     * NR it fluctuates so much (frames close to zero next to frames with residual noise) that the floor
     * tracker sinks and the gate never closes between words. NR delays the audio by one frame, so the
     * decision is applied one frame later to stay aligned. */
    bool open_now = gate_open(pcm, AUDIO_SAMPLES);
    bool open = open_now;
    if (s_nr_on) {
        open = s_gate_prev_open || open_now;        /* the NR output frame mixes frames f-1 and f */
    }
    s_gate_prev_open = open_now;
#endif
    /* Pitch source: the frame BEFORE noise reduction, delayed one frame like the NR output (spectral
     * subtraction weakens the fundamental; pitch searched on the NR output was an octave too high on 7 % of
     * the voiced frames of a real microphone stream, vs 1.3 % before NR). Spectrum/voicing still use the
     * noise-reduced signal. */
    int16_t pre_nr[AUDIO_SAMPLES];
    memcpy(pre_nr, pcm, sizeof(pre_nr));
    if (s_nr_on) {
        int64_t t = NOW_US();
        nr_process(pcm);
        t_nr = NOW_US() - t;
    }
#ifdef ESP_PLATFORM
    memcpy(s_pitch_src, s_pre_prev, sizeof(s_pitch_src));
    memcpy(s_pre_prev, pre_nr, sizeof(s_pre_prev));
    mbe_ambe2400_enc_pitch_pcm = (s_nr_on && s_pitch_pre_nr) ? s_pitch_src : NULL;
#endif
    if (s_shelf_gain != 0.0f) {
        for (int i = 0; i < AUDIO_SAMPLES; ++i) {
            float x = (float)pcm[i];
            s_shelf_hp = SHELF_A * (s_shelf_hp + x - s_shelf_x1);
            s_shelf_x1 = x;
            float y = x + s_shelf_gain * s_shelf_hp;
            pcm[i] = (int16_t)(y > 32767.0f ? 32767.0f : (y < -32768.0f ? -32768.0f : y));
        }
    }

#if CONFIG_AMBE_NOISE_GATE
    if (s_gate_on && !open) {
        memset(pcm, 0, sizeof(pcm));
        if (s_gate_direct) {
            /* The encoder's own silence detector needs 5 quiet frames (100 ms) before it emits silence;
             * send the standard silence frame immediately instead, but keep feeding the encoder zeros so
             * its history/AGC state follows the gap. */
            uint8_t dummy[DSTAR_BLOCK];
            t_enc = NOW_US();
            ambe_encode_dstar(pcm, dummy);
            pt_add(t0, t_nr, NOW_US() - t_enc);
            send_ambe(DSTAR_SILENCE);
            return;
        }
    }
#endif
    if (s_pre_gain != 1.0f) {
        /* Gain with a peak limiter: +12 dB for normal (quiet) microphones, but never past ~-1 dBFS. A fixed
         * gain clipped loud speech (1 kHz at -12 dBFS input: 3rd harmonic only 14 dB down). Attack is
         * immediate, release +0.2 dB per frame; the gain is ramped across the frame (no steps). */
        int peak = 1;
        for (int i = 0; i < AUDIO_SAMPLES; ++i) {
            int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (a > peak) peak = a;
        }
        float target = s_pre_gain;
        float max_g = 29000.0f / (float)peak;
        if (target > max_g) target = max_g;
        float start = s_lim_gain > 0.0f ? s_lim_gain : target;
        if (start > max_g) start = max_g;                 /* attack: never exceed the headroom of this frame */
        float end = start * 1.023f;                       /* release */
        if (end > target) end = target;                   /* target <= max_g, so end never clips either */
        for (int i = 0; i < AUDIO_SAMPLES; ++i) {
            float g = start + (end - start) * (float)i / (float)(AUDIO_SAMPLES - 1);
            pcm[i] = scale_sample(pcm[i], g);
        }
        s_lim_gain = end;
    }

    uint8_t ambe[DSTAR_BLOCK];
    t_enc = NOW_US();
    if (!ambe_encode_dstar(pcm, ambe)) {
        memcpy(ambe, DSTAR_SILENCE, DSTAR_BLOCK);   /* always answer */
    }
    pt_add(t0, t_nr, NOW_US() - t_enc);
    send_ambe(ambe);

#ifdef ESP_PLATFORM
    if (s_dbg_frames) {
        /* F1 <lag x100 u16 BE> <strength x1000 u16 BE> <b0> <voicing state> <b1 (V/UV row)> */
        uint16_t lag = (uint16_t)(mbe_ambe2400_enc_dbg_lag * 100.0f);
        uint16_t st = (uint16_t)(mbe_ambe2400_enc_dbg_strength * 1000.0f);
        uint8_t d[7] = { (uint8_t)(lag >> 8), (uint8_t)lag, (uint8_t)(st >> 8), (uint8_t)st,
                         (uint8_t)mbe_ambe2400_enc_dbg_b0, (uint8_t)mbe_ambe2400_enc_dbg_vstate,
                         (uint8_t)mbe_ambe2400_enc_dbg_b1 };
        send_control(CTRL_DBG_FRAME, d, sizeof(d));
    }
#endif
}

static void handle_ambe(const uint8_t *p, size_t n)
{
    if (n != 2 + DSTAR_BLOCK || p[0] != 0x01 || p[1] != 0x48) {
        DV_LOGW("Bad AMBE packet length=%u", (unsigned)n);
        return;
    }

    int16_t pcm[AUDIO_SAMPLES];
    if (!ambe_decode_dstar(p + 2, pcm)) {
        memset(pcm, 0, sizeof(pcm));                /* always answer */
    }
    if (s_out_gain != 1.0f) {
        for (int i = 0; i < AUDIO_SAMPLES; ++i) {
            pcm[i] = scale_sample(pcm[i], s_out_gain);
        }
    }
    send_audio(pcm);

#ifdef ESP_PLATFORM
    if (s_dbg_dec) {
        /* F3 <w0 x10000 u16> <gamma x100 s16> <L> <voiced> <log2Ml[1..L] x8 as s8> */
        ambe_dec_info_t di;
        ambe_codec_last_decode(&di);
        uint8_t d[6 + 56];
        uint16_t w = (uint16_t)(di.w0 * 10000.0f);
        int16_t g = (int16_t)(di.gamma * 100.0f);
        int L = di.L < 1 ? 1 : (di.L > 56 ? 56 : di.L);
        d[0] = (uint8_t)(w >> 8); d[1] = (uint8_t)w;
        d[2] = (uint8_t)((uint16_t)g >> 8); d[3] = (uint8_t)g;
        d[4] = (uint8_t)L; d[5] = (uint8_t)di.voiced;
        for (int l = 1; l <= L; ++l) {
            float v = di.log2Ml[l] * 8.0f;
            d[5 + l] = (uint8_t)(int8_t)(v > 127.0f ? 127.0f : (v < -128.0f ? -128.0f : v));
        }
        send_control(CTRL_DBG_DEC, d, (size_t)(6 + L));
    }
#endif
}

static void process_packet(const uint8_t *packet, size_t total_len)
{
    const size_t payload_len = total_len - 4;
    const uint8_t *payload = packet + 4;

    switch (packet[3]) {
    case TYPE_CONTROL: handle_control(payload, payload_len); break;
    case TYPE_AUDIO:   handle_audio(payload, payload_len);   break;
    case TYPE_AMBE:    handle_ambe(payload, payload_len);    break;
    default:           DV_LOGW("Unknown packet type 0x%02X", packet[3]); break;
    }
}

void dv3000_protocol_init(dv3000_tx_fn tx_fn)
{
    s_tx = tx_fn;
    s_packet_len = 0;
    nr_init();
    /* Defaults chosen in listening tests (computed exactly like the F2 command does). */
    s_shelf_gain = 0.0f;   /* fixed +6 dB high shelf replaced by the automatic equaliser (noise_reduce.c) */
    s_pre_gain = powf(10.0f, 6.0f / 20.0f);   /* +6 dB: +12 dB over-drove the encoder (Peanut test: 0 > +6 > +12 dB for quality, +6 dB matches other stations in loudness) */
}

void dv3000_protocol_reset_stream(void)
{
    s_packet_len = 0;
}

void dv3000_protocol_feed(const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t room = sizeof(s_packet) - s_packet_len;
        size_t take = len < room ? len : room;

        memcpy(&s_packet[s_packet_len], data, take);
        s_packet_len += take;
        data += take;
        len  -= take;

        /* Drain EVERY complete packet in the buffer, even when no input is pending:
         * several packets can arrive in a single USB/UART chunk. */
        for (;;) {
            size_t skip = 0;
            while (skip < s_packet_len && s_packet[skip] != START_BYTE) {
                ++skip;
            }
            if (skip) {
                memmove(s_packet, s_packet + skip, s_packet_len - skip);
                s_packet_len -= skip;
            }

            if (s_packet_len < 3) {
                break;
            }

            /* Once the type byte is here, reject impossible headers (a stray 0x61 in line noise
             * must not swallow the following packets). */
            if (s_packet_len >= 4 && s_packet[3] > TYPE_AUDIO) {
                memmove(s_packet, s_packet + 1, s_packet_len - 1);
                s_packet_len -= 1;
                continue;
            }

            size_t total = 4 + ((((size_t)s_packet[1] & LENGTH_MASK) << 8) | s_packet[2]);

            if (total > sizeof(s_packet)) {
                DV_LOGE("Invalid packet length %u", (unsigned)total);
                memmove(s_packet, s_packet + 1, s_packet_len - 1);   /* resync */
                s_packet_len -= 1;
                continue;
            }
            if (s_packet_len < total) {
                break;
            }

            process_packet(s_packet, total);

            memmove(s_packet, s_packet + total, s_packet_len - total);
            s_packet_len -= total;
        }
    }
}
