/* m17_cli: the m17_pc station without a window, for tests and scripts.
 *
 *   m17_cli HOST CALLSIGN [options]                   (an M17 reflector, UDP 17000)
 *     --port N           UDP port                       --module X    module to link (A)
 *     --dest CALL        destination of our streams (@ALL)
 *     --time S           stop after S seconds (default: Ctrl+C)
 *     --tx-wav FILE      transmit this WAV (8 kHz mono 16 bit) once, starting --tx-at S seconds after linking
 *     --wav DIR          save received calls            -n            no speaker
 *     --mic N / --spk N  devices (see -l)               --jitter MS   --tx-gain dB   --rx-gain dB
 *   m17_cli parrot CALLSIGN [options]                 (the built-in test reflector on 127.0.0.1:--port, default 17000)
 *   m17_cli radio CALLSIGN [options]                  (a radio on the sound card, 4FSK made here, see radio_win.h)
 *     --rin N / --rout N sound input / output of the radio (see -l)
 *     --ptt COM5:rts     PTT on a serial port: rts, dtr or civ (Icom; --civ-addr A4 --civ-baud 115200)
 *     --tx-level dBFS    level of the +3 symbols (-10)  --tx-delay MS  silence after keying (150)  --tx-invert
 *     --loop             no sound card: the output goes straight back to the input (tests)
 *     --loop-snr dB      with noise                     --loop-voice-band  through 300 Hz - 3 kHz filters both ways
 *   m17_cli HOST CALLSIGN --relay [radio options]    (relay: the radio and a reflector at the same time)
 *     radio -> reflector is on (--no-relay-to-net turns it off); --relay-to-radio also transmits the reflector's
 *     calls on the radio, with our callsign as source (--relay-keep-src: the caller's).
 *     --test-rf WAV:CALL:S  with --loop: CALL calls on the radio channel S seconds after the start (tests)
 *   m17_cli -l           list microphones and speakers
 * A transmission is started here only by --tx-wav: this program has no PTT key. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_win.h"
#include "license_check.h"
#include "m17_engine.h"
#include "m17_parrot.h"
#include "mic_win.h"

static volatile sig_atomic_t s_stop;

static void on_signal(int sig)
{
    (void)sig;
    s_stop = 1;
}

static void log_line(const char *line)
{
    fprintf(stderr, "%s\n", line);
}

static void upper(char *p)
{
    for (; *p; ++p) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 32);
        }
    }
}

int main(int argc, char **argv)
{
    me_cfg_t c;
    me_default_cfg(&c);
    double run_s = 0, tx_at = 3;
    if (argc > 2 && strcmp(argv[1], "--check") == 0) {   /* prints the licence search address (sends nothing) */
        char base[16], url[400];
        license_base_call(argv[2], base);
        if (!license_is_japanese(base)) {
            fprintf(stderr, "%s: not a Japanese amateur callsign, no check\n", base);
            return 2;
        }
        license_search_url(base, url, sizeof(url));
        printf("%s\n", url);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        fprintf(stderr, "microphones:\n");
        mic_list_devices();
        fprintf(stderr, "speakers:\n");
        audio_list_devices();
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: m17_cli HOST CALLSIGN [--port N] [--module X] [--dest CALL] [--time S] [--tx-wav F]\n"
                        "       [--tx-at S] [--wav DIR] [-n] [--mic N] [--spk N] [--jitter MS]\n"
                        "       [--tx-gain dB] [--rx-gain dB]\n"
                        "   or: m17_cli parrot CALLSIGN [same options]   (built-in test reflector on 127.0.0.1)\n"
                        "   or: m17_cli radio CALLSIGN [same options] [--rin N] [--rout N] [--ptt COM5:rts|dtr|civ]\n"
                        "       [--civ-addr A4] [--civ-baud 115200] [--tx-level dBFS] [--tx-delay MS] [--tx-invert]\n"
                        "       [--loop [--loop-snr dB] [--loop-voice-band]]   (a radio on the sound card; --loop: tests)\n"
                        "   or: m17_cli HOST CALLSIGN --relay [radio options] [--relay-to-radio] [--relay-keep-src]\n"
                        "       [--no-relay-to-net] [--test-rf WAV:CALL:SECONDS]   (relay between a radio and a reflector)\n"
                        "   or: m17_cli -l\n");
        return 2;
    }
    snprintf(c.host, sizeof(c.host), "%s", argv[1]);
    snprintf(c.callsign, sizeof(c.callsign), "%s", argv[2]);
    upper(c.callsign);
    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--port")) { c.port = (uint16_t)atoi(v); ++i; }
        else if (!strcmp(a, "--module")) { c.module = v[0]; ++i; }
        else if (!strcmp(a, "--dest")) { snprintf(c.dest, sizeof(c.dest), "%s", v); upper(c.dest); ++i; }
        else if (!strcmp(a, "--time")) { run_s = atof(v); ++i; }
        else if (!strcmp(a, "--tx-wav")) { snprintf(c.tx_wav, sizeof(c.tx_wav), "%s", v); ++i; }
        else if (!strcmp(a, "--tx-at")) { tx_at = atof(v); ++i; }
        else if (!strcmp(a, "--wav")) { snprintf(c.wav_dir, sizeof(c.wav_dir), "%s", v); ++i; }
        else if (!strcmp(a, "-n")) { c.no_speaker = true; }
        else if (!strcmp(a, "--mic")) { c.mic_device = atoi(v); ++i; }
        else if (!strcmp(a, "--spk")) { c.spk_device = atoi(v); ++i; }
        else if (!strcmp(a, "--jitter")) { c.jitter_ms = (unsigned)atoi(v); ++i; }
        else if (!strcmp(a, "--tx-gain")) { c.tx_gain_db = (float)atof(v); ++i; }
        else if (!strcmp(a, "--rx-gain")) { c.rx_gain_db = (float)atof(v); ++i; }
        else if (!strcmp(a, "--rin")) { c.rc.in_device = atoi(v); ++i; }
        else if (!strcmp(a, "--rout")) { c.rc.out_device = atoi(v); ++i; }
        else if (!strcmp(a, "--loop")) { c.rc.in_device = c.rc.out_device = RADIO_DEV_LOOP; }
        else if (!strcmp(a, "--loop-snr")) { c.rc.loop_snr_db = (float)atof(v); ++i; }
        else if (!strcmp(a, "--loop-voice-band")) { c.rc.loop_voice_band = true; }
        else if (!strcmp(a, "--ptt")) {                     /* COM5:rts | COM5:dtr | COM5:civ */
            char port[16] = "", how[8] = "";
            sscanf(v, "%15[^:]:%7s", port, how);
            snprintf(c.rc.ptt_port, sizeof(c.rc.ptt_port), "%s", port);
            c.rc.ptt = !_stricmp(how, "dtr") ? RP_DTR : (!_stricmp(how, "civ") ? RP_CIV : RP_RTS);
            ++i;
        }
        else if (!strcmp(a, "--civ-addr")) { c.rc.civ_addr = (unsigned)strtoul(v, NULL, 16); ++i; }
        else if (!strcmp(a, "--civ-baud")) { c.rc.civ_baud = (unsigned)atoi(v); ++i; }
        else if (!strcmp(a, "--tx-level")) { c.rc.tx_dbfs = (float)atof(v); ++i; }
        else if (!strcmp(a, "--tx-delay")) { c.rc.tx_delay_ms = (unsigned)atoi(v); ++i; }
        else if (!strcmp(a, "--tx-invert")) { c.rc.tx_invert = true; }
        else if (!strcmp(a, "--relay")) { c.radio = c.gateway = true; }
        else if (!strcmp(a, "--relay-to-radio")) { c.gw_net_to_rf = true; }
        else if (!strcmp(a, "--no-relay-to-net")) { c.gw_rf_to_net = false; }
        else if (!strcmp(a, "--relay-keep-src")) { c.gw_keep_src = true; }
        else if (!strcmp(a, "--test-rf")) {                 /* WAV[:CALL[:SECONDS]] */
            char w[260] = "", call[16] = "", at[16] = "";
            snprintf(w, sizeof(w), "%s", v);
            /* the WAV path may hold a drive letter: split only on colons after it */
            char *q = strchr(w + (w[1] == ':' ? 2 : 0), ':');
            if (q) {
                *q++ = 0;
                sscanf(q, "%15[^:]:%15s", call, at);
            }
            snprintf(c.test_rf_wav, sizeof(c.test_rf_wav), "%s", w);
            snprintf(c.test_rf_call, sizeof(c.test_rf_call), "%s", call);
            upper(c.test_rf_call);
            c.test_rf_at = at[0] ? (float)atof(at) : 2.0f;
            ++i;
        }
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, on_signal);
    if (!strcmp(c.host, "radio")) {
        c.radio = true;
        c.gateway = false;
    }
    bool parrot = !strcmp(c.host, "parrot");
    if (parrot) {
        if (!c.port) {
            c.port = PARROT_PORT;
        }
        if (!parrot_start(c.port, log_line)) {
            return 1;
        }
        snprintf(c.host, sizeof(c.host), "127.0.0.1");
    }
    if (!me_start(&c, log_line)) {
        fprintf(stderr, "cannot start\n");
        return 1;
    }
    DWORD t0 = GetTickCount(), linked_at = 0;
    bool tx_done = false;
    while (!s_stop && me_running()) {
        Sleep(50);
        me_status_t st;
        me_get_status(&st);
        if (st.link == ML_UP && !linked_at) {
            linked_at = GetTickCount();
        }
        if (c.tx_wav[0] && linked_at && !tx_done && GetTickCount() - linked_at > tx_at * 1000) {
            me_set_ptt(true);                  /* released by the engine when the WAV ends */
            tx_done = true;
        }
        if (run_s > 0 && GetTickCount() - t0 > run_s * 1000) {
            break;
        }
    }
    me_stop();
    if (parrot) {
        parrot_stop();
    }
    me_status_t st;
    me_get_status(&st);
    fprintf(stderr, "totals: %u stream packets transmitted, %u calls received\n", (unsigned)st.tx_packets_total,
            (unsigned)st.rx_calls_total);
    return 0;
}
