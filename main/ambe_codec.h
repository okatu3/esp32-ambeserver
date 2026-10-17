#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ambe_codec_init(void);
void ambe_codec_reset(void);
void ambe_codec_reset_encoder(void);     /* encoder only (decoder state kept) */
void ambe_codec_prime_encoder(const int16_t pcm[160], int times);   /* dry runs, output discarded */
bool ambe_encode_dstar(const int16_t pcm[160], uint8_t out9[9]);
bool ambe_decode_dstar(const uint8_t in9[9], int16_t pcm[160]);

#ifdef __cplusplus
}
#endif
