#pragma once
/* The front panel of the M17 modem: a menu for a rotary encoder (turn, push, long push) and a small screen of
 * UI_COLS x UI_ROWS characters (a 128x64 LCD with a 6x8 font). The menu does not touch hardware: it reads the modem's
 * state through app_status() and acts through the app_*() calls, which main.c provides. Until the encoder and the
 * screen are fitted, the serial console drives it (r / l / p / b) and prints the screen.
 *
 *   HOME  status: mode, destination, the relay's link, the station heard, input level
 *   layer 1  how to connect: DIRECT CQ / VIA RELAY / REFLECTOR / SETTINGS
 *   layer 2  what: CQ or a station heard; relay functions; the stored reflectors; the settings
 *   layer 3  details: a station, a command to send, the module of a reflector, a value to change */
#include <stdbool.h>
#include <stdint.h>

#define UI_COLS 21
#define UI_ROWS 8
#define UI_HEARD_N 8
#define UI_REFL_N 12

typedef enum { UI_LEFT, UI_RIGHT, UI_PUSH, UI_BACK } ui_event_t;   /* UI_BACK: long push */

typedef enum { UI_MODE_DIRECT, UI_MODE_RELAY, UI_MODE_REFLECTOR } ui_mode_t;

typedef struct {
    char call[10];
    char dst[10];                 /* destination of our voice calls */
    bool dst_once;                /* dst is for the next transmission only (ECHO) */
    ui_mode_t mode;
    bool tx, rx;
    char rx_src[10], rx_dst[10];
    float rx_evm;
    char relay_state[10];         /* last answer of the relay: "M17-XXX A", "UNLINKED", "LINK FAIL", "NOT FOUND", "" */
    char relay_call[10];          /* the relay that answered */
    uint32_t relay_age_s;
    bool cmd_pending;             /* a command was sent, no answer yet */
    float line_dbfs;
    float tx_dbfs;
    int mic_db, vol, can;
    int heard_n;
    char heard[UI_HEARD_N][10];
    uint32_t heard_age_s[UI_HEARD_N];
    int refl_n;
    char refl[UI_REFL_N][8];
} ui_status_t;

/* provided by main.c */
void app_status(ui_status_t *s);
void app_set_mode(ui_mode_t m);
void app_set_dst(const char *dst, bool once);
void app_send_command(const char *dst);      /* a short transmission to this destination (relay command) */
void app_set_call(const char *call);
void app_set_tx_level(float dbfs);
void app_set_mic(int db);
void app_set_vol(int vol);
void app_set_can(int can);

void ui_init(void);
void ui_event(ui_event_t e);
void ui_render(char rows[UI_ROWS][UI_COLS + 1]);
