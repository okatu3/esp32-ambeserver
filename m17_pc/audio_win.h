#pragma once
/* Speaker output for received calls (Win32 waveOut, 8 kHz, 16 bit, mono) with a jitter buffer: the network
 * delivers voice in bursts and with gaps, so playback
 * starts only when `prebuffer_ms` of audio is queued, then runs at the sound card's own 20 ms pace.
 * Adaptive playout: a call can also arrive slower than real time on average (a real call once needed
 * 17.0 s for 15.2 s of voice), which no fixed buffer survives. So when the buffer runs low, a quiet frame (pause
 * between words) is played again (up to 4 times when the buffer is very low); when it grows far beyond the target, quiet frames are left out. Speech
 * frames are never repeated or dropped.
 * Single-threaded: call audio_pump() often (every few ms) from the same loop that pushes frames. */
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_FRAME 160                     /* samples per 20 ms frame */

typedef struct audio_out audio_out_t;

typedef struct {
    uint32_t frames_played, underruns;      /* underrun: the queue ran dry in the middle of a call */
    uint32_t max_queue_ms;                  /* deepest the buffer got */
    uint32_t dropped;                       /* frames thrown away because the buffer was full */
    uint32_t stretched, skipped;            /* extra plays of quiet frames / quiet frames left out, to follow the sender's pace */
} audio_stats_t;

/* device: -1 = Windows' default output. Returns NULL (and prints why) on failure. */
audio_out_t *audio_open(int device, unsigned prebuffer_ms);
void audio_close(audio_out_t *a);

/* Queues one 20 ms frame (dropped, with a count, when more than 60 s are already waiting). */
void audio_push(audio_out_t *a, const int16_t pcm[AUDIO_FRAME]);
/* The call ended: play what is queued even if it is less than the prebuffer. */
void audio_end_of_call(audio_out_t *a);
/* Recycles finished buffers and hands queued frames to the sound card. */
void audio_pump(audio_out_t *a);

/* Milliseconds of audio queued or playing. */
unsigned audio_queued_ms(const audio_out_t *a);
const audio_stats_t *audio_stats(const audio_out_t *a);
/* Prints the output devices (index and name) to stderr. */
void audio_list_devices(void);
