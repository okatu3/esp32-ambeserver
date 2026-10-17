/* DV3000 (AMBE-3000R) serial packet protocol, D-STAR mode only:
 *   0x61 | length (2 bytes, big endian) | type | payload
 * type 0x00 control, 0x01 AMBE channel data, 0x02 speech (160 x int16 big endian).
 * Speech goes straight to the encoder: only the PKT_GAIN gains are applied, as the real chip does. */
#include "dv3000_protocol.h"

#include <math.h>
#include <string.h>
#include "ambe_codec.h"

#define START_BYTE        0x61
#define TYPE_CONTROL      0x00
#define TYPE_AMBE         0x01
#define TYPE_AUDIO        0x02

#define CTRL_RATEP        0x0A
#define CTRL_PRODID       0x30
#define CTRL_VERSTRING    0x31
#define CTRL_RESET        0x33
#define CTRL_RESETSOFTCFG 0x34
#define CTRL_GETCFG       0x36   /* BlueDV sends it every 5 s as a keepalive */
#define CTRL_READCFG      0x37   /* BlueDV sends it once after a transmission */
#define CTRL_READY        0x39
#define CTRL_GAIN         0x4B   /* int8 input gain, int8 output gain, dB */

#define DSTAR_BLOCK       9
#define AUDIO_SAMPLES     160
#define AUDIO_BYTES       320
#define MAX_PACKET        400
#define LENGTH_MASK       0x0F

/* RATEP payload (after the command byte) that selects D-STAR 2400+1200 FEC. */
static const uint8_t DSTAR_FEC_RATEP[12] = {
    0x01, 0x30, 0x07, 0x63, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48
};

/* Standard D-STAR "null" AMBE frame, returned if encoding fails so the host never stalls. */
static const uint8_t DSTAR_SILENCE[DSTAR_BLOCK] = {
    0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8
};

static dv3000_tx_fn s_tx;
static float s_in_gain = 1.0f, s_out_gain = 1.0f;
static uint8_t s_packet[MAX_PACKET];
static size_t s_packet_len;
static uint8_t s_txbuf[6 + AUDIO_BYTES];

/* Noise gate: mbelib-neo encodes background noise as voice, heard as a warble ("gyuun") between words. The
 * floor of the frame RMS is tracked (falls fast, rises slowly); a frame passes only when it is clearly above it
 * and a hang-over keeps word endings. Closed frames are sent as the standard AMBE silence frame at once. */
#define GATE_RATIO     2.0f        /* open above 2 x floor (+6 dB) */
#define GATE_MIN_RMS   60.0f       /* never open below this (digital silence / dither) */
#define GATE_HANG      10          /* frames (200 ms) kept open after the last loud frame */
static float s_floor;              /* 0 = not started */
static int s_gate_hang;
static bool s_was_open;            /* gate state of the previous frame */
static bool s_pitch_locked;        /* a clearly periodic frame was seen since the gate opened */
static int16_t s_prev_pcm[AUDIO_SAMPLES];

#define VOICED_PERIODICITY 0.6f
#define PRIME_TONE_RUNS    3       /* dry runs on the tone (cold start + 2 to settle) */

/* Pitch period by normalised autocorrelation over the encoder's range (lags 20..127 = 400..63 Hz), on the
 * previous + current frame. Returns the peak correlation (near 1 for a vowel, low for breath, fricatives and
 * noise) and in *lag_out the shortest lag whose peak is within 5 % of the best (the period, not a multiple). */
static float periodicity(const int16_t pcm[AUDIO_SAMPLES], int *lag_out)
{
    float buf[2 * AUDIO_SAMPLES];
    static float c[128];
    for (int i = 0; i < AUDIO_SAMPLES; ++i) {
        buf[i] = s_prev_pcm[i];
        buf[AUDIO_SAMPLES + i] = pcm[i];
    }
    const int n = 2 * AUDIO_SAMPLES;
    float best = 0.0f;
    for (int lag = 20; lag <= 127; ++lag) {
        float r = 0.0f, e1 = 1.0f, e2 = 1.0f;
        for (int i = 0; i + lag < n; ++i) {
            r += buf[i] * buf[i + lag];
            e1 += buf[i] * buf[i];
            e2 += buf[i + lag] * buf[i + lag];
        }
        c[lag] = r / sqrtf(e1 * e2);
        if (c[lag] > best) best = c[lag];
    }
    *lag_out = 0;
    for (int lag = 21; lag <= 126; ++lag) {
        if (c[lag] >= 0.95f * best && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) {
            *lag_out = lag;
            break;
        }
    }
    return best;
}

