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
static radio_t *s_radio;                  /* radio mode: the sound-card modem instead of the UDP socket */
static bool s_use_net;                        /* the reflector link is used (reflector or relay mode) */
static uint8_t s_sil3200[8];              /* Codec 2 3200 frame of silence (ends a relayed call cleanly) */

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
    radio_default_cfg(&c->rc);
    c->gw_rf_to_net = true;
    c->gw_cushion_ms = 200;
    c->gw_commands = c->gw_replies = true;
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

static bool resolve_to(const char *host, uint16_t port_no)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port[8];
    snprintf(port, sizeof(port), "%u", port_no ? port_no : M17_PORT);
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        logf_("[E] cannot resolve %s", host);
        return false;
    }
    memcpy(&s_to, res->ai_addr, sizeof(s_to));
    freeaddrinfo(res);
    return true;
}

static bool open_socket(void)
{
    if (s_cfg.host[0] && !resolve_to(s_cfg.host, s_cfg.port)) {
        return false;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) {
        logf_("[E] no UDP socket");
        return false;
    }
    u_long nb = 1;
    ioctlsocket(s_sock, FIONBIO, &nb);
    if (!s_cfg.host[0]) {
        logf_("no reflector yet: link from the radio with the destination \"M17-XXX A\"");
        return true;
    }
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
    bool from_rf;
} R;

/* ---------------------------------------------------------------- relay (gateway) */
static struct {
    bool up;                              /* this call: radio -> reflector */
    uint16_t up_sid, up_fn;
    unsigned up_packets;
    bool down;                            /* this call: reflector -> radio */
    uint16_t down_fn;
    unsigned down_packets;
    double start;
    unsigned under0;                      /* radio output underruns when the call started */
    int cmd;                              /* GC_*: this call is a command, not relayed */
    char cmd_refl[M17_CALL_LEN], cmd_mod;
} G;

/* ---------------------------------------------------------------- relay commands from the radio */
static void make_lsf(const char *dst, const char *src, uint16_t type, uint8_t lsf[M17_LSF_BYTES]);
enum { GC_NONE, GC_LINK, GC_UNLINK, GC_ECHO, GC_INFO };

static struct {
    int cmd;                              /* waiting to be carried out by the thread's loop */
    char refl[M17_CALL_LEN], mod, caller[M17_CALL_LEN];
    bool await;                           /* a commanded link waits for ACKN */
    double await_t;
    char name[M17_CALL_LEN];              /* the reflector we were told to link to, e.g. "M17-USA" */
    /* the answer: a short transmission */
    bool reply;
    double reply_at;
    char reply_dst[M17_CALL_LEN];
    uint8_t (*pl)[16];                    /* payloads (echo, or beeps) */
    int n, cap;
    uint16_t type;
    /* echo recording */
    int echo_n;
} C;

static int parse_cmd(const char *d, char *refl, char *mod)
{
    if (!_stricmp(d, "UNLINK") || !_stricmp(d, "U")) {
        return GC_UNLINK;
    }
    if (!_stricmp(d, "ECHO") || !_stricmp(d, "E")) {
        return GC_ECHO;
    }
    if (!_stricmp(d, "INFO") || !_stricmp(d, "I")) {
        return GC_INFO;
    }
    size_t n = strlen(d);
    if (n >= 7 && !_strnicmp(d, "M17-", 4) && (n == 7 || (n == 9 && d[7] == ' ' && d[8] >= 'A' && d[8] <= 'Z'))) {
        memcpy(refl, d, 7);
        refl[7] = 0;
        *mod = n == 9 ? d[8] : 'A';
        return GC_LINK;
    }
    return GC_NONE;
}

/* hosts file: "NAME HOST [PORT]  # comment" */
static bool hosts_lookup(const char *name, char *host, size_t hs, uint16_t *port)
{
    FILE *f = s_cfg.hosts_file[0] ? fopen(s_cfg.hosts_file, "r") : NULL;
    if (!f) {
        return false;
    }
    char line[512];
    bool ok = false;
    while (!ok && fgets(line, sizeof(line), f)) {
        char *h = strchr(line, '#');
        if (h) {
            *h = 0;
        }
        char nm[64], ho[128];
        unsigned p = 0;
        if (sscanf(line, "%63s %127s %u", nm, ho, &p) >= 2 && !_stricmp(nm, name)) {
            snprintf(host, hs, "%s", ho);
            *port = (uint16_t)(p ? p : M17_PORT);
            ok = true;
        }
    }
    fclose(f);
    return ok;
}

