#include "m17_engine.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <codec2.h>

#include "audio_win.h"
#include "mic_win.h"
#include "wav.h"

#define CONN_RETRY_MS     5000
#define REFUSED_RETRY_MS  30000
#define LINK_TIMEOUT_MS   30000     /* reflectors PING every few seconds */
#define STREAM_TIMEOUT_MS 1000      /* one packet per 40 ms; a call is over after this much silence */

static me_cfg_t s_cfg;
static me_log_fn s_log;
static HANDLE s_thread;
static volatile LONG s_stop, s_ptt, s_running;
static CRITICAL_SECTION s_cs;
static me_status_t s_st;
static LARGE_INTEGER s_freq;
static struct CODEC2 *s_c3200_tx, *s_c3200_rx, *s_c1600_rx;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)s_freq.QuadPart;
}

static void logf_(const char *fmt, ...)
{
    char line[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(line, sizeof(line), "%02u:%02u:%02u  ", st.wHour % 24u, st.wMinute % 60u, st.wSecond % 60u);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (s_log) {
        s_log(line);
    }
}

static float level_dbfs(const int16_t *pcm, int n)
{
    double e = 0;
    for (int i = 0; i < n; ++i) {
        e += (double)pcm[i] * pcm[i];
    }
    double rms = sqrt(e / n);
    return rms < 1.0 ? -99.0f : (float)(20.0 * log10(rms / 32768.0));
}

static void apply_gain(int16_t *pcm, int n, float db)
{
    if (db == 0.0f) {
        return;
    }
    float g = powf(10.0f, db / 20.0f);
    for (int i = 0; i < n; ++i) {
        float v = pcm[i] * g;
        pcm[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
    }
}

void me_default_cfg(me_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->port = M17_PORT;
    c->module = 'A';
    c->mic_device = c->spk_device = -1;
    c->jitter_ms = 300;
    c->tot_s = 180;
}

static void set_link(me_link_t l)
{
    EnterCriticalSection(&s_cs);
    s_st.link = l;
    LeaveCriticalSection(&s_cs);
}

static void stamp_name(char *out, size_t size, const char *dir, const char *what)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[M17_CALL_LEN + 4];
    snprintf(name, sizeof(name), "%s", what[0] ? what : "unknown");
    for (char *c = name; *c; ++c) {
        if (*c == '/' || *c == '\\' || *c == ' ' || *c == '*' || *c == '?' || *c == '@' || *c == '.') {
            *c = '_';
        }
    }
    snprintf(out, size, "%s\\%04u%02u%02u_%02u%02u%02u_%s.wav", dir, st.wYear, st.wMonth % 13u, st.wDay % 32u,
             st.wHour % 24u, st.wMinute % 60u, st.wSecond % 60u, name);
}

/* ---------------------------------------------------------------- test transmission from a WAV file */
static int16_t *s_wav_pcm;
static size_t s_wav_n, s_wav_pos;

static bool load_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return false;
    }
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) {
            fclose(f);
            return false;
        }
        uint32_t len = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "data", 4) == 0) {
            s_wav_n = len / 2;
            s_wav_pcm = malloc(s_wav_n * 2);
            s_wav_n = fread(s_wav_pcm, 2, s_wav_n, f);
            fclose(f);
            return true;
        }
        fseek(f, (long)(len + (len & 1)), SEEK_CUR);
    }
}

/* ---------------------------------------------------------------- network */
static SOCKET s_sock = INVALID_SOCKET;
static struct sockaddr_in s_to;

static void udp_send(const uint8_t *p, size_t n)
{
    sendto(s_sock, (const char *)p, (int)n, 0, (struct sockaddr *)&s_to, sizeof(s_to));
}

static bool open_socket(void)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port[8];
    snprintf(port, sizeof(port), "%u", s_cfg.port ? s_cfg.port : M17_PORT);
    if (getaddrinfo(s_cfg.host, port, &hints, &res) != 0 || !res) {
        logf_("[E] cannot resolve %s", s_cfg.host);
        return false;
    }
    memcpy(&s_to, res->ai_addr, sizeof(s_to));
    freeaddrinfo(res);
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) {
        logf_("[E] no UDP socket");
        return false;
    }
    u_long nb = 1;
    ioctlsocket(s_sock, FIONBIO, &nb);
    char ip[32];
    inet_ntop(AF_INET, &s_to.sin_addr, ip, sizeof(ip));
    logf_("reflector %s = %s:%u, module %c, callsign %s", s_cfg.host, ip, ntohs(s_to.sin_port), s_cfg.module,
          s_cfg.callsign);
    return true;
}

