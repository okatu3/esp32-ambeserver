/* m17_rf_test: checks of m17_rf.c / m17_modem.c on the PC, then an end-to-end run:
 *   voice.wav (8 kHz mono) -> Codec 2 3200 -> M17 stream (preamble, LSF, stream frames, EoT) -> 4FSK 48 kHz
 *   -> channel (gain, polarity, DC, AC-coupling high-pass, noise) -> demodulator -> frames -> Codec 2 -> out.wav
 * usage: m17_rf_test [voice.wav [out.wav [baseband.wav]]] [--invert] [--dc X] [--hp HZ] [--snr DB] [--gain G]
 *   radio without a data port (fm_path.h): --radio  model of the TX audio path (IC-705 guess: HP 300 Hz 2nd order,
 *     pre-emphasis 750 us, limiter, LP 3 kHz) -> FM -> 12 kHz IF -> discriminator; --rx-af: the AF path instead of IF
 *     --r-hp HZ --r-hp-order N --r-preemph US --r-lp HZ --limit X (x +3 symbol) --offset HZ
 *     --precomp FLOOR_HZ  TX precompensation (inverse high-pass down to FLOOR_HZ, de-emphasis); --pre-gain dB
 *     --snr DB is then the carrier-to-noise ratio at the IF (or the SNR at the AF output)
 * Exit code 0 when every check passes and the payload comes back bit-exact (with the channel left clean). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <codec2.h>

#include "fm_path.h"
#include "m17.h"
#include "m17_modem.h"
#include "m17_rf.h"

static int s_fail;
#define CHECK(c, ...) do { printf((c) ? "ok    " : "FAIL  "); printf(__VA_ARGS__); printf("\n"); if (!(c)) s_fail++; } while (0)

/* ------------------------------------------------------------------ WAV */
static int16_t *read_wav(const char *path, int *rate, int *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4)) { fclose(f); return NULL; }
    int16_t *d = NULL;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) break;
        unsigned sz = c[4] | c[5] << 8 | c[6] << 16 | (unsigned)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            unsigned char b[16]; fread(b, 1, 16, f); if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
            *rate = b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24;
        } else if (!memcmp(c, "data", 4)) {
            d = malloc(sz); *n = (int)fread(d, 2, sz / 2, f); break;
        } else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    fclose(f);
    return d;
}

static void write_wav(const char *path, const int16_t *x, int n, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned ds = (unsigned)n * 2, v;
    fwrite("RIFF", 1, 4, f); v = 36 + ds; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    unsigned short s = 1; fwrite(&s, 2, 1, f); fwrite(&s, 2, 1, f);
    v = (unsigned)rate; fwrite(&v, 4, 1, f); v = (unsigned)rate * 2; fwrite(&v, 4, 1, f);
    s = 2; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f); fwrite(x, 2, (size_t)n, f); fclose(f);
}

/* ------------------------------------------------------------------ unit checks */
static uint32_t lcg = 12345;
static uint32_t rnd(void) { lcg = lcg * 1103515245u + 12345u; return lcg >> 8; }