static struct CODEC2 *s_c3200_beep;

static void reply_reserve(int n)
{
    if (n > C.cap) {
        C.pl = realloc(C.pl, sizeof(*C.pl) * (size_t)n);
        C.cap = n;
    }
}

/* the answer to a command: beeps (count, pitch) with the result in the destination field */
static void reply_beeps(const char *dst, int count, float hz, double at)
{
    snprintf(C.reply_dst, sizeof(C.reply_dst), "%s", dst);
    static int16_t pcm[8000];             /* 1 s */
    memset(pcm, 0, sizeof(pcm));
    int on = count == 1 ? 2400 : 960, gap = 800;
    for (int b = 0; b < count; ++b) {
        int s0 = 800 + b * (on + gap);
        for (int i = 0; i < on && s0 + i < 8000; ++i) {
            float env = i < 80 ? i / 80.0f : (on - i < 80 ? (on - i) / 80.0f : 1.0f);
            pcm[s0 + i] = (int16_t)(9000.0f * env * sinf(2.0f * (float)M_PI * hz * (float)i / 8000.0f));
        }
    }
    reply_reserve(25);
    for (int f = 0; f < 25; ++f) {
        codec2_encode(s_c3200_beep, C.pl[f], pcm + 320 * f);
        codec2_encode(s_c3200_beep, C.pl[f] + 8, pcm + 320 * f + 160);
    }
    C.n = 25;
    C.type = M17_TYPE_V3200;
    C.reply = true;
    C.reply_at = at;
}

static void reply_send_now(void)
{
    uint8_t lsf[M17_LSF_BYTES];
    make_lsf(C.reply_dst, s_cfg.callsign, C.type, lsf);
    radio_tx_begin(s_radio, lsf);
    for (int f = 0; f < C.n; ++f) {
        radio_tx_frame(s_radio, (uint16_t)f, f + 1 == C.n, C.pl[f]);
    }
    radio_tx_end(s_radio);
    logf_("    answer on the radio: to \"%s\", %.1f s", C.reply_dst, C.n * 0.04);
    C.reply = false;
}

static uint16_t new_stream_id(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint32_t h = (uint32_t)q.QuadPart * 2654435761u ^ (uint32_t)(q.QuadPart >> 32) ^ GetTickCount();
    h ^= h >> 16;
    return (uint16_t)(h % 0xFFFE + 1);
}

static void udp_send(const uint8_t *p, size_t n);

static void make_lsf(const char *dst, const char *src, uint16_t type, uint8_t lsf[M17_LSF_BYTES])
{
    memset(lsf, 0, M17_LSF_BYTES);
    m17_encode_call(dst, lsf);
    m17_encode_call(src, lsf + 6);
    lsf[12] = (uint8_t)(type >> 8);
    lsf[13] = (uint8_t)type;
    uint16_t crc = m17_crc(lsf, 28);
    lsf[28] = (uint8_t)(crc >> 8);
    lsf[29] = (uint8_t)crc;
}

static void gw_up_send(const uint8_t payload[16], uint16_t type, bool last)
{
    m17_stream_t u;
    memset(&u, 0, sizeof(u));
    u.stream_id = G.up_sid;
    snprintf(u.src, sizeof(u.src), "%s", R.call);
    snprintf(u.dst, sizeof(u.dst), "%s", R.dest);
    u.type = type;
    u.fn = G.up_fn;
    u.last = last;
    memcpy(u.payload, payload, 16);
    uint8_t pkt[M17_STREAM_LEN];
    m17_build_stream(&u, pkt);
    udp_send(pkt, sizeof(pkt));
    G.up_fn = (uint16_t)((G.up_fn + 1) & 0x7FFF);
    G.up_packets++;
}

