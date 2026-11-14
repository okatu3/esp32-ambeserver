#include "m17_parrot.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m17.h"

#define MAX_CLIENTS   16
#define MAX_STREAMS   8
#define MAX_REPLAYS   8
#define PING_MS       3000
#define STREAM_END_MS 1000          /* a stream without its last packet is over after this */
#define REPLAY_DELAY  1000
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

typedef struct {
    bool used;
    struct sockaddr_in addr;
    char call[M17_CALL_LEN];
    char module;
} client_t;

typedef struct {
    bool used;
    struct sockaddr_in addr;
    uint16_t sid;
    uint8_t *pk;
    int n, cap;
    DWORD last;
} stream_t;

typedef struct {
    bool used;
    struct sockaddr_in addr;
    uint8_t *pk;
    int n, idx;
    uint16_t sid;
    DWORD t0;
} replay_t;

static HANDLE s_thread;
static volatile LONG s_stop, s_running;
static SOCKET s_sock = INVALID_SOCKET;
static parrot_log_fn s_log;
static client_t C[MAX_CLIENTS];
static stream_t St[MAX_STREAMS];
static replay_t Rp[MAX_REPLAYS];

static void plog(const char *fmt, ...)
{
    char line[300];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(line, sizeof(line), "%02u:%02u:%02u  [parrot] ", st.wHour % 24u, st.wMinute % 60u, st.wSecond % 60u);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (s_log) {
        s_log(line);
    }
}

static bool same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void send_to(const struct sockaddr_in *a, const uint8_t *p, int n)
{
    sendto(s_sock, (const char *)p, n, 0, (const struct sockaddr *)a, sizeof(*a));
}

static client_t *find_client(const struct sockaddr_in *a)
{
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (C[i].used && same(&C[i].addr, a)) {
            return &C[i];
        }
    }
    return NULL;
}

static void stream_done(stream_t *s, const char *why)
{
    client_t *c = find_client(&s->addr);
    plog("stream %04X from %s ended%s: %d packets (%.1f s), back in 1 s", s->sid, c ? c->call : "?", why, s->n,
         s->n * 0.04);
    replay_t *r = NULL;
    for (int i = 0; i < MAX_REPLAYS && !r; ++i) {
        if (!Rp[i].used) {
            r = &Rp[i];
        }
    }
    if (r && s->n > 0) {
        r->used = true;
        r->addr = s->addr;
        r->pk = s->pk;                       /* handed over */
        r->n = s->n;
        r->idx = 0;
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        r->sid = (uint16_t)(((uint32_t)q.QuadPart * 2654435761u >> 16) % 0xFFFE + 1);
        r->t0 = GetTickCount() + REPLAY_DELAY;
    } else {
        free(s->pk);
    }
    memset(s, 0, sizeof(*s));
}

static void on_stream(const uint8_t *p, const struct sockaddr_in *from, client_t *c)
{
    for (int i = 0; i < MAX_CLIENTS; ++i) {              /* relay to the others on the module */
        if (C[i].used && &C[i] != c && C[i].module == c->module) {
            send_to(&C[i].addr, p, M17_STREAM_LEN);
        }
    }
    uint16_t sid = (uint16_t)(p[4] << 8 | p[5]);
    stream_t *s = NULL;
    for (int i = 0; i < MAX_STREAMS && !s; ++i) {
        if (St[i].used && St[i].sid == sid && same(&St[i].addr, from)) {
            s = &St[i];
        }
    }
    if (!s) {
        for (int i = 0; i < MAX_STREAMS && !s; ++i) {
            if (!St[i].used) {
                s = &St[i];
            }
        }
        if (!s) {
            return;
        }
        m17_pkt_t k = m17_parse(p, M17_STREAM_LEN);
        s->used = true;
        s->addr = *from;
        s->sid = sid;
        plog("stream %04X from %s to %s, Codec 2 %s%s", sid, k.s.src, k.s.dst, m17_type_name(k.s.type),
             k.crc_ok ? "" : ", CRC error");
    }
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 256;
        uint8_t *np = realloc(s->pk, (size_t)cap * M17_STREAM_LEN);
        if (!np) {
            return;
        }
        s->pk = np;
        s->cap = cap;
    }
    if (s->n < 25 * 300) {                                /* keep up to 5 minutes */
        memcpy(s->pk + (size_t)s->n * M17_STREAM_LEN, p, M17_STREAM_LEN);
        s->n++;
    }
    s->last = GetTickCount();
    if (p[34] & 0x80) {
        stream_done(s, "");
    }
}

