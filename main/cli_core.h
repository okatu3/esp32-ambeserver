#pragma once
/* Setup console logic. Portable: all I/O and platform actions go through the two structs below,
 * so the same code runs on the ESP32 (UART) and in the host unit test. Messages are plain ASCII
 * (terminal code pages differ). */
#include <stdbool.h>
#include <stddef.h>
#include "app_config.h"

typedef struct {
    void (*puts)(const char *text);   /* text may contain '\n' (the UART layer sends "\r\n") */
    /* Prints prompt, reads one line. Returns its length, or -1 if the user aborted (Ctrl-C). */
    int  (*readline)(const char *prompt, char *buf, size_t size, bool secret);
} cli_io_t;

typedef struct {
    bool (*save)(const app_config_t *cfg);
    bool (*erase)(void);
    void (*reboot)(void);
    void (*status)(const cli_io_t *io);
    void (*scan)(const cli_io_t *io);
} cli_ops_t;

typedef struct {
    app_config_t cfg;                 /* working copy (what 'show' prints) */
    bool dirty;                       /* changed but not saved */
    const cli_io_t *io;
    const cli_ops_t *ops;
} cli_t;

void cli_init(cli_t *c, const app_config_t *loaded, const cli_io_t *io, const cli_ops_t *ops);
void cli_execute(cli_t *c, char *line);       /* modifies 'line' */