static void unit_checks(void)
{
    /* Golay: rows of the spec's P matrix are the check+parity bits of single data bits (MSB first) */
    static const char *P[12] = { "110001110101", "011000111011", "111101101000", "011110110100",
                                 "001111011010", "110110011001", "011011001101", "001101100111",
                                 "110111000110", "101010010111", "100100111110", "100011101011" };
    int good = 0;
    for (int r = 0; r < 12; ++r) {
        uint32_t cw = m17_golay24_encode((uint16_t)(0x800 >> r));
        char s[13];
        for (int b = 0; b < 12; ++b) s[b] = (char)('0' + ((cw >> (11 - b)) & 1));
        s[12] = 0;
        good += !strcmp(s, P[r]);
    }
    CHECK(good == 12, "Golay(24,12) check bits = the spec's generator matrix rows (%d/12)", good);
    int fixed = 0;
    for (int t = 0; t < 2000; ++t) {
        uint16_t d = (uint16_t)(rnd() & 0xFFF), o;
        uint32_t cw = m17_golay24_encode(d);
        for (int e = 0; e < 3; ++e) cw ^= 1u << (rnd() % 24);
        fixed += m17_golay24_decode(cw, &o) >= 0 && o == d;
    }
    CHECK(fixed == 2000, "Golay corrects up to 3 bit errors (2000 random words)");
    CHECK(m17_interleave_index(1) == 137 && m17_interleave_index(2) == 90 && m17_interleave_index(367) == 47 &&
          m17_interleave_index(45) == 277, "interleaver = the spec's table (1->137, 2->90, 45->277, 367->47)");
    {
        static uint8_t seen[368];
        int dup = 0;
        for (int i = 0; i < 368; ++i) dup += seen[m17_interleave_index(i)]++;
        CHECK(!dup, "interleaver is a permutation of 368");
    }
    int p1 = 0, p2 = 0;
    for (int i = 0; i < 61; ++i) p1 += m17_p1[i];
    for (int i = 0; i < 12; ++i) p2 += m17_p2[i];
    CHECK(p1 == 46 && p2 == 11, "puncture P1 keeps 46/61, P2 keeps 11/12");
    {
        int8_t s[8];
        m17_rf_sync_symbols(M17_SYNC_LSF, s);
        CHECK(s[0] == 3 && s[1] == 3 && s[2] == 3 && s[3] == 3 && s[4] == -3 && s[5] == -3 && s[6] == 3 && s[7] == -3,
              "LSF sync 0x55F7 = +3 +3 +3 +3 -3 -3 +3 -3");
        m17_rf_sync_symbols(M17_SYNC_STREAM, s);
        CHECK(s[0] == -3 && s[3] == -3 && s[4] == 3 && s[5] == 3 && s[6] == -3 && s[7] == 3,
              "stream sync 0xFF5D = -3 -3 -3 -3 +3 +3 -3 +3");
    }
    /* LSF round trip through the symbol layer, with bit errors on the air */
    uint8_t lsf[30];
    memset(lsf, 0, sizeof(lsf));
    m17_encode_call("@ALL", lsf);
    m17_encode_call("N0CALL", lsf + 6);
    lsf[13] = 0x05;
    uint16_t crc = m17_crc(lsf, 28);
    lsf[28] = (uint8_t)(crc >> 8);
    lsf[29] = (uint8_t)crc;
    int8_t sym[192];
    m17_rf_lsf_frame(lsf, sym);
    /* the Viterbi decoder alone, rate 1/2 without puncturing: 240 bits, 488 coded, 10 random errors */
    {
        int okn = 0;
        for (int trial = 0; trial < 300; ++trial) {
            uint8_t b[240], c[488], o[240];
            uint16_t sf[488];
            for (int i = 0; i < 240; ++i) b[i] = (uint8_t)(rnd() & 1);
            m17_conv_encode(b, 240, c);
            for (int i = 0; i < 488; ++i) sf[i] = c[i] ? 65535 : 0;
            for (int e = 0; e < 10; ++e) sf[rnd() % 488] ^= 0xFFFF;
            m17_viterbi(sf, 488, o, 240);
            okn += !memcmp(b, o, 240);
        }
        CHECK(okn >= 297, "Viterbi, unpunctured r=1/2: 10 random errors in 488 bits -> %d / 300 decoded", okn);
    }
    /* hard-decision bit errors on the air: success rate over 300 frames per error count */
    static const int counts[5] = { 0, 5, 10, 15, 20 };
    int rate5 = 0;
    for (int c = 0; c < 5; ++c) {
        int okn = 0;
        for (int trial = 0; trial < 300; ++trial) {
            uint16_t soft[368];
            for (int k = 0; k < 184; ++k) {
                int s = sym[8 + k];
                soft[2 * k] = s < 0 ? 65535 : 0;
                soft[2 * k + 1] = (s == 3 || s == -3) ? 65535 : 0;
            }
            for (int e = 0; e < counts[c]; ++e) soft[rnd() % 368] ^= 0xFFFF;
            uint8_t back[30];
            okn += m17_rf_decode_lsf(soft, back, NULL) && !memcmp(back, lsf, 30);
        }
        printf("      LSF with %2d of 368 bits flipped (hard): %3d / 300 decoded\n", counts[c], okn);
        if (counts[c] == 5) rate5 = okn;
        if (counts[c] == 0) CHECK(okn == 300, "LSF frame decodes cleanly");
    }
    /* P1 leaves rate 0.66, so hard decisions fail sooner than in stream frames; soft decisions do better */
    CHECK(rate5 >= 285, "LSF frame survives 5 random hard bit errors (1.4 %%) in >= 95 %% of frames");
    uint8_t pl[16];
    for (int i = 0; i < 16; ++i) pl[i] = (uint8_t)(i * 37 + 5);
    m17_rf_stream_frame(lsf, 0x1234, true, pl, sym);
    {
        int okn = 0;
        for (int trial = 0; trial < 300; ++trial) {
            uint16_t soft[368];
            for (int k = 0; k < 184; ++k) {
                int s = sym[8 + k];
                soft[2 * k] = s < 0 ? 65535 : 0;
                soft[2 * k + 1] = (s == 3 || s == -3) ? 65535 : 0;
            }
            for (int e = 0; e < 8; ++e) soft[rnd() % 368] ^= 0xFFFF;
            m17_stream_rx_t r;
            m17_rf_decode_stream(soft, &r);
            okn += r.fn == 0x1234 && r.last && !memcmp(r.payload, pl, 16) && r.lich_cnt == 0x1234 % 6 &&
                   !memcmp(r.lich, lsf + 5 * (0x1234 % 6), 5);
        }
        CHECK(okn >= 285, "stream frame with 8 random hard bit errors: FN, last bit, payload, LICH ok in %d / 300", okn);
    }
}

