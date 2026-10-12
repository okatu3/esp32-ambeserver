#pragma once
/* Persistent settings of the Wi-Fi server. Portable header (also built on the host for tests). */
#include <stdbool.h>
#include <stdint.h>

#define CFG_SSID_MAX 32
#define CFG_PASS_MAX 64
#define CFG_HOST_MAX 31

typedef struct {
    char     ssid[CFG_SSID_MAX + 1];
    char     pass[CFG_PASS_MAX + 1];      /* "" = open network */
    char     hostname[CFG_HOST_MAX + 1];
    bool     dhcp;
    uint32_t ip, mask, gw, dns;           /* host byte order, e.g. 192.168.1.50 = 0xC0A80132 */
    uint16_t port;                        /* TCP port of the DV3000 server */
} app_config_t;

/* cfg_parse.c (portable) */
void cfg_set_defaults(app_config_t *c);

/* app_config_nvs.c (ESP-IDF only) */
bool app_config_nvs_init(void);
bool app_config_load(app_config_t *c);    /* false = nothing valid stored; *c holds the defaults */
bool app_config_save(const app_config_t *c);
bool app_config_erase(void);