/* ---------------------------------------------------------------- receiving a call */
static audio_out_t *s_spk;
static struct {
    bool active;
    uint16_t sid;
    char call[M17_CALL_LEN], dest[M17_CALL_LEN], type[6];
    double first, last;
    uint16_t next_fn;
    unsigned packets, lost, other, bad_crc;
    unsigned frames;                      /* 20 ms frames played */
    wav_t wav;
    audio_stats_t at_start;
} R;

static void play(int16_t *pcm, float *lv)
{
    apply_gain(pcm, 160, s_cfg.rx_gain_db);
    if (s_spk) {
        audio_push(s_spk, pcm);
    }
    wav_write(&R.wav, pcm, 160);
    float l = level_dbfs(pcm, 160);
    *lv = l > *lv ? l : *lv;
    R.frames++;
}

static void rx_finish(const char *why)
{
    if (!R.active) {
        return;
    }
    R.active = false;
    wav_close(&R.wav);
    unsigned under = 0;
    if (s_spk) {
        audio_end_of_call(s_spk);
        under = audio_stats(s_spk)->underruns - R.at_start.underruns;
    }
    float secs = R.frames ? R.frames * 0.02f : R.other * 0.04f;
    logf_("    end%s: %.1f s %s, packets %u, lost %u, CRC errors %u, underruns %u%s", why, secs, R.type, R.packets,
          R.lost, R.bad_crc, under, R.other ? "  [no voice in this stream]" : "");
    EnterCriticalSection(&s_cs);
    s_st.rx = false;
    s_st.rx_dbfs = -99;
    memmove(&s_st.heard[1], &s_st.heard[0], sizeof(me_heard_t) * (ME_HEARD_N - 1));
    SYSTEMTIME st;
    GetLocalTime(&st);
    me_heard_t *h = &s_st.heard[0];
    snprintf(h->time, sizeof(h->time), "%02u:%02u:%02u", st.wHour % 24u, st.wMinute % 60u, st.wSecond % 60u);
    snprintf(h->call, sizeof(h->call), "%s", R.call);
    snprintf(h->dest, sizeof(h->dest), "%s", R.dest);
    snprintf(h->type, sizeof(h->type), "%s", R.type);
    h->seconds = secs;
    if (s_st.heard_n < ME_HEARD_N) {
        s_st.heard_n++;
    }
    LeaveCriticalSection(&s_cs);
}

static void rx_start(const m17_stream_t *s, double t)
{
    snprintf(R.call, sizeof(R.call), "%s", s->src);
    snprintf(R.dest, sizeof(R.dest), "%s", s->dst);
    snprintf(R.type, sizeof(R.type), "%s", m17_type_name(s->type));
    R.active = true;
    R.sid = s->stream_id;
    R.first = R.last = t;
    R.next_fn = s->fn;
    R.packets = R.lost = R.other = R.bad_crc = R.frames = 0;
    if (s_spk) {
        R.at_start = *audio_stats(s_spk);
    }
    if (s_cfg.wav_dir[0]) {
        char path[420];
        stamp_name(path, sizeof(path), s_cfg.wav_dir, R.call);
        wav_open(&R.wav, path);
    }
    EnterCriticalSection(&s_cs);
    s_st.rx = true;
    snprintf(s_st.rx_call, sizeof(s_st.rx_call), "%s", R.call);
    snprintf(s_st.rx_dest, sizeof(s_st.rx_dest), "%s", R.dest);
    snprintf(s_st.rx_type, sizeof(s_st.rx_type), "%s", R.type);
    s_st.rx_lost_pct = 0;
    s_st.rx_calls_total++;
    LeaveCriticalSection(&s_cs);
    logf_("RX  %s  to %s  Codec 2 %s", R.call, R.dest, R.type);
}

