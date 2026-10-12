#pragma once
/* Pure, portable parsing/validation helpers (no ESP-IDF dependency). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"

bool cfg_parse_ipv4(const char *s, uint32_t *out);        /* strict dotted quad, no leading zeros */
bool cfg_parse_netmask(const char *s, uint32_t *out);     /* "255.255.255.0" or "/24" (/1../30) */
bool cfg_valid_netmask(uint32_t mask);
bool cfg_parse_port(const char *s, uint16_t *out);        /* 1..65535 */
bool cfg_valid_hostname(const char *s);                   /* 1..31 chars, [A-Za-z0-9-], no leading/trailing '-' */
bool cfg_valid_ssid(const char *s);                       /* 1..32 bytes */
bool cfg_valid_pass(const char *s);                       /* "" (open) or 8..64 chars */
const char *cfg_check_static(uint32_t ip, uint32_t mask, uint32_t gw);   /* NULL = OK, else reason */
bool cfg_validate(const app_config_t *c);                 /* whole-struct sanity check (used for NVS data) */
void cfg_ipv4_str(uint32_t ip, char out[16]);

/* In-place tokenizer: whitespace separated, "double quotes" group, \" and \\ escapes inside quotes.
 * Returns argc, -1 = unterminated quote, -2 = too many tokens. */
int cfg_tokenize(char *line, char **argv, int max_argv);
