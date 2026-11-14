/* m17_test: checks of m17.c and of Codec 2 as m17_pc uses it. Exit code 0 when all pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <codec2.h>

#include "m17.h"

static int s_fail;

#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } \
                              else { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* CRC test vectors of the M17 specification */
    CHECK(m17_crc((const uint8_t *)"", 0) == 0xFFFF, "CRC of nothing = FFFF (%04X)", m17_crc((const uint8_t *)"", 0));
    CHECK(m17_crc((const uint8_t *)"A", 1) == 0x206E, "CRC of \"A\" = 206E (%04X)", m17_crc((const uint8_t *)"A", 1));
    CHECK(m17_crc((const uint8_t *)"123456789", 9) == 0x772B, "CRC of \"123456789\" = 772B (%04X)",
          m17_crc((const uint8_t *)"123456789", 9));
    {
        uint8_t all[256];
        for (int i = 0; i < 256; ++i) {
            all[i] = (uint8_t)i;
        }
        CHECK(m17_crc(all, 256) == 0x1C31, "CRC of bytes 00..FF = 1C31 (%04X)", m17_crc(all, 256));
    }

    /* base-40 callsigns */
    {
        uint8_t b[6];
        m17_encode_call("AB1CD", b);       /* example of the specification: 0x0000009FDD51 */
        CHECK(!memcmp(b, "\x00\x00\x00\x9F\xDD\x51", 6), "AB1CD = 00 00 00 9F DD 51 (%02X %02X %02X %02X %02X %02X)",
              b[0], b[1], b[2], b[3], b[4], b[5]);
        const char *calls[] = { "N0CALL", "M17-XXX A", "TEST/P", "A", "Z9Z9Z9Z9Z", "@ALL" };
        for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i) {
            char back[M17_CALL_LEN];
            m17_encode_call(calls[i], b);
            m17_decode_call(b, back);
            CHECK(!strcmp(back, calls[i]), "callsign \"%s\" comes back as \"%s\"", calls[i], back);
        }
        m17_encode_call("@ALL", b);
        CHECK(!memcmp(b, "\xFF\xFF\xFF\xFF\xFF\xFF", 6), "@ALL = FF FF FF FF FF FF");
    }

    /* control packets */
    {
        uint8_t p[16];
        CHECK(m17_conn("N0CALL", 'C', p) == 11 && !memcmp(p, "CONN", 4) && p[10] == 'C', "CONN is 11 bytes, module last");
        CHECK(m17_pong("N0CALL", p) == 10 && !memcmp(p, "PONG", 4), "PONG is 10 bytes");
        CHECK(m17_disc("N0CALL", p) == 10 && !memcmp(p, "DISC", 4), "DISC is 10 bytes");
        CHECK(m17_parse((const uint8_t *)"ACKN", 4).kind == MP_ACKN, "ACKN parsed");
        CHECK(m17_parse((const uint8_t *)"NACK", 4).kind == MP_NACK, "NACK parsed");
        uint8_t ping[10] = "PING";
        m17_encode_call("M17-TST", ping + 4);
        m17_pkt_t k = m17_parse(ping, 10);
        CHECK(k.kind == MP_PING && !strcmp(k.from, "M17-TST"), "PING parsed with the reflector's name (%s)", k.from);
    }

    /* stream packet */
    {
        m17_stream_t s;
        memset(&s, 0, sizeof(s));
        s.stream_id = 0xBEEF;
        strcpy(s.dst, "@ALL");
        strcpy(s.src, "N0CALL");
        s.type = M17_TYPE_V3200;
        s.fn = 0x1234;
        s.last = true;
        for (int i = 0; i < 16; ++i) {
            s.payload[i] = (uint8_t)(i * 17);
        }
        uint8_t p[M17_STREAM_LEN];
        m17_build_stream(&s, p);
        m17_pkt_t k = m17_parse(p, sizeof(p));
        CHECK(k.kind == MP_STREAM && k.crc_ok, "stream packet: 54 bytes, CRC good");
        CHECK(k.s.stream_id == 0xBEEF && k.s.fn == 0x1234 && k.s.last && k.s.type == M17_TYPE_V3200 &&
              !strcmp(k.s.src, "N0CALL") && !strcmp(k.s.dst, "@ALL") && !memcmp(k.s.payload, s.payload, 16),
              "stream packet fields come back");
        CHECK(p[34] == 0x92 && p[35] == 0x34, "FN 1234 with the last-packet bit = 92 34");
        p[40] ^= 1;
        CHECK(!m17_parse(p, sizeof(p)).crc_ok, "a flipped payload bit fails the CRC");
        CHECK(!strcmp(m17_type_name(M17_TYPE_V3200), "3200") && !strcmp(m17_type_name(M17_TYPE_V1600), "1600"),
              "type names 3200 / 1600");
    }

    /* Codec 2 sizes as the packet needs them */
    {
        struct CODEC2 *c3 = codec2_create(CODEC2_MODE_3200), *c1 = codec2_create(CODEC2_MODE_1600);
        CHECK(c3 && codec2_samples_per_frame(c3) == 160 && codec2_bytes_per_frame(c3) == 8,
              "Codec 2 3200: 160 samples -> 8 bytes (two per packet)");
        CHECK(c1 && codec2_samples_per_frame(c1) == 320 && codec2_bytes_per_frame(c1) == 8,
              "Codec 2 1600: 320 samples -> 8 bytes (one per packet)");
        /* round trip of a 200 Hz tone with harmonics: the output must carry energy */
        short in[160], out[160];
        unsigned char bits[8];
        double e = 0;
        for (int f = 0; f < 50; ++f) {
            for (int i = 0; i < 160; ++i) {
                int n = f * 160 + i;
                in[i] = (short)(3000 * ((n % 40) < 20 ? 1 : -1));
            }
            codec2_encode(c3, bits, in);
            codec2_decode(c3, out, bits);
            if (f >= 10) {
                for (int i = 0; i < 160; ++i) {
                    e += (double)out[i] * out[i];
                }
            }
        }
        CHECK(e / (40 * 160) > 1e5, "Codec 2 3200 round trip of a buzz is not silent (mean square %.0f)", e / (40 * 160));
        codec2_destroy(c3);
        codec2_destroy(c1);
    }

    printf(s_fail ? "%d check(s) FAILED\n" : "all checks passed\n", s_fail);
    return s_fail ? 1 : 0;
}