/* a call starts being heard: decide whether and where it is relayed */
static void gw_start(const m17_stream_t *s, bool from_rf, double t)
{
    memset(&G, 0, sizeof(G));
    if (!s_cfg.gateway || !s_radio || !s_use_net) {
        return;
    }
    G.start = t;
    G.under0 = radio_stats(s_radio)->tx_underruns;
    if (from_rf && s_cfg.gw_commands) {
        G.cmd = parse_cmd(s->dst, G.cmd_refl, &G.cmd_mod);
        if (G.cmd != GC_NONE) {
            static const char *names[] = { "", "link", "unlink", "echo", "info" };
            logf_("    command from %s: %s%s%s%s%c", s->src, names[G.cmd], G.cmd == GC_LINK ? " " : "",
                  G.cmd == GC_LINK ? G.cmd_refl : "", G.cmd == GC_LINK ? " " : "", G.cmd == GC_LINK ? G.cmd_mod : ' ');
            if (G.cmd == GC_ECHO) {
                C.echo_n = 0;
            }
            return;                           /* a command is not relayed */
        }
    }
    if (from_rf && s_cfg.gw_rf_to_net && s_st.link == ML_UP) {
        G.up = true;
        G.up_sid = new_stream_id();
        logf_("    relay: radio -> reflector");
    } else if (!from_rf && s_cfg.gw_net_to_rf) {
        uint8_t lsf[M17_LSF_BYTES];
        make_lsf(s->dst, s_cfg.gw_keep_src ? s->src : s_cfg.callsign, s->type, lsf);
        radio_tx_begin_ex(s_radio, lsf, s_cfg.gw_cushion_ms);
        G.down = true;
        logf_("    relay: reflector -> radio (transmitting as %s)", s_cfg.gw_keep_src ? s->src : s_cfg.callsign);
    }
}

/* the call ends (last packet, end of signal, time-out or cut): close what was relayed */
static void gw_stop(const char *why)
{
    if (G.cmd != GC_NONE) {
        C.cmd = G.cmd;
        snprintf(C.refl, sizeof(C.refl), "%s", G.cmd_refl);
        C.mod = G.cmd_mod;
        snprintf(C.caller, sizeof(C.caller), "%s", R.call);
        G.cmd = GC_NONE;
        return;
    }
    static const uint8_t zero[16];
    uint8_t sil[16];
    memcpy(sil, s_sil3200, 8);
    memcpy(sil + 8, s_sil3200, 8);
    if (G.up) {
        gw_up_send(sil, M17_TYPE_V3200, true);
        G.up = false;
    }
    if (G.down) {
        radio_tx_frame(s_radio, G.down_fn, true, zero);
        radio_tx_end(s_radio);
        G.down = false;
    }
    if (G.up_packets || G.down_packets) {
        unsigned under = radio_stats(s_radio)->tx_underruns - G.under0;
        logf_("    relay end%s: %u packets to the reflector, %u frames on the radio%s", why, G.up_packets, G.down_packets,
              under ? "  [the radio ran dry: packets came too late, raise relay_cushion_ms]" : "");
        EnterCriticalSection(&s_cs);
        s_st.gw_up_calls += G.up_packets > 0;
        s_st.gw_down_calls += G.down_packets > 0;
        LeaveCriticalSection(&s_cs);
        G.up_packets = G.down_packets = 0;
    }
}

/* one packet of the call being heard */
static void gw_packet(const m17_stream_t *s, double t)
{
    if (G.cmd == GC_ECHO) {
        if (C.echo_n < 1500) {                /* up to 60 s */
            reply_reserve(C.echo_n + 1);
            memcpy(C.pl[C.echo_n++], s->payload, 16);
            C.type = s->type;
        }
        return;
    }
    if (!G.up && !G.down) {
        return;
    }
    if (s_cfg.tot_s && (t - G.start) / 1000.0 > (double)s_cfg.tot_s) {
        logf_("[W] relay time-out (%u s): the rest of this call is not relayed", s_cfg.tot_s);
        gw_stop(" (time-out)");
        return;
    }
    if (G.up) {
        if (strcmp(R.call, "?") == 0) {
            return;                           /* joined late: wait until the LICH gives the callsign */
        }
        if (s_st.link != ML_UP) {
            gw_stop(" (reflector link lost)");
            return;
        }
        gw_up_send(s->payload, s->type, s->last);
        if (s->last) {
            G.up = false;
        }
    }
    if (G.down) {
        radio_tx_frame(s_radio, G.down_fn, s->last, s->payload);
        G.down_fn = (uint16_t)((G.down_fn + 1) & 0x7FFF);
        G.down_packets++;
        if (s->last) {
            radio_tx_end(s_radio);
            G.down = false;
        }
    }
}

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
    gw_stop(why);
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