static void rx_stream(const m17_pkt_t *k, double t)
{
    const m17_stream_t *s = &k->s;
    if (R.active && s->stream_id != R.sid) {
        if (t - R.last < STREAM_TIMEOUT_MS) {
            return;                               /* someone else while a call is playing */
        }
        rx_finish(" (lost)");
    }
    if (!R.active) {
        if (s->last) {
            return;                               /* tail of a stream we did not hear */
        }
        rx_start(s, t);
    }
    R.last = t;
    if (!k->crc_ok) {
        R.bad_crc++;
    }
    R.packets++;
    uint16_t gap = (uint16_t)((s->fn - R.next_fn) & 0x7FFF);
    if (gap > 0 && gap < 25) {                    /* missing packets: fill with silence so the timing stays */
        R.lost += gap;
        int16_t z[160] = { 0 };
        float dummy = -99;
        for (unsigned i = 0; i < gap * 2u; ++i) {
            play(z, &dummy);
        }
    }
    R.next_fn = (uint16_t)((s->fn + 1) & 0x7FFF);

    int16_t pcm[320];
    float lv = -99;
    if (s->type == M17_TYPE_V3200 || ((s->type & 1) && ((s->type >> 1) & 3) == 2)) {
        for (int i = 0; i < 2; ++i) {
            codec2_decode(s_c3200_rx, pcm, s->payload + 8 * i);
            play(pcm, &lv);
        }
    } else if ((s->type & 1) && ((s->type >> 1) & 3) == 3) {
        codec2_decode(s_c1600_rx, pcm, s->payload);    /* 40 ms */
        play(pcm, &lv);
        play(pcm + 160, &lv);
    } else {
        R.other++;
    }
    EnterCriticalSection(&s_cs);
    s_st.rx_dbfs = lv;
    s_st.rx_lost_pct = (R.packets + R.lost) ? 100u * R.lost / (R.packets + R.lost) : 0;
    LeaveCriticalSection(&s_cs);
    if (s->last) {
        rx_finish("");
    }
}

/* ---------------------------------------------------------------- transmitting */
static struct {
    bool active;
    m17_stream_t s;
    int n;                                /* 20 ms frames already in s.payload (0 or 1) */
    unsigned packets;
    double start, next;
    wav_t mic;
} T;

static void tx_send(bool last)
{
    uint8_t pkt[M17_STREAM_LEN];
    T.s.last = last;
    m17_build_stream(&T.s, pkt);
    udp_send(pkt, sizeof(pkt));
    T.s.fn = (uint16_t)((T.s.fn + 1) & 0x7FFF);
    T.n = 0;
    T.packets++;
    EnterCriticalSection(&s_cs);
    s_st.tx_packets_total++;
    LeaveCriticalSection(&s_cs);
}

static void tx_start(double t)
{
    if (R.active) {
        rx_finish(" (cut by our transmission)");
    }
    memset(&T.s, 0, sizeof(T.s));
    {   /* a new stream id each time (rand() would repeat: its state is per thread and not seeded here) */
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        uint32_t h = (uint32_t)q.QuadPart * 2654435761u ^ (uint32_t)(q.QuadPart >> 32) ^ GetTickCount();
        h ^= h >> 16;
        T.s.stream_id = (uint16_t)(h % 0xFFFE + 1);
    }
    snprintf(T.s.src, sizeof(T.s.src), "%s", s_cfg.callsign);
    snprintf(T.s.dst, sizeof(T.s.dst), "%s", s_cfg.dest[0] ? s_cfg.dest : "@ALL");
    T.s.type = M17_TYPE_V3200;
    T.active = true;
    T.n = 0;
    T.packets = 0;
    T.start = T.next = t;
    if (s_cfg.wav_dir[0] && !s_cfg.tx_wav[0]) {
        char path[420];
        stamp_name(path, sizeof(path), s_cfg.wav_dir, "tx_mic");
        wav_open(&T.mic, path);
    }
    EnterCriticalSection(&s_cs);
    s_st.tx = true;
    s_st.tx_seconds = 0;
    LeaveCriticalSection(&s_cs);
    logf_("TX  %s  to %s  Codec 2 3200  stream %04X", T.s.src, T.s.dst, T.s.stream_id);
}

static void tx_voice(const int16_t pcm[160], double t)
{
    wav_write(&T.mic, pcm, 160);
    codec2_encode(s_c3200_tx, T.s.payload + 8 * T.n, (short *)pcm);
    if (++T.n == 2) {
        tx_send(false);
    }
    float secs = (float)((t - T.start) / 1000.0);
    EnterCriticalSection(&s_cs);
    s_st.tx_seconds = secs;
    LeaveCriticalSection(&s_cs);
    if (s_cfg.tot_s && secs > (float)s_cfg.tot_s) {
        logf_("[W] transmit time-out (%u s): PTT released", s_cfg.tot_s);
        InterlockedExchange(&s_ptt, 0);
    }
}

