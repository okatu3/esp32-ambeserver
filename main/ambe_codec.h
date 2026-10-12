#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ambe_codec_init(void);
void ambe_codec_reset(void);
bool ambe_encode_dstar(const int16_t pcm[160], uint8_t out9[9]);
bool ambe_decode_dstar(const uint8_t in9[9], int16_t pcm[160]);

/* Diagnostics: parameters of the last decoded frame. */
typedef struct {
    float w0;              /* fundamental, radians/sample */
    float gamma;           /* gain */
    int L;                 /* number of harmonics */
    int voiced;            /* number of voiced harmonics */
    float log2Ml[57];      /* log2 spectral amplitudes, 1..L */
} ambe_dec_info_t;
void ambe_codec_last_decode(ambe_dec_info_t *out);

#ifdef __cplusplus
}
#endif