static void rx_start(const m17_stream_t *s, double t, bool from_rf)
{
    R.from_rf = from_rf;
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
    logf_("RX  %s  to %s  Codec 2 %s%s", R.call, R.dest, R.type, s_cfg.gateway ? (from_rf ? "  [radio]" : "  [reflector]") : "");
    gw_start(s, from_rf, t);
}

static void rx_stream(const m17_pkt_t *k, double t, bool from_rf)
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
        rx_start(s, t, from_rf);
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
    gw_packet(s, t);

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
    if (s_radio) {
        radio_tx_frame(s_radio, T.s.fn, last, T.s.payload);
    }
    if (s_use_net && s_st.link == ML_UP) {
        m17_build_stream(&T.s, pkt);
        udp_send(pkt, sizeof(pkt));
    }
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
    T.s.stream_id = new_stream_id();      /* rand() would repeat: its state is per thread and not seeded here */
    snprintf(T.s.src, sizeof(T.s.src), "%s", s_cfg.callsign);
    snprintf(T.s.dst, sizeof(T.s.dst), "%s", s_cfg.dest[0] ? s_cfg.dest : "@ALL");
    T.s.type = M17_TYPE_V3200;
    if (s_radio) {
        uint8_t lsf[M17_LSF_BYTES];
        make_lsf(T.s.dst, T.s.src, T.s.type, lsf);
        radio_tx_begin(s_radio, lsf);
    }
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
    if (s_radio) {
        radio_tx_end(s_radio);
    }
    T.active = false;
    wav_close(&T.mic);
    logf_("    end of transmission: %.1f s, %u packets", (t - T.start) / 1000.0, T.packets);
    EnterCriticalSection(&s_cs);
    s_st.tx = false;
    LeaveCriticalSection(&s_cs);
}

/* ---------------------------------------------------------------- radio mode: decoded RF frames -> the RX path */
static uint16_t s_rad_sid = 1;
static struct {
    unsigned frames;
    double evm_sum;
    float evm_max;
} RS;

static struct {
    unsigned frames, bad;                 /* relay + loopback: our own transmission heard back */
    char src[M17_CALL_LEN];
} SELF;