static void tx_finish(double t)
{
    if (!T.active) {
        return;
    }
    short z[160] = { 0 };
    while (T.n < 2) {                             /* the last packet: rest of the voice, then silence */
        codec2_encode(s_c3200_tx, T.s.payload + 8 * T.n, z);
        T.n++;
    }
    tx_send(true);
    T.active = false;
    wav_close(&T.mic);
    logf_("    end of transmission: %.1f s, %u packets", (t - T.start) / 1000.0, T.packets);
    EnterCriticalSection(&s_cs);
    s_st.tx = false;
    LeaveCriticalSection(&s_cs);
}

/* ---------------------------------------------------------------- the thread */
static DWORD WINAPI run(LPVOID arg)
{
    (void)arg;
    timeBeginPeriod(1);
    memset(&R, 0, sizeof(R));
    memset(&T, 0, sizeof(T));
    if (!open_socket()) {
        set_link(ML_OFF);
        InterlockedExchange(&s_running, 0);
        timeEndPeriod(1);
        return 0;
    }
    s_spk = s_cfg.no_speaker ? NULL : audio_open(s_cfg.spk_device, s_cfg.jitter_ms);
    mic_in_t *mic = s_cfg.tx_wav[0] ? NULL : mic_open(s_cfg.mic_device);
    if (s_cfg.tx_wav[0] && !load_wav(s_cfg.tx_wav)) {
        logf_("[E] cannot read %s", s_cfg.tx_wav);
    }
    if (!s_cfg.no_speaker && !s_spk) {
        logf_("[W] no speaker: received calls are not played");
    }
    if (!s_cfg.tx_wav[0] && !mic) {
        logf_("[W] no microphone: transmitting is not possible");
    }

    uint8_t pkt[256];
    double t_conn = -1e9, t_rx = now_ms();
    me_link_t link = ML_CONNECTING;
    set_link(link);
    unsigned tries = 0;

    while (!s_stop) {
        double t = now_ms();
        if ((link == ML_CONNECTING && t - t_conn > CONN_RETRY_MS) || (link == ML_REFUSED && t - t_conn > REFUSED_RETRY_MS)) {
            udp_send(pkt, m17_conn(s_cfg.callsign, s_cfg.module, pkt));
            logf_("connecting ...");
            if (++tries == 3) {
                logf_("[W] no answer from the reflector for 15 s: it may be down or blocking us "
                      "(check the host and port, or pick another reflector)");
            }
            t_conn = t;
        }
        if (link == ML_UP && t - t_rx > LINK_TIMEOUT_MS) {
            logf_("[W] nothing from the reflector for 30 s: connecting again");
            link = ML_CONNECTING;
            set_link(link);
            tries = 0;
            t_conn = -1e9;
        }
        for (;;) {
            struct sockaddr_in from;
            int fl = sizeof(from);
            int n = recvfrom(s_sock, (char *)pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) {
                break;
            }
            if (from.sin_addr.s_addr != s_to.sin_addr.s_addr || from.sin_port != s_to.sin_port) {
                continue;
            }
            t_rx = t;
            m17_pkt_t k = m17_parse(pkt, (size_t)n);
            switch (k.kind) {
            case MP_ACKN:
                if (link != ML_UP) {
                    link = ML_UP;
                    set_link(link);
                    logf_("linked to %s module %c", s_cfg.host, s_cfg.module);
                }
                break;
            case MP_NACK:
                if (link != ML_REFUSED) {
                    logf_("[E] the reflector refused the link (NACK): check the callsign and the module; "
                          "trying again every 30 s");
                }
                link = ML_REFUSED;
                set_link(link);
                break;
            case MP_PING:
                udp_send(pkt, m17_pong(s_cfg.callsign, pkt));
                if (k.from[0]) {
                    EnterCriticalSection(&s_cs);
                    if (strcmp(s_st.reflector, k.from)) {
                        snprintf(s_st.reflector, sizeof(s_st.reflector), "%s", k.from);
                        LeaveCriticalSection(&s_cs);
                        logf_("reflector name: %s", k.from);
                    } else {
                        LeaveCriticalSection(&s_cs);
                    }
                }
                if (link == ML_CONNECTING) {      /* a PING means we are known: some reflectors skip ACKN on relink */
                    link = ML_UP;
                    set_link(link);
                    logf_("linked to %s module %c", s_cfg.host, s_cfg.module);
                }
                break;
            case MP_DISC:
                if (link == ML_UP) {
                    logf_("[W] the reflector dropped us (DISC): connecting again");
                    link = ML_CONNECTING;
                    set_link(link);
                    tries = 0;
                    t_conn = -1e9;
                }
                break;
            case MP_STREAM:
                if (!T.active && link == ML_UP) {
                    rx_stream(&k, t);
                }
                break;
            default:
                break;
            }
        }
        if (R.active && t - R.last > STREAM_TIMEOUT_MS) {
            rx_finish(" (no last packet)");
        }

        /* ---- PTT and transmitting */
        bool want = s_ptt != 0;
        if (want && !T.active) {
            if ((!mic && !s_wav_pcm) || link != ML_UP) {
                InterlockedExchange(&s_ptt, 0);
                want = false;
            } else {
                tx_start(t);
                if (mic) {
                    mic_flush(mic);
                }
                s_wav_pos = 0;
            }
        }
        int16_t pcm[160];
        bool have = false;
        if (mic) {
            have = mic_read(mic, pcm);
        } else if (T.active && s_wav_pcm && t >= T.next) {
            if (s_wav_pos + 160 <= s_wav_n) {
                memcpy(pcm, s_wav_pcm + s_wav_pos, sizeof(pcm));
                s_wav_pos += 160;
                have = true;
            } else {
                InterlockedExchange(&s_ptt, 0);    /* WAV finished: release PTT */
                want = false;
            }
            T.next += 20.0;
        }
        if (have) {
            apply_gain(pcm, 160, s_cfg.tx_gain_db);
            float lv = level_dbfs(pcm, 160);
            EnterCriticalSection(&s_cs);
            s_st.mic_dbfs = lv;
            LeaveCriticalSection(&s_cs);
        }
        if (T.active && have && want && s_ptt) {
            tx_voice(pcm, t);
        }
        if (T.active && (!want || !s_ptt || s_stop)) {
            tx_finish(t);
        }
        if (s_spk) {
            audio_pump(s_spk);
        }
        Sleep(1);
    }

    tx_finish(now_ms());
    rx_finish("");
    udp_send(pkt, m17_disc(s_cfg.callsign, pkt));
    logf_("unlinked");
    closesocket(s_sock);
    s_sock = INVALID_SOCKET;
    if (s_spk) {
        Sleep(audio_queued_ms(s_spk) > 2000 ? 2000 : audio_queued_ms(s_spk));
        audio_close(s_spk);
        s_spk = NULL;
    }
    mic_close(mic);
    free(s_wav_pcm);
    s_wav_pcm = NULL;
    set_link(ML_OFF);
    timeEndPeriod(1);
    InterlockedExchange(&s_running, 0);
    return 0;
}

