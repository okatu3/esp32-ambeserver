#include "audio_win.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBUF       4                        /* frames handed to the sound card ahead (80 ms): the playout decisions
                                               below are taken close to the moment a frame is heard */
#define FIFO_LEN   3000                     /* frames waiting in the jitter buffer (60 s: a call arriving faster than real time is never cut) */
#define QUIET_RMS  400.0f                   /* below this (-38 dBFS) a frame is a pause that may be stretched or left out */

struct audio_out {
    HWAVEOUT wo;
    WAVEHDR hdr[NBUF];
    int16_t buf[NBUF][AUDIO_FRAME];
    bool busy[NBUF];                        /* handed to the sound card, not yet done */
    int16_t fifo[FIFO_LEN][AUDIO_FRAME];
    bool quiet[FIFO_LEN];
    unsigned head, count;                   /* fifo read position and fill */
    unsigned prebuffer;                     /* frames: start threshold and the depth playout aims for */
    bool playing, flushing;
    unsigned repeats;                       /* extra plays of the head frame so far (pause stretching) */
    audio_stats_t st;
};

static unsigned card_queue(const audio_out_t *a)
{
    unsigned n = 0;
    for (int i = 0; i < NBUF; ++i) {
        n += a->busy[i];
    }
    return n;
}

void audio_list_devices(void)
{
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        WAVEOUTCAPSA caps;
        if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            fprintf(stderr, "  %u: %s\n", i, caps.szPname);
        }
    }
}

audio_out_t *audio_open(int device, unsigned prebuffer_ms)
{
    audio_out_t *a = calloc(1, sizeof(*a));
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = 8000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = 16000;
    MMRESULT r = waveOutOpen(&a->wo, device < 0 ? WAVE_MAPPER : (UINT)device, &fmt, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        char msg[128] = "";
        waveOutGetErrorTextA(r, msg, sizeof(msg));
        fprintf(stderr, "[E] cannot open the speaker (device %d): %s\n", device, msg);
        free(a);
        return NULL;
    }
    for (int i = 0; i < NBUF; ++i) {
        a->hdr[i].lpData = (LPSTR)a->buf[i];
        a->hdr[i].dwBufferLength = sizeof(a->buf[i]);
        waveOutPrepareHeader(a->wo, &a->hdr[i], sizeof(WAVEHDR));
    }
    a->prebuffer = (prebuffer_ms + 19) / 20;
    if (a->prebuffer < 1) {
        a->prebuffer = 1;
    }
    return a;
}

void audio_close(audio_out_t *a)
{
    if (!a) {
        return;
    }
    waveOutReset(a->wo);
    for (int i = 0; i < NBUF; ++i) {
        waveOutUnprepareHeader(a->wo, &a->hdr[i], sizeof(WAVEHDR));
    }
    waveOutClose(a->wo);
    free(a);
}

void audio_push(audio_out_t *a, const int16_t pcm[AUDIO_FRAME])
{
    if (a->count == FIFO_LEN) {
        a->st.dropped++;
        return;
    }
    unsigned slot = (a->head + a->count) % FIFO_LEN;
    memcpy(a->fifo[slot], pcm, sizeof(a->fifo[0]));
    float e = 0.0f;
    for (int i = 0; i < AUDIO_FRAME; ++i) {
        e += (float)pcm[i] * (float)pcm[i];
    }
    a->quiet[slot] = e < QUIET_RMS * QUIET_RMS * AUDIO_FRAME;
    a->count++;
    a->flushing = false;
    unsigned q = audio_queued_ms(a);
    if (q > a->st.max_queue_ms) {
        a->st.max_queue_ms = q;
    }
}

void audio_end_of_call(audio_out_t *a)
{
    a->flushing = true;
}

void audio_pump(audio_out_t *a)
{
    for (int i = 0; i < NBUF; ++i) {
        if (a->busy[i] && (a->hdr[i].dwFlags & WHDR_DONE)) {
            a->busy[i] = false;
            a->st.frames_played++;
        }
    }
    unsigned on_card = card_queue(a);
    if (!a->playing) {
        if (a->count == 0 || (a->count < a->prebuffer && !a->flushing)) {
            return;                         /* still filling the jitter buffer */
        }
        a->playing = true;
    } else if (on_card == 0 && a->count == 0) {
        if (!a->flushing) {
            a->st.underruns++;              /* ran dry mid-call: refill the prebuffer before going on */
        }
        a->playing = false;
        return;
    }
    /* Below the target depth a pause frame is played twice, below half of it up to 4 times: a slow sender (seen:
     * 21.9 ms per frame, then a 226 ms gap in the middle of a sentence) must be caught up in the pauses
     * that come before the next long stretch of speech. Above 3 x target, pause frames are left out. */
    unsigned target = a->prebuffer > 2 ? a->prebuffer : 2;
    unsigned high = a->prebuffer * 3;
    for (int i = 0; i < NBUF && a->count > 0; ++i) {
        if (a->busy[i]) {
            continue;
        }
        unsigned total = a->count + card_queue(a);
        bool quiet = a->quiet[a->head] && !a->flushing;
        if (quiet && total > high && a->repeats == 0) {
            a->head = (a->head + 1) % FIFO_LEN;                   /* leave a pause frame out */
            a->count--;
            a->st.skipped++;
            --i;                                                  /* this sound card buffer is still free */
            continue;
        }
        memcpy(a->buf[i], a->fifo[a->head], sizeof(a->buf[i]));
        unsigned max_repeats = total * 2 <= target ? 3 : (total < target ? 1 : 0);
        if (quiet && a->repeats < max_repeats) {
            a->repeats++;                                         /* play this pause frame once more */
            a->st.stretched++;
        } else {
            a->repeats = 0;
            a->head = (a->head + 1) % FIFO_LEN;
            a->count--;
        }
        a->hdr[i].dwFlags &= ~WHDR_DONE;
        if (waveOutWrite(a->wo, &a->hdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            a->busy[i] = true;
        }
    }
}

unsigned audio_queued_ms(const audio_out_t *a)
{
    return (a->count + card_queue(a)) * 20;
}

const audio_stats_t *audio_stats(const audio_out_t *a)
{
    return &a->st;
}
