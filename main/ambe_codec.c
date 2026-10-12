#include "ambe_codec.h"

#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include <mbelib-neo/mbelib.h>

#if CONFIG_AMBE_BENCH_LOG
#include "esp_timer.h"
#endif

static const char *TAG = "ambe_codec";

static mbe_ambe2400_encoder *s_encoder;
static mbe_parms s_enc_cur, s_enc_prev, s_enc_prev_enhanced;
static mbe_parms s_dec_cur, s_dec_prev, s_dec_prev_enhanced;
static bool s_initialized;

#if CONFIG_AMBE_BENCH_LOG
typedef struct { uint64_t sum, max; uint32_t n; } bench_t;
static bench_t s_enc_b, s_dec_b;

static void bench_add(bench_t *b, const char *name, uint64_t us)
{
    b->sum += us;
    if (us > b->max) b->max = us;
    if (++b->n >= CONFIG_AMBE_BENCH_INTERVAL) {
        ESP_LOGI(TAG, "%s: avg %u us, max %u us over %u frames (real time needs < 20000 us)",
                 name, (unsigned)(b->sum / b->n), (unsigned)b->max, (unsigned)b->n);
        b->sum = b->max = 0; b->n = 0;
    }
}
#define BENCH_START()    int64_t t0_ = esp_timer_get_time()
#define BENCH_STOP(b, n) bench_add(&(b), (n), (uint64_t)(esp_timer_get_time() - t0_))
#else
#define BENCH_START()    do {} while (0)
#define BENCH_STOP(b, n) do {} while (0)
#endif

bool ambe_codec_init(void)
{
    if (s_initialized) {
        return true;
    }
    s_encoder = mbe_ambe2400EncoderAlloc();
    if (s_encoder == NULL) {
        ESP_LOGE(TAG, "mbe_ambe2400EncoderAlloc failed (out of memory?)");
        return false;
    }
    mbe_initMbeParms(&s_enc_cur, &s_enc_prev, &s_enc_prev_enhanced);
    mbe_initMbeParms(&s_dec_cur, &s_dec_prev, &s_dec_prev_enhanced);
    s_initialized = true;
    ESP_LOGI(TAG, "mbelib-neo %s", mbe_versionString());
    return true;
}

void ambe_codec_reset(void)
{
    if (!s_initialized) {
        return;
    }
    mbe_ambe2400EncoderReset(s_encoder);
    mbe_initMbeParms(&s_enc_cur, &s_enc_prev, &s_enc_prev_enhanced);
    mbe_initMbeParms(&s_dec_cur, &s_dec_prev, &s_dec_prev_enhanced);
}

bool ambe_encode_dstar(const int16_t pcm[160], uint8_t out9[9])
{
    if (!s_initialized) {
        return false;
    }
    BENCH_START();

    char ambe_d[49];
    char ambe_fr[4][24];

    int rc = mbe_encodeAmbe2400ParmsShort(s_encoder, pcm, ambe_d, &s_enc_cur, &s_enc_prev);
    if (rc < 0) { ESP_LOGE(TAG, "parm encode: %d", rc); return false; }

    rc = mbe_encodeAmbe3600x2400Frame(ambe_d, ambe_fr);
    if (rc < 0) { ESP_LOGE(TAG, "FEC/frame encode: %d", rc); return false; }

    rc = mbe_encodeDStarDVData(ambe_fr, out9);
    if (rc < 0) { ESP_LOGE(TAG, "D-STAR pack: %d", rc); return false; }

    /* Documented upstream: advance the parameter history between frames. */
    mbe_moveMbeParms(&s_enc_cur, &s_enc_prev);

    BENCH_STOP(s_enc_b, "encode");
    return true;
}

bool ambe_decode_dstar(const uint8_t in9[9], int16_t pcm[160])
{
    if (!s_initialized) {
        return false;
    }
    BENCH_START();

    char ambe_fr[4][24];
    char ambe_d[49];
    mbe_process_result result;
    memset(&result, 0, sizeof(result));

    int rc = mbe_decodeDStarDVData(in9, ambe_fr);
    if (rc < 0) { ESP_LOGE(TAG, "D-STAR unpack: %d", rc); return false; }

    rc = mbe_processAmbe3600x2400Frame(pcm, &result, ambe_fr, ambe_d,
                                       &s_dec_cur, &s_dec_prev, &s_dec_prev_enhanced);
    if (rc < 0) { ESP_LOGE(TAG, "AMBE decode: %d", rc); return false; }

    BENCH_STOP(s_dec_b, "decode");
    return true;
}

void ambe_codec_last_decode(ambe_dec_info_t *out)
{
    /* processAmbe...Frame leaves the frame just decoded in s_dec_cur (copied to prev for the next call
     * inside mbelib, which keeps cur intact). */
    out->w0 = s_dec_cur.w0;
    out->gamma = s_dec_cur.gamma;
    out->L = s_dec_cur.L;
    out->voiced = 0;
    for (int l = 1; l <= s_dec_cur.L && l < 57; ++l) {
        out->voiced += s_dec_cur.Vl[l] ? 1 : 0;
        out->log2Ml[l] = s_dec_cur.log2Ml[l];
    }
}
