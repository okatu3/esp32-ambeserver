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
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, on_signal);
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