/* ------------------------------------------------------------------ end to end */
typedef struct {
    uint8_t lsf[30];
    int lsf_ok, lsf_seen, frames, eot, lost;
    int fn_expect;
    uint8_t (*sent)[16];
    int nsent;
    int payload_bit_errors, frame_mismatch;
    float score_min, evm_max;
    struct CODEC2 *c2;
    int16_t *pcm;
    int pcm_n;
} rx_t;

static int popc8(uint8_t v) { int n = 0; while (v) { v &= v - 1; ++n; } return n; }

static void on_frame(void *ctx, const m17_sync_info_t *in, const uint16_t *soft)
{
    rx_t *r = ctx;
    if (getenv("M17_VERBOSE")) {
        static const char *nm[6] = { "LSF", "STREAM", "PACKET", "BERT", "EOT", "LOST" };
        printf("  frame %-6s at %8llu  score %.3f gain %8.1f dc %7.1f timing %+.2f evm %.3f\n", nm[in->type],
               (unsigned long long)in->at, in->score, in->gain, in->dc, in->timing, in->evm);
    }
    if (in->type == M17_FR_EOT) { r->eot++; return; }
    if (in->type == M17_FR_LOST) { r->lost++; return; }
    if (in->score < r->score_min) r->score_min = in->score;
    if (in->evm > r->evm_max) r->evm_max = in->evm;
    if (in->type == M17_FR_LSF) {
        r->lsf_seen++;
        uint8_t l[30];
        m17_rx_info_t info;
        bool crc = m17_rf_decode_lsf(soft, l, &info);
        if (crc && !memcmp(l, r->lsf, 30)) r->lsf_ok++;
        if (getenv("M17_VERBOSE")) {
            char src[10], dst[10];
            m17_decode_call(l, dst);
            m17_decode_call(l + 6, src);
            printf("        LSF CRC %s: %s -> %s type %02X%02X\n", crc ? "ok" : "BAD", src, dst, l[12], l[13]);
        }
        return;
    }
    if (in->type != M17_FR_STREAM) return;
    m17_stream_rx_t s;
    m17_rf_decode_stream(soft, &s);
    r->frames++;
    if (s.fn < r->nsent) {
        int be = 0;
        for (int i = 0; i < 16; ++i) be += popc8(s.payload[i] ^ r->sent[s.fn][i]);
        r->payload_bit_errors += be;
        r->frame_mismatch += be != 0;
    } else {
        r->frame_mismatch++;
    }
    for (int h = 0; h < 2; ++h) {
        codec2_decode(r->c2, r->pcm + r->pcm_n, s.payload + 8 * h);
        r->pcm_n += 160;
    }
}

