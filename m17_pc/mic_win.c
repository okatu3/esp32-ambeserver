#include "mic_win.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBUF 10                          /* 200 ms of recording in flight */

struct mic_in {
    HWAVEIN wi;
    WAVEHDR hdr[NBUF];
    int16_t buf[NBUF][160];
    int next;                            /* buffer that completes next (they come back in order) */
};

int mic_device_count(void)
{
    return (int)waveInGetNumDevs();
}

bool mic_device_name(int index, char *name, int size)
{
    WAVEINCAPSA caps;
    if (waveInGetDevCapsA((UINT)index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
        return false;
    }
    snprintf(name, (size_t)size, "%s", caps.szPname);
    return true;
}

void mic_list_devices(void)
{
    char name[64];
    for (int i = 0; i < mic_device_count(); ++i) {
        if (mic_device_name(i, name, sizeof(name))) {
            fprintf(stderr, "  %d: %s\n", i, name);
        }
    }
}

mic_in_t *mic_open(int device)
{
    mic_in_t *m = calloc(1, sizeof(*m));
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = 8000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = 16000;
    MMRESULT r = waveInOpen(&m->wi, device < 0 ? WAVE_MAPPER : (UINT)device, &fmt, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        char msg[128] = "";
        waveInGetErrorTextA(r, msg, sizeof(msg));
        fprintf(stderr, "[E] cannot open the microphone (device %d): %s\n", device, msg);
        free(m);
        return NULL;
    }
    for (int i = 0; i < NBUF; ++i) {
        m->hdr[i].lpData = (LPSTR)m->buf[i];
        m->hdr[i].dwBufferLength = sizeof(m->buf[i]);
        waveInPrepareHeader(m->wi, &m->hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(m->wi, &m->hdr[i], sizeof(WAVEHDR));
    }
    waveInStart(m->wi);
    return m;
}

void mic_close(mic_in_t *m)
{
    if (!m) {
        return;
    }
    waveInReset(m->wi);
    for (int i = 0; i < NBUF; ++i) {
        waveInUnprepareHeader(m->wi, &m->hdr[i], sizeof(WAVEHDR));
    }
    waveInClose(m->wi);
    free(m);
}

bool mic_read(mic_in_t *m, int16_t pcm[160])
{
    WAVEHDR *h = &m->hdr[m->next];
    if (!(h->dwFlags & WHDR_DONE)) {
        return false;
    }
    memcpy(pcm, m->buf[m->next], sizeof(m->buf[0]));
    h->dwFlags &= ~WHDR_DONE;
    h->dwBytesRecorded = 0;
    waveInAddBuffer(m->wi, h, sizeof(WAVEHDR));
    m->next = (m->next + 1) % NBUF;
    return true;
}

void mic_flush(mic_in_t *m)
{
    int16_t tmp[160];
    while (mic_read(m, tmp)) {
    }
}
