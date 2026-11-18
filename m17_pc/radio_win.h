#pragma once
/* An M17 RF modem in software: the PC's sound card (48 kHz mono) is wired to an FM radio's 9600 bps data port, or to a
 * radio's USB audio. Transmit: LSF / stream frames -> 4FSK baseband (m17_modem/core) -> sound output, with the PTT
 * keyed on a serial port (RTS, DTR or an Icom CI-V command). Receive: sound input -> demodulator -> decoded frames.
 * Half duplex: while keyed, the input is not demodulated. For tests the output can be looped back to the input
 * internally (no sound card), optionally with noise and the band limits of a radio's microphone / speaker path. */
#include <stdbool.h>
#include <stdint.h>

#include "fm_eq.h"
#include "fm_path.h"
#include "m17_rf.h"

#define RADIO_DEV_DEFAULT  (-1)
#define RADIO_DEV_LOOP     (-2)            /* output -> input inside the program (tests) */

typedef enum { RP_NONE, RP_RTS, RP_DTR, RP_CIV } radio_ptt_t;   /* RP_NONE: the radio keys itself (VOX) */

typedef struct {
    int in_device, out_device;             /* waveIn / waveOut index, RADIO_DEV_DEFAULT, or RADIO_DEV_LOOP (both) */
    radio_ptt_t ptt;
    char ptt_port[16];                     /* "COM5" */
    unsigned civ_addr;                     /* Icom CI-V address, e.g. 0xA4 for the IC-705 */
    unsigned civ_baud;                     /* e.g. 19200 (CI-V jack) or 115200 (USB) */
    unsigned tx_delay_ms;                  /* keyed, silent, before the preamble (radio switching to TX) */
    unsigned tx_tail_ms;                   /* keyed after the end-of-transmission word has been played */
    float tx_dbfs;                         /* level of the +3 symbols, dBFS (e.g. -10) */
    bool tx_invert;                        /* some radios invert the modulation */
    float loop_snr_db;                     /* RADIO_DEV_LOOP: noise, 0 = none */
    bool loop_voice_band;                  /* RADIO_DEV_LOOP: 300 Hz high-pass and 3 kHz low-pass on both sides */
    /* a radio without a data port, e.g. the IC-705 over USB (fm_path.h) */
    bool rx_if;                            /* the input is the radio's 12 kHz IF: an FM discriminator here */
    bool tx_pre;                           /* precompensate the radio's TX audio path (inverse high-pass, de-emphasis) */
    fmp_radio_t pre_radio;                 /* that path: high-pass, order, pre-emphasis, low-pass */
    float pre_floor_hz;                    /* lowest frequency restored (e.g. 50) */
    float pre_gain_db;                     /* after the precompensation (e.g. -24): room for the boosted lows */
    bool loop_radio;                       /* RADIO_DEV_LOOP: through a model of such a radio (pre_radio, limiter) */
    float loop_limit;                      /* that model's limiter, x the +3 symbol deviation (1.5) */
    /* equalizers learned with taikou-test1-1 on two radios (fm_eq.h). The IC-705's FM-D path delays the lows by
     * 5-12 ms; the magnitude-only precompensation leaves the 4-level eye closed. 2026-11-18, two IC-705s: with
     * tx_eq (eq_A_tx.txt) a receiver without any equalizer decoded 100 of 100 frames without errors. */
    char tx_eq[260];                       /* transmit: after the precompensation (empty: none) */
    float tx_eq_hp_hz;                     /* high-pass before it (25): the radio drops the very lows anyway */
    float tx_eq_gain_db;                   /* after it (-2): the equalizer lifts the lows and 3.2 kHz */
    char rx_eq[260];                       /* receive (rx_if): after the discriminator (empty: none) */
} radio_cfg_t;

/* IC-705 over USB: CI-V A4 115200, IF receive, precompensation with the values measured on 2026-11-18 (FM-D,
 * USB MOD Level 50 %): high-pass 110 Hz order 2, pre-emphasis 5000 us, +3 symbols -47.5 dBFS, +30 dB after it */
void radio_cfg_ic705(radio_cfg_t *c);

typedef enum { RE_LSF, RE_STREAM, RE_END } radio_evt_kind_t;

typedef struct {
    radio_evt_kind_t kind;
    bool have_lsf;                         /* lsf valid (from the LSF frame, or rebuilt from 6 LICH chunks) */
    bool lsf_from_lich;
    uint8_t lsf[M17_LSF_BYTES];
    m17_stream_rx_t s;                     /* RE_STREAM */
    float evm, gain;
    bool signal_lost;                      /* RE_END: no EoT, the signal went away */
} radio_evt_t;

typedef struct {
    float in_dbfs;                         /* input level, slow average */
    unsigned tx_underruns;                 /* output ran dry while keyed (the frames came too late) */
    unsigned frames_rx;
    unsigned pre_clips;                    /* samples the precompensation clipped (lower pre_gain_db) */
    unsigned eq_clips;                     /* samples the transmit equalizer clipped (lower tx_eq_gain_db) */
    float if_offset_hz;                    /* rx_if: frequency error of the received signal */
} radio_stats_t;

typedef struct radio radio_t;
typedef void (*radio_log_fn)(const char *fmt, ...);

void radio_default_cfg(radio_cfg_t *c);
radio_t *radio_open(const radio_cfg_t *c, radio_log_fn log);   /* NULL if a device or the port cannot be opened */
void radio_close(radio_t *r);                                   /* unkeys */

void radio_tx_begin(radio_t *r, const uint8_t lsf[M17_LSF_BYTES]);   /* key, delay, preamble, LSF */
/* the same with extra_ms more silence after keying: a cushion for packets from the internet that arrive unevenly */
void radio_tx_begin_ex(radio_t *r, const uint8_t lsf[M17_LSF_BYTES], unsigned extra_ms);
void radio_tx_frame(radio_t *r, uint16_t fn, bool last, const uint8_t payload[16]);
void radio_tx_end(radio_t *r);                                  /* EoT; unkeys once it has been played */
bool radio_keyed(const radio_t *r);
bool radio_full_duplex(const radio_t *r);                       /* loopback: receive while transmitting */

/* loopback tests: another station on the channel (symbols are modulated and mixed into the input, unkeyed) */
void radio_loop_inject(radio_t *r, const int8_t *sym, int n);
int radio_poll(radio_t *r, radio_evt_t *ev, int max);           /* call often (every ms): moves audio, returns events */
const radio_stats_t *radio_stats(const radio_t *r);

int radio_in_count(void);
int radio_out_count(void);
bool radio_in_name(int i, char *name, int size);
bool radio_out_name(int i, char *name, int size);
