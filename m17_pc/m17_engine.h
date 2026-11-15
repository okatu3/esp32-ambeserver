#pragma once
/* The M17 station: one thread links to one module of an M17 reflector (CONN, answers PING with PONG), plays received
 * voice streams (Codec 2 3200, and 1600 from voice+data streams; adaptive jitter buffer of audio_win.c) and, while
 * PTT is on, encodes the microphone with Codec 2 3200 and sends one 54-byte stream packet per 40 ms.
 * Half duplex: received streams are ignored while transmitting. Used by the window (m17_gui.c) and the command line
 * (m17_cli.c). */
#include <stdbool.h>
#include <stdint.h>

#include "m17.h"
#include "radio_win.h"

#define ME_HEARD_N 20

typedef struct {
    char host[128];
    uint16_t port;                 /* 0 = 17000 */
    char module;                   /* 'A'..'Z' */
    char callsign[M17_CALL_LEN];   /* up to 9 characters */
    char dest[M17_CALL_LEN];       /* destination of our streams, "" = "@ALL" */
    int mic_device, spk_device;    /* -1 = Windows' default */
    unsigned jitter_ms;            /* playout target, e.g. 300 */
    float tx_gain_db, rx_gain_db;
    unsigned tot_s;                /* transmit time-out, e.g. 180 */
    char wav_dir[260];             /* "" = do not save calls */
    char tx_wav[260];              /* tests: transmit this WAV (8 kHz mono 16 bit) instead of the microphone */
    bool no_speaker;               /* tests */
    bool radio;                    /* talk to a radio through the sound card (radio_win.c) instead of a reflector */
    radio_cfg_t rc;
    /* relay (gateway): radio and reflector at the same time, one call at a time; our own PTT goes to both */
    bool gateway;                  /* needs radio = true and a reflector host */
    bool gw_rf_to_net;             /* calls heard on the radio -> reflector (default on) */
    bool gw_net_to_rf;             /* calls from the reflector -> transmitted by our radio (default off) */
    bool gw_keep_src;              /* reflector -> radio: keep the caller's callsign as source (default: ours) */
    unsigned gw_cushion_ms;        /* reflector -> radio: extra silence before the call (default 200) */
    /* loopback tests: another station calls on the radio channel test_rf_at seconds after the start */
    char test_rf_wav[260];         /* 8 kHz mono 16 bit */
    char test_rf_call[M17_CALL_LEN];
    float test_rf_at;
} me_cfg_t;

typedef enum { ML_OFF, ML_CONNECTING, ML_UP, ML_REFUSED } me_link_t;

typedef struct {
    char time[9], call[M17_CALL_LEN], dest[M17_CALL_LEN];
    float seconds;
    char type[6];                  /* "3200", "1600", "DATA" */
} me_heard_t;

typedef struct {
    me_link_t link;
    bool radio_up;                 /* radio or relay mode: the sound card is open */
    uint32_t gw_up_calls, gw_down_calls;   /* calls relayed radio -> reflector / reflector -> radio */
    char reflector[M17_CALL_LEN];  /* callsign the reflector gives in PING, e.g. "M17-XXX" */
    bool tx, rx;
    float tx_seconds;
    char rx_call[M17_CALL_LEN], rx_dest[M17_CALL_LEN], rx_type[6];
    float mic_dbfs, rx_dbfs;       /* level of the last 20 ms, -99 = silence */
    unsigned rx_lost_pct;          /* packets missing from the stream so far, % */
    me_heard_t heard[ME_HEARD_N];  /* newest first */
    int heard_n;
    uint32_t tx_packets_total, rx_calls_total;
} me_status_t;

typedef void (*me_log_fn)(const char *line);

void me_default_cfg(me_cfg_t *c);
bool me_start(const me_cfg_t *c, me_log_fn log);   /* false if audio/codec/socket failed */
void me_stop(void);                                 /* ends a transmission, unlinks, waits */
bool me_running(void);
void me_set_ptt(bool on);
void me_get_status(me_status_t *s);
