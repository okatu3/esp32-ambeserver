/* D-STAR (AMBE 3600x2400) through the published mbelib-neo API, used exactly as upstream documents it. */
#include "ambe_codec.h"

#include <string.h>
#include <mbelib-neo/mbelib.h>

static mbe_ambe2400_encoder *s_encoder;
static mbe_parms s_enc_cur, s_enc_prev, s_enc_prev_enhanced;
static mbe_parms s_dec_cur, s_dec_prev, s_dec_prev_enhanced;

bool ambe_codec_init(void)
{
    if (s_encoder) {
        return true;
    }
    s_encoder = mbe_ambe2400EncoderAlloc();
    if (s_encoder == NULL) {
        return false;
    }
    mbe_initMbeParms(&s_enc_cur, &s_enc_prev, &s_enc_prev_enhanced);
    mbe_initMbeParms(&s_dec_cur, &s_dec_prev, &s_dec_prev_enhanced);
    return true;
}

void ambe_codec_reset(void)
{
    if (!s_encoder) {
        return;
    }
    mbe_ambe2400EncoderReset(s_encoder);
    mbe_initMbeParms(&s_enc_cur, &s_enc_prev, &s_enc_prev_enhanced);
    mbe_initMbeParms(&s_dec_cur, &s_dec_prev, &s_dec_prev_enhanced);
}

void ambe_codec_reset_encoder(void)
{
    if (!s_encoder) {
        return;
    }
    mbe_ambe2400EncoderReset(s_encoder);
    mbe_initMbeParms(&s_enc_cur, &s_enc_prev, &s_enc_prev_enhanced);
}

void ambe_codec_prime_encoder(const int16_t pcm[160], int times)
{
    /* Dry runs on one frame: the encoder's pitch moves at most 25 % per call towards the period it finds,
     * so a few calls let it reach the real pitch before the frame is encoded for real. The parameter history
     * (used for the differential quantisation) is left untouched. */
    if (!s_encoder) {
        return;
    }
    char ambe_d[49];
    static mbe_parms cur, prev;              /* static: mbe_parms is large for the task stack */
    cur = s_enc_cur;
    prev = s_enc_prev;
    for (int i = 0; i < times; ++i) {
        mbe_encodeAmbe2400ParmsShort(s_encoder, pcm, ambe_d, &cur, &prev);
    }
}

bool ambe_encode_dstar(const int16_t pcm[160], uint8_t out9[9])
{
    char ambe_d[49];
    char ambe_fr[4][24];

    if (!s_encoder ||
        mbe_encodeAmbe2400ParmsShort(s_encoder, pcm, ambe_d, &s_enc_cur, &s_enc_prev) < 0 ||
        mbe_encodeAmbe3600x2400Frame(ambe_d, ambe_fr) < 0 ||
        mbe_encodeDStarDVData(ambe_fr, out9) < 0) {
        return false;
    }
    mbe_moveMbeParms(&s_enc_cur, &s_enc_prev);      /* advance the parameter history between frames */
    return true;
}

bool ambe_decode_dstar(const uint8_t in9[9], int16_t pcm[160])
{
    char ambe_fr[4][24];
    char ambe_d[49];
    mbe_process_result result;
    memset(&result, 0, sizeof(result));

    if (!s_encoder || mbe_decodeDStarDVData(in9, ambe_fr) < 0) {
        return false;
    }
    return mbe_processAmbe3600x2400Frame(pcm, &result, ambe_fr, ambe_d,
                                         &s_dec_cur, &s_dec_prev, &s_dec_prev_enhanced) >= 0;
}