static DWORD WINAPI run(LPVOID arg)
{
    (void)arg;
    timeBeginPeriod(1);
    uint8_t ping[10] = "PING";
    m17_encode_call("PARROT", ping + 4);
    DWORD t_ping = GetTickCount();
    uint8_t buf[512];
    while (!s_stop) {
        for (;;) {
            struct sockaddr_in from;
            int fl = sizeof(from);
            int n = recvfrom(s_sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) {
                break;
            }
            client_t *c = find_client(&from);
            if (n == 11 && !memcmp(buf, "CONN", 4)) {
                if (!c) {
                    for (int i = 0; i < MAX_CLIENTS && !c; ++i) {
                        if (!C[i].used) {
                            c = &C[i];
                        }
                    }
                }
                if (!c) {
                    send_to(&from, (const uint8_t *)"NACK", 4);
                    continue;
                }
                c->used = true;
                c->addr = from;
                m17_decode_call(buf + 4, c->call);
                c->module = (char)buf[10];
                send_to(&from, (const uint8_t *)"ACKN", 4);
                plog("%s linked to module %c", c->call, c->module);
            } else if (n >= 4 && !memcmp(buf, "DISC", 4)) {
                if (c) {
                    plog("%s unlinked", c->call);
                    memset(c, 0, sizeof(*c));
                }
                send_to(&from, (const uint8_t *)"DISC", 4);
            } else if (n == M17_STREAM_LEN && !memcmp(buf, "M17 ", 4) && c) {
                on_stream(buf, &from, c);
            }
        }
        DWORD now = GetTickCount();
        if (now - t_ping >= PING_MS) {
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (C[i].used) {
                    send_to(&C[i].addr, ping, 10);
                }
            }
            t_ping = now;
        }
        for (int i = 0; i < MAX_STREAMS; ++i) {
            if (St[i].used && now - St[i].last > STREAM_END_MS) {
                stream_done(&St[i], " (no last packet)");
            }
        }
        for (int i = 0; i < MAX_REPLAYS; ++i) {
            replay_t *r = &Rp[i];
            if (!r->used) {
                continue;
            }
            while (r->idx < r->n && (LONG)(now - (r->t0 + (DWORD)r->idx * 40u)) >= 0) {
                uint8_t *p = r->pk + (size_t)r->idx * M17_STREAM_LEN;
                p[4] = (uint8_t)(r->sid >> 8);
                p[5] = (uint8_t)r->sid;
                if (r->idx == r->n - 1) {
                    p[34] |= 0x80;                         /* make sure the replay ends cleanly */
                }
                uint16_t crc = m17_crc(p, 52);
                p[52] = (uint8_t)(crc >> 8);
                p[53] = (uint8_t)crc;
                send_to(&r->addr, p, M17_STREAM_LEN);
                r->idx++;
            }
            if (r->idx >= r->n) {
                free(r->pk);
                memset(r, 0, sizeof(*r));
            }
        }
        Sleep(1);
    }
    for (int i = 0; i < MAX_STREAMS; ++i) {
        free(St[i].pk);
    }
    for (int i = 0; i < MAX_REPLAYS; ++i) {
        free(Rp[i].pk);
    }
    memset(St, 0, sizeof(St));
    memset(Rp, 0, sizeof(Rp));
    memset(C, 0, sizeof(C));
    closesocket(s_sock);
    s_sock = INVALID_SOCKET;
    timeEndPeriod(1);
    plog("stopped");
    InterlockedExchange(&s_running, 0);
    return 0;
}

bool parrot_start(uint16_t port, parrot_log_fn log)
{
    if (s_running) {
        return true;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    s_log = log;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) {
        return false;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port ? port : PARROT_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s_sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
        plog("[E] port %u is in use: cannot start", port ? port : PARROT_PORT);
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        return false;
    }
    u_long nb = 1;
    ioctlsocket(s_sock, FIONBIO, &nb);
    BOOL no_reset = FALSE;                               /* a client that went away must not break recvfrom */
    DWORD got;
    WSAIoctl(s_sock, SIO_UDP_CONNRESET, &no_reset, sizeof(no_reset), NULL, 0, &got, NULL, NULL);
    memset(C, 0, sizeof(C));
    memset(St, 0, sizeof(St));
    memset(Rp, 0, sizeof(Rp));
    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_running, 1);
    s_thread = CreateThread(NULL, 0, run, NULL, 0, NULL);
    if (!s_thread) {
        InterlockedExchange(&s_running, 0);
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        return false;
    }
    plog("test reflector on 127.0.0.1:%u (your transmissions come back 1 s after you stop)", port ? port : PARROT_PORT);
    return true;
}

void parrot_stop(void)
{
    if (!s_thread) {
        return;
    }
    InterlockedExchange(&s_stop, 1);
    WaitForSingleObject(s_thread, 3000);
    CloseHandle(s_thread);
    s_thread = NULL;
}

bool parrot_running(void)
{
    return s_running != 0;
}