/* Puts the encoder's pitch memory on `lag` before a word's first frame is encoded. Cold, the encoder prefers the
 * shortest period and takes 2 x the first formant (~350 Hz) for the pitch of a low voice; from there it can move
 * only 25 % per frame (the 400 -> 100 Hz glide heard as a chirp), and dry runs on the voice itself stay there.
 * A pure tone at the measured period has a single AMDF minimum, so the encoder locks on it; one dry run on the
 * real frame then refills its history with speech. */
static void prime_pitch(const int16_t pcm[AUDIO_SAMPLES], int lag, int runs, const int16_t history[AUDIO_SAMPLES])
{
    float e = 0.0f;
    for (int i = 0; i < AUDIO_SAMPLES; ++i) {
        e += (float)pcm[i] * (float)pcm[i];
    }
    float a = sqrtf(2.0f * e / AUDIO_SAMPLES);       /* same RMS as the frame (keeps the encoder AGC in place) */
    int16_t tone[AUDIO_SAMPLES];
    static int phase;
    for (int r = 0; r < runs; ++r) {
        for (int i = 0; i < AUDIO_SAMPLES; ++i, ++phase) {
            tone[i] = (int16_t)lrintf(a * sinf(6.2831853f * (float)(phase % lag) / (float)lag));
        }
        ambe_codec_prime_encoder(tone, 1);
    }
    ambe_codec_prime_encoder(history, 1);
}
static bool gate_open(const int16_t pcm[AUDIO_SAMPLES])
{
    float e = 0.0f;
    for (int i = 0; i < AUDIO_SAMPLES; ++i) {
        e += (float)pcm[i] * (float)pcm[i];
    }
    float rms = sqrtf(e / AUDIO_SAMPLES);
    if (s_floor <= 0.0f) {
        s_floor = GATE_MIN_RMS;    /* start low: learning from the first frame would mute speech at the start */
    }
    if (rms < s_floor) {
        s_floor = s_floor * 0.9f + rms * 0.1f;
    } else {
        s_floor *= 1.01f;          /* +4 dB/s: a vowel lifts it by < 1.5 dB */
    }
    if (rms > s_floor * GATE_RATIO && rms > GATE_MIN_RMS) {
        s_gate_hang = GATE_HANG;
    } else if (s_gate_hang > 0) {
        s_gate_hang--;
    }
    return s_gate_hang > 0;
}

static float db_to_lin(int8_t db)
{
    return powf(10.0f, (float)db / 20.0f);
}

static int16_t scale(int16_t v, float g)
{
    if (g == 1.0f) {
        return v;
    }
    float x = (float)v * g;
    return (int16_t)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : lrintf(x));
}

static void send_packet(uint8_t type, size_t payload_len)
{
    s_txbuf[0] = START_BYTE;
    s_txbuf[1] = (uint8_t)(payload_len >> 8);
    s_txbuf[2] = (uint8_t)payload_len;
    s_txbuf[3] = type;
    if (s_tx) {
        s_tx(s_txbuf, 4 + payload_len);
    }
}

static void send_control(uint8_t command, const uint8_t *payload, size_t len)
{
    s_txbuf[4] = command;
    if (len) {
        memcpy(&s_txbuf[5], payload, len);
    }
    send_packet(TYPE_CONTROL, 1 + len);
}

static void handle_control(const uint8_t *p, size_t n)
{
    static const uint8_t ok = 0x00;

    if (n < 1) {
        return;
    }
    switch (p[0]) {
    case CTRL_PRODID: {
        static const uint8_t id[] = "AMBE3000R";               /* incl. NUL, like the chip */
        send_control(CTRL_PRODID, id, sizeof(id));
        break;
    }
    case CTRL_VERSTRING: {
        static const uint8_t ver[] = "ESP32 AMBE3000 compatible (USB, mbelib-neo)";
        send_control(CTRL_VERSTRING, ver, sizeof(ver));
        break;
    }
    case CTRL_RESET:
    case CTRL_RESETSOFTCFG:
        s_in_gain = s_out_gain = 1.0f;
        s_floor = 0.0f;
        s_gate_hang = 0;
        s_was_open = false;
        s_pitch_locked = false;
        memset(s_prev_pcm, 0, sizeof(s_prev_pcm));
        ambe_codec_reset();
        send_control(CTRL_READY, NULL, 0);
        break;
    case CTRL_RATEP:
        /* Only D-STAR is supported: other modes get no answer rather than a false OK. */
        if (n - 1 == sizeof(DSTAR_FEC_RATEP) && memcmp(p + 1, DSTAR_FEC_RATEP, sizeof(DSTAR_FEC_RATEP)) == 0) {
            send_control(CTRL_RATEP, &ok, 1);
        }
        break;
    case CTRL_GAIN:
        if (n == 3) {
            s_in_gain = db_to_lin((int8_t)p[1]);
            s_out_gain = db_to_lin((int8_t)p[2]);
            send_control(CTRL_GAIN, &ok, 1);
        }
        break;
    case CTRL_GETCFG:
    case CTRL_READCFG: {
        /* The real chip returns its power-up configuration; the DV3000 values are not known, so zeros. */
        static const uint8_t cfg[3] = { 0x00, 0x00, 0x00 };
        send_control(p[0], cfg, sizeof(cfg));
        break;
    }
    default:
        break;
    }
}