static void radio_events(double t)
{
    radio_evt_t ev[16];
    int n = radio_poll(s_radio, ev, 16);
    for (int i = 0; i < n; ++i) {
        const radio_evt_t *e = &ev[i];
        /* our own transmission: in relay mode never relayed again (the loopback only counts it as a check) */
        bool ours = s_use_net && (T.active || G.down || radio_keyed(s_radio));
        if (ours) {
            if (radio_full_duplex(s_radio)) {
                if (e->kind == RE_LSF && e->have_lsf) {
                    m17_decode_call(e->lsf + 6, SELF.src);
                } else if (e->kind == RE_STREAM) {
                    SELF.frames++;
                    SELF.bad += e->s.lich_cnt < 0;
                } else if (e->kind == RE_END && SELF.frames) {
                    logf_("    loopback check of our transmission: %u frames decoded, %u bad LICH, source %s",
                          SELF.frames, SELF.bad, SELF.src[0] ? SELF.src : "?");
                    memset(&SELF, 0, sizeof(SELF));
                }
            }
            continue;
        }
        if (T.active && !radio_full_duplex(s_radio)) {
            continue;
        }
        if (e->kind == RE_END) {
            if (R.active) {
                rx_finish(e->signal_lost ? " (signal lost)" : "");
            }
            if (RS.frames) {
                logf_("    RF: %u frames, EVM avg %.3f max %.3f", RS.frames, RS.evm_sum / RS.frames, RS.evm_max);
            }
            memset(&RS, 0, sizeof(RS));
            s_rad_sid++;
            continue;
        }
        char src[M17_CALL_LEN] = "?", dst[M17_CALL_LEN] = "?";
        uint16_t type = M17_TYPE_V3200;
        if (e->have_lsf) {
            m17_decode_call(e->lsf, dst);
            m17_decode_call(e->lsf + 6, src);
            type = (uint16_t)(e->lsf[12] << 8 | e->lsf[13]);
        }
        if (e->kind == RE_LSF) {
            if (e->lsf_from_lich && R.active) {            /* joined late: now we know who it is */
                snprintf(R.call, sizeof(R.call), "%s", src);
                snprintf(R.dest, sizeof(R.dest), "%s", dst);
                EnterCriticalSection(&s_cs);
                snprintf(s_st.rx_call, sizeof(s_st.rx_call), "%s", src);
                snprintf(s_st.rx_dest, sizeof(s_st.rx_dest), "%s", dst);
                LeaveCriticalSection(&s_cs);
                logf_("    callsign from the LICH: %s to %s", src, dst);
                if (R.from_rf && s_cfg.gateway && s_cfg.gw_commands && G.cmd == GC_NONE) {
                    G.cmd = parse_cmd(dst, G.cmd_refl, &G.cmd_mod);   /* a command heard late is still a command */
                    if (G.cmd != GC_NONE) {
                        G.up = false;                 /* nothing was relayed yet: the callsign was unknown */
                        C.echo_n = 0;
                        logf_("    command from %s (joined late): %s", src, dst);
                    }
                }
            } else {
                if (R.active) {
                    rx_finish(" (new call)");
                }
                s_rad_sid++;
                memset(&RS, 0, sizeof(RS));
                logf_("RF  LSF %s to %s, level %.0f%s", src, dst, fabsf(e->gain),
                      e->gain < 0 ? "  inverted (the other station may tick its invert option)" : "");
            }
            continue;
        }
        m17_pkt_t k;
        memset(&k, 0, sizeof(k));
        k.kind = MP_STREAM;
        k.crc_ok = e->s.lich_cnt >= 0;
        k.s.stream_id = s_rad_sid;
        snprintf(k.s.src, sizeof(k.s.src), "%s", src);
        snprintf(k.s.dst, sizeof(k.s.dst), "%s", dst);
        k.s.type = type;
        k.s.fn = e->s.fn;
        k.s.last = e->s.last;
        memcpy(k.s.payload, e->s.payload, 16);
        RS.frames++;
        RS.evm_sum += e->evm;
        RS.evm_max = e->evm > RS.evm_max ? e->evm : RS.evm_max;
        rx_stream(&k, t, true);
        if (e->s.last) {
            s_rad_sid++;
        }
    }
}