int main(int argc, char **argv)
{
    int use_radio = 0, rx_af = 0;
    double limit = 1.5, offset = 0, pre_floor = 0, pre_db = 0;
    fmp_radio_t rr;
    fmp_radio_ic705_guess(&rr);
    const char *in_wav = NULL, *out_wav = NULL, *bb_wav = NULL;
    int invert = 0;
    double dc = 0, hp = 0, snr = 99, gain = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--invert")) invert = 1;
        else if (!strcmp(argv[i], "--dc") && i + 1 < argc) dc = atof(argv[++i]);
        else if (!strcmp(argv[i], "--hp") && i + 1 < argc) hp = atof(argv[++i]);
        else if (!strcmp(argv[i], "--snr") && i + 1 < argc) snr = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain = atof(argv[++i]);
        else if (!strcmp(argv[i], "--radio")) use_radio = 1;
        else if (!strcmp(argv[i], "--rx-af")) rx_af = 1;
        else if (!strcmp(argv[i], "--r-hp") && i + 1 < argc) rr.hp_hz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--r-hp-order") && i + 1 < argc) rr.hp_order = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--r-preemph") && i + 1 < argc) rr.preemph_us = atof(argv[++i]);
        else if (!strcmp(argv[i], "--r-lp") && i + 1 < argc) rr.lp_hz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atof(argv[++i]);
        else if (!strcmp(argv[i], "--offset") && i + 1 < argc) offset = atof(argv[++i]);
        else if (!strcmp(argv[i], "--precomp") && i + 1 < argc) pre_floor = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pre-gain") && i + 1 < argc) pre_db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pre-eps") && i + 1 < argc) fmp_pre_eps_hz = atof(argv[++i]);
        else if (!in_wav) in_wav = argv[i];
        else if (!out_wav) out_wav = argv[i];
        else bb_wav = argv[i];
    }
    m17_rf_init();
    if (argc > 2 && !strcmp(argv[1], "--rx")) {          /* demodulate a recorded 48 kHz baseband WAV */
        int rate = 0, n = 0;
        int16_t *x = read_wav(argv[2], &rate, &n);
        if (!x || rate != M17_FS) { printf("need a 48 kHz mono 16-bit WAV\n"); return 2; }
        double e = 0; int pk = 0;
        for (int i = 0; i < n; ++i) { e += (double)x[i] * x[i]; if (abs(x[i]) > pk) pk = abs(x[i]); }
        printf("%s: %.2f s, RMS %.1f dBFS, peak %d\n", argv[2], n / 48000.0, 10 * log10(e / n / 1073741824.0), pk);
        rx_t r;
        memset(&r, 0, sizeof(r));
        r.score_min = 9;
        r.c2 = codec2_create(CODEC2_MODE_3200);
        r.pcm = calloc((size_t)n / 6 + 640, 2);
        r.nsent = 0;
        _putenv("M17_VERBOSE=1");
        m17_demod_t *dm = malloc(sizeof(*dm));
        m17_demod_init(dm, on_frame, &r);
        for (int i = 0; i < n; i += 480) m17_demod_push(dm, x + i, (n - i) < 480 ? n - i : 480);
        printf("frames: LSF %d (%d CRC ok), stream %d, EoT %d, lost %d\n", r.lsf_seen, r.lsf_ok, r.frames, r.eot, r.lost);
        if (argc > 3) write_wav(argv[3], r.pcm, r.pcm_n, 8000);
        return 0;
    }
    unit_checks();
    if (!in_wav) {
        printf(s_fail ? "%d check(s) FAILED\n" : "all checks passed\n", s_fail);
        return s_fail != 0;
    }

    int rate = 0, n = 0;
    int16_t *voice = read_wav(in_wav, &rate, &n);
    if (!voice || rate != 8000) { printf("need an 8 kHz mono 16-bit WAV\n"); return 2; }
    int nfr = n / 320;                                 /* 40 ms per stream frame */
    struct CODEC2 *enc = codec2_create(CODEC2_MODE_3200);
    rx_t r;
    memset(&r, 0, sizeof(r));
    r.sent = calloc((size_t)nfr, 16);
    r.nsent = nfr;
    r.score_min = 9;
    r.c2 = codec2_create(CODEC2_MODE_3200);
    r.pcm = calloc((size_t)nfr * 320 + 320, 2);
    m17_encode_call("@ALL", r.lsf);
    m17_encode_call("N0CALL", r.lsf + 6);
    r.lsf[13] = 0x05;
    uint16_t crc = m17_crc(r.lsf, 28);
    r.lsf[28] = (uint8_t)(crc >> 8);
    r.lsf[29] = (uint8_t)crc;

    /* transmit: silence, preamble, LSF, frames, EoT, silence */
    int nsym = 192 * (nfr + 3) + 2 * 480;
    int8_t *sym = calloc((size_t)nsym, 1);
    int k = 480;                                       /* 100 ms of nothing first */
    m17_rf_preamble(sym + k); k += 192;
    m17_rf_lsf_frame(r.lsf, sym + k); k += 192;
    for (int f = 0; f < nfr; ++f) {
        codec2_encode(enc, r.sent[f], voice + 320 * f);
        codec2_encode(enc, r.sent[f] + 8, voice + 320 * f + 160);
        m17_rf_stream_frame(r.lsf, (uint16_t)f, f == nfr - 1, r.sent[f], sym + k);
        k += 192;
    }
    m17_rf_eot(sym + k); k += 192;
    k += 480;
    int16_t *bb = malloc((size_t)k * M17_SPS * 2);
    m17_mod_t mod;
    m17_mod_init(&mod, 12000);
    int ns = m17_mod(&mod, sym, k, bb);

    if (use_radio) {
        /* TX precompensation, the radio's audio path, FM, the receiver (IF + discriminator, or AF) */
        double pg = pow(10, pre_db / 20);
        if (pre_floor > 0) {
            fmp_pre_t pre;
            fmp_pre_init(&pre, &rr, pre_floor, pg);
            int pk0 = 0, pk1 = 0;
            for (int i = 0; i < ns; ++i) pk0 = abs(bb[i]) > pk0 ? abs(bb[i]) : pk0;
            fmp_pre_run(&pre, bb, ns);
            for (int i = 0; i < ns; ++i) pk1 = abs(bb[i]) > pk1 ? abs(bb[i]) : pk1;
            printf("precomp: floor %.1f Hz, inverse gain up to %.1f dB, gain %.1f dB, peak %d -> %d, clipped %u\n",
                   pre_floor, 20 * log10(fmp_pre_peak_gain(&rr, pre_floor)), pre_db, pk0, pk1, pre.clips);
        }
        fmp_model_t md;
        fmp_model_init(&md, &rr, limit, !rx_af, 1.0 / (12000.0 * (pre_floor > 0 ? pg : 1.0)));
        md.rx_offset_hz = offset;
        double nr = snr < 90 ? (rx_af ? FMP_NOMINAL * 0.6 : 8000.0 / sqrt(2.0)) * pow(10, -snr / 20) : 0;
        int16_t *o = malloc((size_t)ns * 2);
        unsigned seed = 12345;
        fmp_model_run(&md, bb, NULL, o, ns, nr, &seed);
        if (!rx_af) {
            fmp_ifdem_t dem;
            fmp_ifdem_init(&dem, FMP_IF_HZ);
            fmp_ifdem_run(&dem, o, bb, ns);
            printf("receiver: 12 kHz IF -> discriminator, offset seen %.0f Hz\n", fmp_ifdem_offset_hz(&dem));
        } else {
            memcpy(bb, o, (size_t)ns * 2);
            printf("receiver: AF path (de-emphasis, voice band)\n");
        }
        free(o);
        printf("radio: HP %.0f Hz order %d, pre-emphasis %.0f us, LP %.0f Hz, limiter %.2f (touched %.2f %%)\n",
               rr.hp_hz, rr.hp_order, rr.preemph_us, rr.lp_hz, limit, 100.0 * md.limited / ns);
        snr = 99;                                     /* the noise is in already */
    }

    /* channel */
    double hp_a = hp > 0 ? exp(-2 * M_PI * hp / M17_FS) : 0, hp_x = 0, hp_y = 0;
    double sig = 0;
    for (int i = 0; i < ns; ++i) sig += (double)bb[i] * bb[i];
    sig = sqrt(sig / ns);
    double nsd = snr < 90 ? sig * pow(10, -snr / 20) : 0;
    for (int i = 0; i < ns; ++i) {
        double v = bb[i] * gain * (invert ? -1 : 1) + dc;
        if (hp > 0) { double y = hp_a * (hp_y + v - hp_x); hp_x = v; hp_y = y; v = y; }
        if (nsd > 0) {
            double u1 = (rnd() + 1.0) / 16777217.0, u2 = (rnd() + 1.0) / 16777217.0;
            v += nsd * sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
        }
        bb[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    if (bb_wav) write_wav(bb_wav, bb, ns, M17_FS);

    /* receive in 10 ms blocks, like the I2S driver gives them */
    m17_demod_t *dm = malloc(sizeof(*dm));
    m17_demod_init(dm, on_frame, &r);
    for (int i = 0; i < ns; i += 480) m17_demod_push(dm, bb + i, (ns - i) < 480 ? ns - i : 480);
    if (out_wav) write_wav(out_wav, r.pcm, r.pcm_n, 8000);

    printf("channel: gain %.2f%s, DC %.0f, high-pass %.1f Hz, SNR %s\n", gain, invert ? ", inverted" : "", dc, hp,
           snr < 90 ? (char[16]){0} : "clean");
    if (snr < 90) printf("         SNR %.1f dB\n", snr);
    printf("sent: LSF + %d stream frames (%.2f s)\n", nfr, nfr * 0.04);
    printf("rx:   LSF %d seen / %d ok, %d stream frames, EoT %d, lost %d, worst sync %.3f, worst EVM %.3f\n",
           r.lsf_seen, r.lsf_ok, r.frames, r.eot, r.lost, r.score_min, r.evm_max);
    printf("      payload: %d frames differ, %d bit errors\n", r.frame_mismatch, r.payload_bit_errors);
    bool clean = snr >= 90;
    if (clean || snr >= 15) {
        CHECK(r.lsf_ok == 1 && r.frames == nfr && r.frame_mismatch == 0 && r.eot == 1,
              "end to end: LSF ok, all %d frames bit-exact, EoT seen", nfr);
    }
    printf(s_fail ? "%d check(s) FAILED\n" : "all checks passed\n", s_fail);
    return s_fail != 0;
}
