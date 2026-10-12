#include "cfg_parse.h"

#include <stdio.h>
#include <string.h>

bool cfg_parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int parts = 0;
    if (!s) {
        return false;
    }
    for (;;) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        unsigned o = 0;
        int digits = 0;
        bool lead0 = (*s == '0');
        while (*s >= '0' && *s <= '9') {
            o = o * 10 + (unsigned)(*s - '0');
            if (++digits > 3 || o > 255) {
                return false;
            }
            s++;
        }
        if (lead0 && digits > 1) {
            return false;                 /* "01" is ambiguous (octal in some stacks) */
        }
        v = (v << 8) | o;
        if (++parts == 4) {
            break;
        }
        if (*s != '.') {
            return false;
        }
        s++;
    }
    if (*s) {
        return false;
    }
    *out = v;
    return true;
}

bool cfg_valid_netmask(uint32_t m)
{
    uint32_t inv = ~m;
    return m != 0 && (inv & (inv + 1)) == 0 && inv >= 3;     /* contiguous, /1../30 */
}

bool cfg_parse_netmask(const char *s, uint32_t *out)
{
    if (!s) {
        return false;
    }
    uint32_t m;
    if (s[0] == '/') {
        unsigned n = 0;
        int digits = 0;
        for (const char *p = s + 1; *p; ++p) {
            if (*p < '0' || *p > '9' || ++digits > 2) {
                return false;
            }
            n = n * 10 + (unsigned)(*p - '0');
        }
        if (digits == 0 || n < 1 || n > 30) {
            return false;
        }
        m = 0xFFFFFFFFu << (32 - n);
    } else if (!cfg_parse_ipv4(s, &m)) {
        return false;
    }
    if (!cfg_valid_netmask(m)) {
        return false;
    }
    *out = m;
    return true;
}

bool cfg_parse_port(const char *s, uint16_t *out)
{
    if (!s || !*s) {
        return false;
    }
    unsigned v = 0;
    int digits = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || ++digits > 5) {
            return false;
        }
        v = v * 10 + (unsigned)(*s - '0');
    }
    if (v < 1 || v > 65535) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

bool cfg_valid_hostname(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > CFG_HOST_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!alnum && c != '-') {
            return false;
        }
        if (c == '-' && (i == 0 || i == n - 1)) {
            return false;
        }
    }
    return true;
}

bool cfg_valid_ssid(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    return n >= 1 && n <= CFG_SSID_MAX;
}

bool cfg_valid_pass(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    return n == 0 || (n >= 8 && n <= CFG_PASS_MAX);
}

const char *cfg_check_static(uint32_t ip, uint32_t mask, uint32_t gw)
{
    if (!cfg_valid_netmask(mask)) {
        return "invalid netmask (must be contiguous, /1../30)";
    }
    if (ip == 0) {
        return "IP address is 0.0.0.0";
    }
    if ((ip >> 24) == 127) {
        return "IP address is a loopback address";
    }
    if ((ip >> 24) >= 224) {
        return "IP address is multicast/reserved";
    }
    if ((ip & ~mask) == 0) {
        return "IP address is the network address of the subnet";
    }
    if ((ip | mask) == 0xFFFFFFFFu) {
        return "IP address is the broadcast address of the subnet";
    }
    if (gw != 0) {                                  /* 0.0.0.0 = no gateway (LAN-only use) */
        if ((gw & mask) != (ip & mask)) {
            return "gateway is not in the same subnet as the IP address";
        }
        if (gw == ip) {
            return "gateway equals the IP address";
        }
        if ((gw & ~mask) == 0 || (gw | mask) == 0xFFFFFFFFu) {
            return "gateway is the network/broadcast address";
        }
    }
    return NULL;
}

void cfg_ipv4_str(uint32_t ip, char out[16])
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255),
             (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
}

void cfg_set_defaults(app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->hostname, "ambe3000");
    c->dhcp = true;
    c->ip   = 0xC0A80132u;     /* 192.168.1.50 (used only when switched to static) */
    c->mask = 0xFFFFFF00u;
    c->gw   = 0xC0A80101u;
    c->dns  = 0xC0A80101u;
    c->port = 2460;
}

bool cfg_validate(const app_config_t *c)
{
    if (memchr(c->ssid, 0, sizeof(c->ssid)) == NULL || memchr(c->pass, 0, sizeof(c->pass)) == NULL ||
        memchr(c->hostname, 0, sizeof(c->hostname)) == NULL) {
        return false;
    }
    if (c->ssid[0] && !cfg_valid_ssid(c->ssid)) {
        return false;
    }
    if (!cfg_valid_pass(c->pass) || !cfg_valid_hostname(c->hostname) || c->port == 0) {
        return false;
    }
    if (!c->dhcp && cfg_check_static(c->ip, c->mask, c->gw) != NULL) {
        return false;
    }
    return true;
}

int cfg_tokenize(char *line, char **argv, int max_argv)
{
    int argc = 0;
    char *r = line, *w = line;

    for (;;) {
        while (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n') {
            r++;
        }
        if (!*r) {
            break;
        }
        if (argc >= max_argv) {
            return -2;
        }
        argv[argc++] = w;
        bool q = false;
        while (*r) {
            if (!q && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n')) {
                break;
            }
            if (*r == '"') {
                q = !q;
                r++;
                continue;
            }
            if (q && *r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                r++;
            }
            *w++ = *r++;
        }
        if (q) {
            return -1;
        }
        if (*r) {
            r++;
        }
        *w++ = '\0';
    }
    return argc;
}