/* carries out a command heard on the radio, follows a commanded link, sends the answer when the channel is free */
static void gw_service(double t, me_link_t *link, double *t_conn, unsigned *tries)
{
    uint8_t pkt[16];
    if (C.cmd == GC_LINK) {
        char host[128];
        uint16_t port = 0;
        if (!hosts_lookup(C.refl, host, sizeof(host), &port)) {
            logf_("[W] %s is not in the reflector list (%s)", C.refl, s_cfg.hosts_file);
            reply_beeps("NOT FOUND", 3, 440.0f, t + 800);
        } else if (*link == ML_UP && !_stricmp(C.name, C.refl) && s_cfg.module == C.mod) {
            char d[M17_CALL_LEN];
            snprintf(d, sizeof(d), "%.7s %c", C.refl, C.mod);
            logf_("    already linked to %s", d);
            reply_beeps(d, 2, 880.0f, t + 800);
        } else {
            if (*link != ML_OFF && s_cfg.host[0]) {
                udp_send(pkt, m17_disc(s_cfg.callsign, pkt));
            }
            if (resolve_to(host, port)) {
                snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
                s_cfg.port = port;
                s_cfg.module = C.mod;
                snprintf(C.name, sizeof(C.name), "%s", C.refl);
                *link = ML_CONNECTING;
                *t_conn = -1e9;
                *tries = 0;
                C.await = true;
                C.await_t = t;
                EnterCriticalSection(&s_cs);
                s_st.reflector[0] = 0;
                LeaveCriticalSection(&s_cs);
                set_link(*link);
                logf_("linking to %s module %c (%s:%u), asked by %s", C.refl, C.mod, host, port, C.caller);
            } else {
                *link = ML_OFF;
                set_link(*link);
                reply_beeps("LINK FAIL", 3, 440.0f, t + 800);
            }
        }
    } else if (C.cmd == GC_UNLINK) {
        if (*link != ML_OFF && s_cfg.host[0]) {
            udp_send(pkt, m17_disc(s_cfg.callsign, pkt));
        }
        *link = ML_OFF;
        set_link(*link);
        C.await = false;
        C.name[0] = 0;
        EnterCriticalSection(&s_cs);
        s_st.reflector[0] = 0;
        LeaveCriticalSection(&s_cs);
        logf_("unlinked, asked by %s", C.caller);
        reply_beeps("UNLINKED", 1, 660.0f, t + 800);
    } else if (C.cmd == GC_INFO) {
        if (*link == ML_UP) {
            char d[M17_CALL_LEN];
            snprintf(d, sizeof(d), "%.7s %c", C.name[0] ? C.name : (s_st.reflector[0] ? s_st.reflector : "M17-???"),
                     s_cfg.module);
            reply_beeps(d, 2, 880.0f, t + 800);
        } else {
            reply_beeps("UNLINKED", 1, 660.0f, t + 800);
        }
    } else if (C.cmd == GC_ECHO) {
        if (C.echo_n) {
            snprintf(C.reply_dst, sizeof(C.reply_dst), "ECHO");
            C.n = C.echo_n;
            C.reply = true;
            C.reply_at = t + 1000;
            logf_("    echo: %.1f s recorded, playing it back in 1 s", C.echo_n * 0.04);
        }
    }
    C.cmd = GC_NONE;

    if (C.await) {
        if (*link == ML_UP) {
            char d[M17_CALL_LEN];
            snprintf(d, sizeof(d), "%.7s %c", C.name, s_cfg.module);
            reply_beeps(d, 2, 880.0f, t + 300);
            C.await = false;
        } else if (*link == ML_REFUSED || t - C.await_t > 15000) {
            logf_("[W] could not link to %s module %c (%s)", C.name, s_cfg.module,
                  *link == ML_REFUSED ? "refused" : "no answer");
            if (s_cfg.host[0]) {
                udp_send(pkt, m17_disc(s_cfg.callsign, pkt));
            }
            *link = ML_OFF;
            set_link(*link);
            C.await = false;
            C.name[0] = 0;
            reply_beeps("LINK FAIL", 3, 440.0f, t + 300);
        }
    }

    if (C.reply && t >= C.reply_at && !R.active && !T.active && !radio_keyed(s_radio)) {
        if (s_cfg.gw_replies) {
            reply_send_now();
        } else {
            logf_("    (answer \"%s\" not transmitted: answers are off)", C.reply_dst);
            C.reply = false;
        }
    }
}

/* ---------------------------------------------------------------- loopback tests: another station on the radio */
static void test_rf_call(int k)
{
    int16_t *keep_pcm = s_wav_pcm;        /* load_wav fills the --tx-wav buffer: keep that one */
    size_t keep_n = s_wav_n;
    s_wav_pcm = NULL;
    s_wav_n = 0;
    bool ok = load_wav(s_cfg.test_rf[k].wav);
    int16_t *pcm = s_wav_pcm;
    size_t n = s_wav_n;
    s_wav_pcm = keep_pcm;
    s_wav_n = keep_n;
    if (!ok) {
        logf_("[E] cannot read %s", s_cfg.test_rf[k].wav);
        return;
    }
    struct CODEC2 *c2 = codec2_create(CODEC2_MODE_3200);
    uint8_t lsf[M17_LSF_BYTES];
    const char *call = s_cfg.test_rf[k].call[0] ? s_cfg.test_rf[k].call : "JA0RF";
    const char *dst = s_cfg.test_rf[k].dst[0] ? s_cfg.test_rf[k].dst : "@ALL";
    make_lsf(dst, call, M17_TYPE_V3200, lsf);
    int8_t sym[M17_SYMS_PER_FRAME];
    m17_rf_preamble(sym);
    radio_loop_inject(s_radio, sym, M17_SYMS_PER_FRAME);
    m17_rf_lsf_frame(lsf, sym);
    radio_loop_inject(s_radio, sym, M17_SYMS_PER_FRAME);
    size_t frames = n / 320;
    for (size_t f = 0; f < frames; ++f) {
        uint8_t pl[16];
        codec2_encode(c2, pl, pcm + 320 * f);
        codec2_encode(c2, pl + 8, pcm + 320 * f + 160);
        m17_rf_stream_frame(lsf, (uint16_t)f, f + 1 == frames, pl, sym);
        radio_loop_inject(s_radio, sym, M17_SYMS_PER_FRAME);
    }
    m17_rf_eot(sym);
    radio_loop_inject(s_radio, sym, M17_SYMS_PER_FRAME);
    codec2_destroy(c2);
    free(pcm);
    logf_("test: %s calls \"%s\" on the radio channel (%u frames)", call, dst, (unsigned)frames);
}