static void handle_audio(const uint8_t *p, size_t n)
{
    if (n != 2 + AUDIO_BYTES || p[0] != 0x00 || p[1] != 0xA0) {
        return;
    }
    int16_t pcm[AUDIO_SAMPLES];
    for (int i = 0; i < AUDIO_SAMPLES; ++i) {
        pcm[i] = scale((int16_t)(((uint16_t)p[2 + i * 2] << 8) | p[3 + i * 2]), s_in_gain);
    }
    s_txbuf[4] = 0x01;              /* channel data field */
    s_txbuf[5] = 0x48;              /* 72 bits */
    bool open = gate_open(pcm);
    if (!open) {
        /* Send silence at once and keep the encoder out of the gap: fed zeros, its pitch search latches on the
         * shortest lag (400 Hz), and as it moves at most 25 % per frame every word then started with a
         * 400 -> 160 Hz glide over 5 frames, heard as a chirp. */
        memcpy(&s_txbuf[6], DSTAR_SILENCE, DSTAR_BLOCK);
    } else {
        if (!s_was_open) {
            s_pitch_locked = false;
        }
        if (!s_pitch_locked) {
            int lag;
            bool voiced = periodicity(pcm, &lag) >= VOICED_PERIODICITY && lag > 0;
            /* Until the first clearly periodic frame (a word starting with breath or a consonant), restart the
             * encoder every frame: otherwise it settles on 400 Hz in the aperiodic part and glides down from
             * there. On the first voiced frame its pitch memory is set to the measured period. */
            ambe_codec_reset_encoder();
            s_pitch_locked = voiced;
            if (voiced) {
                prime_pitch(pcm, lag, PRIME_TONE_RUNS, pcm);
            }
        }

        if (!ambe_encode_dstar(pcm, &s_txbuf[6])) {
            memcpy(&s_txbuf[6], DSTAR_SILENCE, DSTAR_BLOCK);
        }
    }
    s_was_open = open;
    memcpy(s_prev_pcm, pcm, sizeof(s_prev_pcm));
    send_packet(TYPE_AMBE, 2 + DSTAR_BLOCK);
}

static void handle_ambe(const uint8_t *p, size_t n)
{
    if (n != 2 + DSTAR_BLOCK || p[0] != 0x01 || p[1] != 0x48) {
        return;
    }
    int16_t pcm[AUDIO_SAMPLES];
    if (!ambe_decode_dstar(&p[2], pcm)) {
        memset(pcm, 0, sizeof(pcm));
    }
    s_txbuf[4] = 0x00;              /* speech data field */
    s_txbuf[5] = 0xA0;              /* 160 samples */
    for (int i = 0; i < AUDIO_SAMPLES; ++i) {
        uint16_t v = (uint16_t)scale(pcm[i], s_out_gain);
        s_txbuf[6 + i * 2] = (uint8_t)(v >> 8);
        s_txbuf[7 + i * 2] = (uint8_t)v;
    }
    send_packet(TYPE_AUDIO, 2 + AUDIO_BYTES);
}

void dv3000_protocol_init(dv3000_tx_fn tx_fn)
{
    s_tx = tx_fn;
    s_packet_len = 0;
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
        len -= take;

        /* Drain every complete packet: one UART chunk can hold several. */
        for (;;) {
            size_t skip = 0;
            while (skip < s_packet_len && s_packet[skip] != START_BYTE) {
                ++skip;
            }
            if (skip) {
                memmove(s_packet, s_packet + skip, s_packet_len - skip);
                s_packet_len -= skip;
            }
            if (s_packet_len < 4) {
                break;
            }
            size_t total = 4 + ((((size_t)s_packet[1] & LENGTH_MASK) << 8) | s_packet[2]);
            if (s_packet[3] > TYPE_AUDIO || total > sizeof(s_packet)) {
                /* not a real header (a stray 0x61): resync on the next byte */
                memmove(s_packet, s_packet + 1, s_packet_len - 1);
                s_packet_len -= 1;
                continue;
            }
            if (s_packet_len < total) {
                break;
            }
            switch (s_packet[3]) {
            case TYPE_CONTROL: handle_control(s_packet + 4, total - 4); break;
            case TYPE_AMBE:    handle_ambe(s_packet + 4, total - 4);    break;
            case TYPE_AUDIO:   handle_audio(s_packet + 4, total - 4);   break;
            }
            memmove(s_packet, s_packet + total, s_packet_len - total);
            s_packet_len -= total;
        }
    }
}