static bool init_once(void)
{
    static bool done, ok;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        InitializeCriticalSection(&s_cs);
        QueryPerformanceFrequency(&s_freq);
        s_c3200_tx = codec2_create(CODEC2_MODE_3200);
        s_c3200_rx = codec2_create(CODEC2_MODE_3200);
        s_c1600_rx = codec2_create(CODEC2_MODE_1600);
        ok = s_c3200_tx && s_c3200_rx && s_c1600_rx;
        done = true;
    }
    return ok;
}

bool me_start(const me_cfg_t *c, me_log_fn log)
{
    if (!init_once() || s_running) {
        return false;
    }
    s_cfg = *c;
    if (s_cfg.module >= 'a' && s_cfg.module <= 'z') {
        s_cfg.module = (char)(s_cfg.module - 'a' + 'A');
    }
    if (s_cfg.module < 'A' || s_cfg.module > 'Z') {
        s_cfg.module = 'A';
    }
    s_log = log;
    memset(&s_st, 0, sizeof(s_st));
    s_st.mic_dbfs = s_st.rx_dbfs = -99;
    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_ptt, 0);
    InterlockedExchange(&s_running, 1);
    s_thread = CreateThread(NULL, 0, run, NULL, 0, NULL);
    if (s_thread) {
        SetThreadPriority(s_thread, THREAD_PRIORITY_TIME_CRITICAL);
    }
    return s_thread != NULL;
}

void me_stop(void)
{
    if (!s_thread) {
        return;
    }
    InterlockedExchange(&s_ptt, 0);
    InterlockedExchange(&s_stop, 1);
    WaitForSingleObject(s_thread, 5000);
    CloseHandle(s_thread);
    s_thread = NULL;
}

bool me_running(void)
{
    return s_running != 0;
}

void me_set_ptt(bool on)
{
    InterlockedExchange(&s_ptt, on ? 1 : 0);
}

void me_get_status(me_status_t *s)
{
    init_once();
    EnterCriticalSection(&s_cs);
    *s = s_st;
    LeaveCriticalSection(&s_cs);
}
