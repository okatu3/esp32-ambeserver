#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    S_HOME, S_L1, S_DIRECT, S_RELAY, S_REFL, S_SET,        /* home, layer 1, layer 2 */
    S_STATION, S_CMD, S_REFL_LINK, S_VALUE, S_CALL         /* layer 3 */
} screen_t;

enum { CMD_INFO, CMD_ECHO, CMD_UNLINK };
enum { SET_CALL, SET_LEVEL, SET_MIC, SET_VOL, SET_CAN, SET_N };

static const char CALL_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";

static struct {
    screen_t scr;
    int sel, top;                 /* cursor and first row shown in a list */
    int l1_sel, l2_sel;           /* where to come back to */
    int cmd;                      /* S_CMD */
    char station[10];             /* S_STATION */
    int refl;                     /* S_REFL_LINK: which reflector */
    char module;
    bool editing;                 /* S_REFL_LINK: turning changes the module */
    int setting;                  /* S_VALUE / S_CALL */
    int value;
    char call[10];                /* S_CALL */
    int pos;
    char msg[UI_COLS + 1];        /* one line on HOME after an action */
} U;

static ui_status_t S;             /* the modem's state while handling an event or drawing */

void ui_init(void)
{
    memset(&U, 0, sizeof(U));
    U.module = 'A';
}

/* ---------------------------------------------------------------- lists */
static int list_len(void)
{
    switch (U.scr) {
    case S_L1: return 4;
    case S_DIRECT: return 1 + S.heard_n;
    case S_RELAY: return 4;
    case S_REFL: return S.refl_n ? S.refl_n : 1;
    case S_SET: return SET_N;
    case S_STATION: return 2;
    case S_CMD: return 2;
    case S_REFL_LINK: return 3;
    default: return 0;
    }
}

static void go(screen_t s, int sel)
{
    U.scr = s;
    U.sel = sel;
    U.top = 0;
    U.editing = false;
}

