#pragma once
/* Microphone capture (Win32 waveIn, 8 kHz, 16 bit, mono) in 20 ms frames. Polled: call mic_read() often; the
 * sound card's clock paces the transmission (one frame every 20 ms). */
#include <stdbool.h>
#include <stdint.h>

typedef struct mic_in mic_in_t;

mic_in_t *mic_open(int device);         /* -1 = Windows' default recording device; NULL (message printed) on error */
void mic_close(mic_in_t *m);
void mic_flush(mic_in_t *m);            /* drop what was recorded so far (call when PTT is pressed) */
bool mic_read(mic_in_t *m, int16_t pcm[160]);   /* true when one more 20 ms frame was taken */
void mic_list_devices(void);            /* prints the recording devices (index and name) to stderr */
int mic_device_count(void);
bool mic_device_name(int index, char *name, int size);