/* ---------------------------------------------------------------- the thread */
static DWORD WINAPI run(LPVOID arg)
{
    (void)arg;
    timeBeginPeriod(1);
    memset(&R, 0, sizeof(R));
    memset(&T, 0, sizeof(T));
    s_use_net = !s_cfg.radio || s_cfg.gateway;
    if (s_cfg.radio) {
        s_radio = radio_open(&s_cfg.rc, logf_);
    }
    if ((s_cfg.radio && !s_radio) || (s_use_net && !open_socket())) {
        radio_close(s_radio);
        s_radio = NULL;
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
    double t_conn = -1e9, t_rx = now_ms(), t_begin = now_ms();
    int test_rf_next = s_radio && radio_full_duplex(s_radio) ? 0 : s_cfg.test_rf_n;
    me_link_t link = s_use_net ? (s_cfg.host[0] ? ML_CONNECTING : ML_OFF) : ML_UP;
    set_link(link);
    EnterCriticalSection(&s_cs);
    s_st.radio_up = s_radio != NULL;
    if (s_radio && !s_use_net) {
        snprintf(s_st.reflector, sizeof(s_st.reflector), "RADIO");
    }
    LeaveCriticalSection(&s_cs);
    if (s_radio && !s_use_net) {
        logf_("radio mode: callsign %s, destination %s", s_cfg.callsign, s_cfg.dest[0] ? s_cfg.dest : "@ALL");
    } else if (s_radio) {
        logf_("relay mode: radio -> reflector %s, reflector -> radio %s%s", s_cfg.gw_rf_to_net ? "on" : "off",
              s_cfg.gw_net_to_rf ? "on" : "off",
              s_cfg.gw_net_to_rf ? (s_cfg.gw_keep_src ? " (caller's callsign)" : " (our callsign)") : "");
    }
    unsigned tries = 0;

    while (!s_stop) {
        double t = now_ms();
        if (s_radio) {
            if (test_rf_next < s_cfg.test_rf_n && t - t_begin > s_cfg.test_rf[test_rf_next].at * 1000.0) {
                test_rf_call(test_rf_next++);
            }
            radio_events(t);
            if (s_use_net && s_cfg.gateway) {
                gw_service(t, &link, &t_conn, &tries);
            }
        }
        if (s_use_net && ((link == ML_CONNECTING && t - t_conn > CONN_RETRY_MS) || (link == ML_REFUSED && t - t_conn > REFUSED_RETRY_MS))) {
            udp_send(pkt, m17_conn(s_cfg.callsign, s_cfg.module, pkt));
            logf_("connecting ...");
            if (++tries == 3) {
                logf_("[W] no answer from the reflector for 15 s: it may be down or blocking us "
                      "(check the host and port, or pick another reflector)");
            }
            t_conn = t;
        }
        if (s_use_net && link == ML_UP && t - t_rx > LINK_TIMEOUT_MS) {
            logf_("[W] nothing from the reflector for 30 s: connecting again");
            link = ML_CONNECTING;
            set_link(link);
            tries = 0;
            t_conn = -1e9;
        }
        for (; s_use_net;) {
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
                    rx_stream(&k, t, false);
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
            if ((!mic && !s_wav_pcm) || (link != ML_UP && !s_radio)) {
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
    if (s_radio) {
        for (int i = 0; i < 3000 && radio_keyed(s_radio); ++i) {   /* let the end of the transmission play */
            radio_events(now_ms());
            Sleep(1);
        }
        radio_close(s_radio);
        s_radio = NULL;
        logf_("radio closed");
    }
    if (s_use_net) {
        if (s_cfg.host[0] && link != ML_OFF) {
            udp_send(pkt, m17_disc(s_cfg.callsign, pkt));
        }
        logf_("unlinked");
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
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
        s_c3200_beep = codec2_create(CODEC2_MODE_3200);
        if (ok) {
            struct CODEC2 *c = codec2_create(CODEC2_MODE_3200);
            short z[160] = { 0 };
            for (int i = 0; i < 4; ++i) {
                codec2_encode(c, s_sil3200, z);
            }
            codec2_destroy(c);
        }
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