static void go_home(const char *msg)
{
    go(S_HOME, 0);
    snprintf(U.msg, sizeof(U.msg), "%s", msg ? msg : "");
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void setting_range(int k, int *lo, int *hi)
{
    static const int r[SET_N][2] = { { 0, 0 }, { -30, 0 }, { 0, 24 }, { 0, 33 }, { 0, 15 } };
    *lo = r[k][0];
    *hi = r[k][1];
}

static int setting_value(int k)
{
    switch (k) {
    case SET_LEVEL: return (int)(S.tx_dbfs - 0.5f);
    case SET_MIC: return S.mic_db;
    case SET_VOL: return S.vol;
    case SET_CAN: return S.can;
    default: return 0;
    }
}

static void callsign_from_edit(char *out)
{
    snprintf(out, 10, "%s", U.call);
    for (int i = (int)strlen(out) - 1; i >= 0 && out[i] == ' '; --i) {
        out[i] = 0;
    }
}

/* ---------------------------------------------------------------- events */
static void push(void)
{
    switch (U.scr) {
    case S_HOME:
        go(S_L1, U.l1_sel);
        break;
    case S_L1:
        U.l1_sel = U.sel;
        go(U.sel == 0 ? S_DIRECT : U.sel == 1 ? S_RELAY : U.sel == 2 ? S_REFL : S_SET, 0);
        break;
    case S_DIRECT:
        if (U.sel == 0) {
            app_set_mode(UI_MODE_DIRECT);
            app_set_dst("@ALL", false);
            go_home("CQ: talk with PTT");
        } else {
            U.l2_sel = U.sel;
            snprintf(U.station, sizeof(U.station), "%s", S.heard[U.sel - 1]);
            go(S_STATION, 0);
        }
        break;
    case S_STATION:
        if (U.sel == 0) {
            app_set_mode(UI_MODE_DIRECT);
            app_set_dst(U.station, false);
            char m[UI_COLS + 1];
            snprintf(m, sizeof(m), "Calling %s", U.station);
            go_home(m);
        } else {
            go(S_DIRECT, U.l2_sel);
        }
        break;
    case S_RELAY:
        U.l2_sel = U.sel;
        if (U.sel == 0) {
            app_set_mode(UI_MODE_RELAY);
            app_set_dst("@ALL", false);
            go_home("Relay: talk with PTT");
        } else {
            U.cmd = U.sel == 1 ? CMD_INFO : U.sel == 2 ? CMD_ECHO : CMD_UNLINK;
            go(S_CMD, 0);
        }
        break;
    case S_CMD:
        if (U.sel == 1) {
            go(S_RELAY, U.l2_sel);
        } else if (U.cmd == CMD_ECHO) {
            app_set_mode(UI_MODE_RELAY);
            app_set_dst("ECHO", true);
            go_home("ECHO: talk, then wait");
        } else {
            app_send_command(U.cmd == CMD_INFO ? "INFO" : "UNLINK");
            go_home(U.cmd == CMD_INFO ? "Sent INFO" : "Sent UNLINK");
        }
        break;
    case S_REFL:
        if (S.refl_n) {
            U.l2_sel = U.sel;
            U.refl = U.sel;
            go(S_REFL_LINK, 1);
        }
        break;
    case S_REFL_LINK:
        if (U.sel == 0) {
            U.editing = !U.editing;
        } else if (U.sel == 1) {
            char d[10];
            snprintf(d, sizeof(d), "%.7s %c", S.refl[U.refl], U.module);
            app_send_command(d);
            app_set_mode(UI_MODE_REFLECTOR);
            app_set_dst("@ALL", false);
            char m[UI_COLS + 1];
            snprintf(m, sizeof(m), "Linking %s", d);
            go_home(m);
        } else {
            go(S_REFL, U.l2_sel);
        }
        break;
    case S_SET:
        U.l2_sel = U.sel;
        U.setting = U.sel;
        if (U.sel == SET_CALL) {
            memset(U.call, ' ', 9);
            U.call[9] = 0;
            memcpy(U.call, S.call, strlen(S.call));
            U.pos = 0;
            go(S_CALL, 0);
        } else {
            U.value = setting_value(U.sel);
            go(S_VALUE, 0);
        }
        break;
    case S_VALUE:
        switch (U.setting) {
        case SET_LEVEL: app_set_tx_level((float)U.value); break;
        case SET_MIC: app_set_mic(U.value); break;
        case SET_VOL: app_set_vol(U.value); break;
        case SET_CAN: app_set_can(U.value); break;
        }
        go(S_SET, U.l2_sel);
        break;
    case S_CALL:
        if (++U.pos >= 9) {
            U.pos = 0;
        }
        break;
    }
}

static void back(void)
{
    switch (U.scr) {
    case S_HOME: break;
    case S_L1: go_home(NULL); break;
    case S_DIRECT: case S_RELAY: case S_REFL: case S_SET: go(S_L1, U.l1_sel); break;
    case S_STATION: go(S_DIRECT, U.l2_sel); break;
    case S_CMD: go(S_RELAY, U.l2_sel); break;
    case S_REFL_LINK: go(S_REFL, U.l2_sel); break;
    case S_VALUE: go(S_SET, U.l2_sel); break;                /* not saved */
    case S_CALL: {                                           /* long push ends the editing and keeps it */
        char c[10];
        callsign_from_edit(c);
        if (c[0] && c[0] != ' ') {
            app_set_call(c);
        }
        go(S_SET, U.l2_sel);
        break;
    }
    }
}

static void turn(int d)
{
    if (U.scr == S_VALUE) {
        int lo, hi;
        setting_range(U.setting, &lo, &hi);
        U.value = clampi(U.value + d, lo, hi);
        return;
    }
    if (U.scr == S_CALL) {
        const char *p = strchr(CALL_CHARS, U.call[U.pos]);
        int i = p ? (int)(p - CALL_CHARS) : 0, n = (int)strlen(CALL_CHARS);
        U.call[U.pos] = CALL_CHARS[(i + d + n) % n];
        return;
    }
    if (U.scr == S_REFL_LINK && U.editing) {
        U.module = (char)('A' + ((U.module - 'A' + d + 26) % 26));
        return;
    }
    if (U.scr == S_HOME) {
        return;
    }
    U.sel = clampi(U.sel + d, 0, list_len() - 1);
}

void ui_event(ui_event_t e)
{
    app_status(&S);
    if (U.scr != S_HOME) {
        U.msg[0] = 0;
    }
    switch (e) {
    case UI_LEFT: turn(-1); break;
    case UI_RIGHT: turn(+1); break;
    case UI_PUSH: push(); break;
    case UI_BACK: back(); break;
    }
}

/* ---------------------------------------------------------------- drawing */
static char (*R)[UI_COLS + 1];

static void put(int row, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(int row, const char *fmt, ...)
{
    char t[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    snprintf(R[row], UI_COLS + 1, "%-21.21s", t);
}

static void list(const char *title, const char *const *items, int n)
{
    put(0, "%s", title);
    if (U.sel < U.top) {
        U.top = U.sel;
    }
    if (U.sel >= U.top + 6) {
        U.top = U.sel - 5;
    }
    for (int r = 0; r < 6; ++r) {
        int i = U.top + r;
        if (i < n) {
            put(1 + r, "%c%s", i == U.sel ? '>' : ' ', items[i]);
        } else {
            put(1 + r, "%s", "");
        }
    }
    put(7, "turn  push  long:back");
}

static const char *mode_name(ui_mode_t m)
{
    return m == UI_MODE_DIRECT ? "DIRECT CQ" : m == UI_MODE_RELAY ? "VIA RELAY" : "REFLECTOR";
}

static void bar(char *out, float db)             /* -60..0 dB -> 10 cells */
{
    int n = clampi((int)((db + 60.0f) / 6.0f), 0, 10);
    for (int i = 0; i < 10; ++i) {
        out[i] = i < n ? '#' : '.';
    }
    out[10] = 0;
}

static void age(char *out, size_t n, uint32_t s)
{
    if (s < 60) {
        snprintf(out, n, "%us", (unsigned)s);
    } else if (s < 3600) {
        snprintf(out, n, "%um", (unsigned)(s / 60));
    } else {
        snprintf(out, n, "%uh", (unsigned)(s / 3600) % 1000u);
    }
}

static void draw_home(void)
{
    put(0, "M17 %-9s %s", S.call, S.tx ? "  TX" : (S.rx ? "  RX" : ""));
    put(1, "%s", mode_name(S.mode));
    put(2, "To %s%s", S.dst, S.dst_once ? " (next)" : "");
    if (S.mode == UI_MODE_DIRECT) {
        put(3, "%s", "");
    } else if (S.cmd_pending) {
        put(3, "Link: waiting...");
    } else if (S.relay_state[0]) {
        char a[8];
        age(a, sizeof(a), S.relay_age_s);
        put(3, "Link %-9s %s", S.relay_state, a);
    } else {
        put(3, "Link: ? (INFO)");
    }
    if (S.rx) {
        put(4, "RX %s", S.rx_src);
        put(5, "  >%s EVM %.2f", S.rx_dst, S.rx_evm);
    } else if (S.heard_n) {
        char a[8];
        age(a, sizeof(a), S.heard_age_s[0]);
        put(4, "Last %s %s", S.heard[0], a);
        put(5, "%s", "");
    } else {
        put(4, "%s", "");
        put(5, "%s", "");
    }
    char b[11];
    bar(b, S.line_dbfs);
    put(6, "In%4.0f %s", S.line_dbfs < -99 ? -99.0f : S.line_dbfs, b);
    put(7, "%s", U.msg[0] ? U.msg : "push: menu");
}

void ui_render(char rows[UI_ROWS][UI_COLS + 1])
{
    app_status(&S);
    R = rows;
    switch (U.scr) {
    case S_HOME:
        draw_home();
        break;
    case S_L1: {
        static const char *const it[] = { "DIRECT CQ", "VIA RELAY", "REFLECTOR", "SETTINGS" };
        list("CONNECT", it, 4);
        break;
    }
    case S_DIRECT: {
        char lines[1 + UI_HEARD_N][22];
        const char *it[1 + UI_HEARD_N];
        snprintf(lines[0], 22, "CQ (@ALL)");
        for (int i = 0; i < S.heard_n; ++i) {
            char a[8];
            age(a, sizeof(a), S.heard_age_s[i]);
            snprintf(lines[1 + i], 22, "%-9s %s", S.heard[i], a);
        }
        for (int i = 0; i <= S.heard_n; ++i) {
            it[i] = lines[i];
        }
        list("DIRECT CQ", it, 1 + S.heard_n);
        break;
    }
    case S_STATION: {
        static const char *const it[] = { "Call this station", "Back" };
        list("STATION", it, 2);
        put(1, " %s", U.station);
        put(2, "%c%s", U.sel == 0 ? '>' : ' ', it[0]);
        put(3, "%c%s", U.sel == 1 ? '>' : ' ', it[1]);
        for (int r = 4; r < 7; ++r) {
            put(r, "%s", "");
        }
        break;
    }
    case S_RELAY: {
        static const char *const it[] = { "Talk (@ALL)", "INFO  where linked", "ECHO  test my audio", "UNLINK" };
        char t[24];
        snprintf(t, sizeof(t), "RELAY %s", S.relay_state[0] ? S.relay_state : "?");
        list(t, it, 4);
        break;
    }
    case S_CMD: {
        static const char *const send[] = { "Send", "Cancel" };
        static const char *const ready[] = { "Ready (next PTT)", "Cancel" };
        list(U.cmd == CMD_ECHO ? "ECHO" : U.cmd == CMD_INFO ? "INFO" : "UNLINK",
             U.cmd == CMD_ECHO ? ready : send, 2);
        put(1, "%s", U.cmd == CMD_INFO ? " Ask the relay where" : U.cmd == CMD_ECHO ? " Talk with PTT; the" : " Disconnect the");
        put(2, "%s", U.cmd == CMD_INFO ? " it is linked." : U.cmd == CMD_ECHO ? " relay plays it back." : " relay's reflector.");
        put(3, "%s", "");
        put(4, "%c%s", U.sel == 0 ? '>' : ' ', U.cmd == CMD_ECHO ? ready[0] : send[0]);
        put(5, "%c%s", U.sel == 1 ? '>' : ' ', "Cancel");
        put(6, "%s", "");
        break;
    }
    case S_REFL: {
        const char *it[UI_REFL_N];
        static const char *const none[] = { "(none: add by serial)" };
        for (int i = 0; i < S.refl_n; ++i) {
            it[i] = S.refl[i];
        }
        list("REFLECTOR", S.refl_n ? it : none, S.refl_n ? S.refl_n : 1);
        break;
    }
    case S_REFL_LINK: {
        char m[22];
        snprintf(m, sizeof(m), "Module %c%s", U.module, U.editing ? "  <turn>" : "");
        const char *it[3] = { m, "Link", "Back" };
        list(S.refl[U.refl], it, 3);
        break;
    }
    case S_SET: {
        char lines[SET_N][22];
        snprintf(lines[SET_CALL], 22, "Callsign  %s", S.call);
        snprintf(lines[SET_LEVEL], 22, "TX level  %.0f dB", S.tx_dbfs);
        snprintf(lines[SET_MIC], 22, "Mic gain  %d dB", S.mic_db);
        snprintf(lines[SET_VOL], 22, "Volume    %d", S.vol);
        snprintf(lines[SET_CAN], 22, "CAN       %d", S.can);
        const char *it[SET_N];
        for (int i = 0; i < SET_N; ++i) {
            it[i] = lines[i];
        }
        list("SETTINGS", it, SET_N);
        break;
    }
    case S_VALUE: {
        static const char *const names[SET_N] = { "", "TX LEVEL (dBFS)", "MIC GAIN (dB)", "VOLUME", "CAN (0-15)" };
        put(0, "%s", names[U.setting]);
        put(1, "%s", "");
        put(2, "%s", "");
        put(3, "        %d", U.value);
        put(4, "%s", "");
        put(5, "%s", "");
        put(6, "%s", "");
        put(7, "turn push:save long:x");
        break;
    }
    case S_CALL: {
        put(0, "CALLSIGN");
        put(1, "%s", "");
        put(2, "  %s", U.call);
        char caret[UI_COLS + 1];
        memset(caret, ' ', sizeof(caret));
        caret[2 + U.pos] = '^';
        caret[3 + U.pos] = 0;
        put(3, "%s", caret);
        put(4, "%s", "");
        put(5, " turn: letter");
        put(6, " push: next letter");
        put(7, " long: save");
        break;
    }
    }
}
