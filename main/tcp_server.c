#include "tcp_server.h"

#include <errno.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <unistd.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "dv3000_log.h"
#include "dv3000_protocol.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define RX_CHUNK        512
#define SEND_TIMEOUT_S  2

static int s_client = -1;
static tcp_stats_t s_stats;

static void delay_ms(unsigned ms)
{
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    usleep(ms * 1000u);
#endif
}

static void close_client(const char *why)
{
    if (s_client >= 0) {
        DV_LOGI("client disconnected (%s)", why);
        close(s_client);
        s_client = -1;
        s_stats.connected = false;
    }
}

/* UDP (AMBEserver style, e.g. BlueDV "AMBE server"): one DV3000 packet per datagram, the reply goes back
 * to the sender of the request being processed. */
static int s_udp = -1;
static bool s_reply_udp;                 /* the packet being processed came in over UDP */
static struct sockaddr_in s_udp_peer;

static bool udp_tx(const uint8_t *data, size_t len)
{
    if (sendto(s_udp, data, len, 0, (struct sockaddr *)&s_udp_peer, sizeof(s_udp_peer)) != (ssize_t)len) {
        s_stats.tx_errors++;
        return false;
    }
    s_stats.tx_bytes += (uint32_t)len;
    return true;
}

/* Protocol layer -> network. Never blocks longer than SEND_TIMEOUT_S: a stalled client is dropped
 * so the server can accept a new one. */
static bool tcp_tx(const uint8_t *data, size_t len)
{
    if (s_reply_udp) {
        return udp_tx(data, len);
    }
    if (s_client < 0) {
        return false;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(s_client, data + off, len - off, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOMEM || errno == ENOBUFS)) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s_client, &wf);
            struct timeval tv = { SEND_TIMEOUT_S, 0 };
            if (select(s_client + 1, NULL, &wf, NULL, &tv) > 0) {
                continue;
            }
        }
        s_stats.tx_errors++;
        close_client("send failed");
        return false;
    }
    s_stats.tx_bytes += (uint32_t)len;
    return true;
}

void tcp_server_get_stats(tcp_stats_t *out)
{
    *out = s_stats;
}

static void setup_client_socket(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));     /* 20 ms voice frames: no Nagle delay */
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));     /* notice a vanished client */
#ifdef TCP_KEEPIDLE
    int idle = 10, intvl = 5, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

void tcp_server_run(uint16_t port)
{
    static uint8_t buf[RX_CHUNK];

    dv3000_protocol_init(tcp_tx);

    for (;;) {
        int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls < 0) {
            DV_LOGE("socket() failed, errno %d", errno);
            delay_ms(1000);
            continue;
        }
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 2) < 0) {
            DV_LOGE("bind/listen(TCP %u) failed, errno %d", (unsigned)port, errno);
            close(ls);
            delay_ms(1000);
            continue;
        }
        s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_udp >= 0 && bind(s_udp, (struct sockaddr *)&a, sizeof(a)) < 0) {
            DV_LOGE("bind(UDP %u) failed, errno %d", (unsigned)port, errno);
            close(s_udp);
            s_udp = -1;
        }
        DV_LOGI("DV3000 server listening on TCP%s port %u", s_udp >= 0 ? " + UDP" : "", (unsigned)port);

        for (;;) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(ls, &rf);
            int maxfd = ls;
            if (s_udp >= 0) {
                FD_SET(s_udp, &rf);
                if (s_udp > maxfd) {
                    maxfd = s_udp;
                }
            }
            if (s_client >= 0) {
                FD_SET(s_client, &rf);
                if (s_client > maxfd) {
                    maxfd = s_client;
                }
            }
            struct timeval tv = { 1, 0 };
            int r = select(maxfd + 1, &rf, NULL, NULL, &tv);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                DV_LOGE("select failed, errno %d", errno);
                break;
            }
            if (r == 0) {
                continue;
            }

            if (FD_ISSET(ls, &rf)) {
                struct sockaddr_in pa;
                socklen_t pl = sizeof(pa);
                int nfd = accept(ls, (struct sockaddr *)&pa, &pl);
                if (nfd >= 0) {
                    if (s_client >= 0) {
                        close_client("replaced by a new connection");
                    }
                    setup_client_socket(nfd);
                    s_client = nfd;
                    s_stats.connected = true;
                    s_stats.accepted++;
                    s_stats.client_ip = ntohl(pa.sin_addr.s_addr);
                    s_stats.client_port = ntohs(pa.sin_port);
                    dv3000_protocol_reset_stream();          /* no half packet from the previous client */
                    DV_LOGI("client connected: %u.%u.%u.%u:%u", (unsigned)(s_stats.client_ip >> 24),
                            (unsigned)((s_stats.client_ip >> 16) & 255), (unsigned)((s_stats.client_ip >> 8) & 255),
                            (unsigned)(s_stats.client_ip & 255), (unsigned)s_stats.client_port);
                }
            }

            if (s_client >= 0 && FD_ISSET(s_client, &rf)) {
                ssize_t n = recv(s_client, buf, sizeof(buf), 0);
                if (n > 0) {
                    s_stats.rx_bytes += (uint32_t)n;
                    dv3000_protocol_feed(buf, (size_t)n);
                } else if (n == 0) {
                    close_client("closed by peer");
                } else if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
                    close_client("recv error");
                }
            }

            if (s_udp >= 0 && FD_ISSET(s_udp, &rf)) {
                struct sockaddr_in pa;
                socklen_t pl = sizeof(pa);
                ssize_t n = recvfrom(s_udp, buf, sizeof(buf), 0, (struct sockaddr *)&pa, &pl);
                if (n > 0) {
                    uint32_t ip = ntohl(pa.sin_addr.s_addr);
                    if (ip != s_stats.client_ip || ntohs(pa.sin_port) != s_stats.client_port) {
                        DV_LOGI("UDP client: %u.%u.%u.%u:%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255),
                                (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255), (unsigned)ntohs(pa.sin_port));
                    }
                    s_stats.client_ip = ip;
                    s_stats.client_port = ntohs(pa.sin_port);
                    s_stats.rx_bytes += (uint32_t)n;
                    s_udp_peer = pa;
                    s_reply_udp = true;
                    dv3000_protocol_reset_stream();          /* a datagram is a whole packet: start clean */
                    dv3000_protocol_feed(buf, (size_t)n);
                    s_reply_udp = false;
                }
            }
        }
        close_client("server restart");
        if (s_udp >= 0) {
            close(s_udp);
            s_udp = -1;
        }
        close(ls);
        delay_ms(200);
    }
}

#ifdef ESP_PLATFORM
static void server_task(void *arg)
{
    tcp_server_run((uint16_t)(uintptr_t)arg);
}

void tcp_server_start(uint16_t port)
{
    /* The codec runs in this task: same 16 KB stack as the serial protocol task. Pinned to the core the Wi-Fi
     * stack does not use (Wi-Fi/lwIP run on core 0) with a higher priority, so a ~10 ms encode is not
     * preempted by network work; otherwise frames of a burst queued behind it and the audio broke up. */
    BaseType_t core = (portNUM_PROCESSORS > 1) ? 1 : 0;
    if (xTaskCreatePinnedToCore(server_task, "dv3000_tcp", 16384, (void *)(uintptr_t)port, 10, NULL, core) != pdPASS) {
        DV_LOGE("could not create the TCP server task");
    }
}
#endif
