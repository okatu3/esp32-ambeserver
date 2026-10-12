#include "cli_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cfg_parse.h"

static void say(cli_t *c, const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    c->io->puts(b);
}

static void copy_str(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool is(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

void cli_init(cli_t *c, const app_config_t *loaded, const cli_io_t *io, const cli_ops_t *ops)
{
    memset(c, 0, sizeof(*c));
    c->cfg = *loaded;
    c->io = io;
    c->ops = ops;
}

static void mark_dirty(cli_t *c)
{
    c->dirty = true;
    say(c, "OK (not saved yet - use 'save', or 'apply' to save and reboot)\n");
}

static void print_ip(cli_t *c, const app_config_t *g)
{
    char a[16], m[16], w[16], d[16];
    cfg_ipv4_str(g->ip, a);
    cfg_ipv4_str(g->mask, m);
    cfg_ipv4_str(g->gw, w);
    cfg_ipv4_str(g->dns, d);
    if (g->dhcp) {
        say(c, "IP mode    : DHCP\n");
    } else {
        say(c, "IP mode    : static\n  IP       : %s\n  Netmask  : %s\n  Gateway  : %s\n  DNS      : %s\n", a, m, w, d);
    }
}

static void show(cli_t *c)
{
    const app_config_t *g = &c->cfg;
    say(c, "Wi-Fi SSID : %s\n", g->ssid[0] ? g->ssid : "(not set)");
    say(c, "Password   : %s\n", g->pass[0] ? "********" : "(none, open network)");
    say(c, "Hostname   : %s\n", g->hostname);
    print_ip(c, g);
    say(c, "TCP port   : %u\n", (unsigned)g->port);
    if (c->dirty) {
        say(c, "** unsaved changes: 'save' or 'apply' **\n");
    }
}

static void help(cli_t *c)
{
    /* puts, not say(): the text is longer than say()'s buffer */
    c->io->puts(
        "Commands:\n"
        "  setup                              step-by-step wizard\n"
        "  show                               show settings\n"
        "  status                             Wi-Fi / IP / server state\n"
        "  scan                               list access points\n"
        "  wifi ssid <name>                   (use \"quotes\" if it has spaces)\n"
        "  wifi pass <password>               8-64 chars, \"\" = open network\n"
        "  ip dhcp                            get the address by DHCP\n"
        "  ip static <ip> <mask> <gw> [dns]   e.g. ip static 192.168.1.50 /24 192.168.1.1\n"
        "  port <1-65535>                     TCP port (default 2460)\n"
        "  hostname <name>\n"
        "  save                               store settings in flash\n"
        "  apply                              save, then reboot\n"
        "  reboot [force]\n"
        "  factory-reset                      erase all settings\n"
        "Settings take effect after a reboot. Ctrl-C cancels a prompt.\n");
}

/* ---------------------------------------------------------------- wizard */
#define ASK_CANCEL (-1)

static int ask(cli_t *c, const char *prompt, char *buf, size_t n, bool secret)
{
    int r = c->io->readline(prompt, buf, n, secret);
    if (r < 0) {
        buf[0] = '\0';
        return ASK_CANCEL;
    }
    /* trim */
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
        buf[--len] = '\0';
    }
    size_t lead = 0;
    while (buf[lead] == ' ' || buf[lead] == '\t') {
        lead++;
    }
    if (lead) {
        memmove(buf, buf + lead, strlen(buf + lead) + 1);
    }
    return 0;
}

/* Re-asks until valid or empty (= keep). kind: 0 ip, 1 netmask, 2 ip that may be 0.0.0.0 */
static int ask_addr(cli_t *c, const char *label, uint32_t *val, int kind)
{
    char buf[64], cur[16], prompt[96];
    for (;;) {
        cfg_ipv4_str(*val, cur);
        snprintf(prompt, sizeof(prompt), "%s [%s]: ", label, cur);
        if (ask(c, prompt, buf, sizeof(buf), false) == ASK_CANCEL) {
            return ASK_CANCEL;
        }
        if (!buf[0]) {
            return 0;
        }
        uint32_t v;
        bool ok = (kind == 1) ? cfg_parse_netmask(buf, &v) : cfg_parse_ipv4(buf, &v);
        if (ok) {
            *val = v;
            return 0;
        }
        say(c, "ERR: not a valid %s (example: %s)\n", kind == 1 ? "netmask" : "IPv4 address",
            kind == 1 ? "255.255.255.0 or /24" : "192.168.1.50");
    }
}

static void wizard(cli_t *c)
{
    app_config_t w = c->cfg;
    char buf[128], prompt[128];

    say(c, "--- Setup wizard (Enter = keep the value in [ ], Ctrl-C = cancel) ---\n");

    for (;;) {
        snprintf(prompt, sizeof(prompt), "Wi-Fi SSID [%s]: ", w.ssid[0] ? w.ssid : "(none)");
        if (ask(c, prompt, buf, sizeof(buf), false) == ASK_CANCEL) goto cancelled;
        if (!buf[0]) break;
        if (!cfg_valid_ssid(buf)) {
            say(c, "ERR: SSID must be 1-32 bytes\n");
            continue;
        }
        copy_str(w.ssid, sizeof(w.ssid), buf);
        break;
    }
    for (;;) {
        if (ask(c, "Wi-Fi password [keep] (8-64 chars, '-' = open network): ", buf, sizeof(buf), true) == ASK_CANCEL) goto cancelled;
        if (!buf[0]) break;
        if (!strcmp(buf, "-")) {
            w.pass[0] = '\0';
            break;
        }
        if (!cfg_valid_pass(buf) || !buf[0]) {
            say(c, "ERR: password must be 8-64 characters\n");
            continue;
        }
        copy_str(w.pass, sizeof(w.pass), buf);
        break;
    }
    for (;;) {
        snprintf(prompt, sizeof(prompt), "Get the IP address by DHCP? (y/n) [%c]: ", w.dhcp ? 'y' : 'n');
        if (ask(c, prompt, buf, sizeof(buf), false) == ASK_CANCEL) goto cancelled;
        if (!buf[0]) break;
        if (is(buf, "y") || is(buf, "yes")) { w.dhcp = true; break; }
        if (is(buf, "n") || is(buf, "no"))  { w.dhcp = false; break; }
        say(c, "ERR: answer y or n\n");
    }
    if (!w.dhcp) {
        for (;;) {
            if (ask_addr(c, "IP address", &w.ip, 0) == ASK_CANCEL) goto cancelled;
            if (ask_addr(c, "Netmask (e.g. 255.255.255.0 or /24)", &w.mask, 1) == ASK_CANCEL) goto cancelled;
            if (ask_addr(c, "Gateway (0.0.0.0 = none)", &w.gw, 2) == ASK_CANCEL) goto cancelled;
            if (ask_addr(c, "DNS server", &w.dns, 2) == ASK_CANCEL) goto cancelled;
            const char *why = cfg_check_static(w.ip, w.mask, w.gw);
            if (!why) break;
            say(c, "ERR: %s\nPlease enter the static settings again.\n", why);
        }
    }
    for (;;) {
        snprintf(prompt, sizeof(prompt), "TCP port [%u]: ", (unsigned)w.port);
        if (ask(c, prompt, buf, sizeof(buf), false) == ASK_CANCEL) goto cancelled;
        if (!buf[0]) break;
        uint16_t p;
        if (cfg_parse_port(buf, &p)) { w.port = p; break; }
        say(c, "ERR: port must be 1-65535\n");
    }

    {
        cli_t tmp = *c;
        tmp.cfg = w;
        say(c, "\n--- New settings ---\n");
        show(&tmp);
    }
    if (ask(c, "Save and reboot now? (y/n) [n]: ", buf, sizeof(buf), false) == ASK_CANCEL) goto cancelled;
    if (is(buf, "y") || is(buf, "yes")) {
        c->cfg = w;
        if (!c->ops->save(&c->cfg)) {
            c->dirty = true;
            say(c, "ERR: could not write flash\n");
            return;
        }
        c->dirty = false;
        say(c, "Saved. Rebooting...\n");
        c->ops->reboot();
        return;
    }
    say(c, "Cancelled. Nothing changed.\n");
    return;

cancelled:
    say(c, "Cancelled. Nothing changed.\n");
}

/* ---------------------------------------------------------------- commands */
static void cmd_wifi(cli_t *c, int argc, char **argv)
{
    if (argc == 1) {
        say(c, "Wi-Fi SSID : %s\nPassword   : %s\n", c->cfg.ssid[0] ? c->cfg.ssid : "(not set)",
            c->cfg.pass[0] ? "********" : "(none, open network)");
    } else if (argc == 3 && is(argv[1], "ssid")) {
        if (!cfg_valid_ssid(argv[2])) {
            say(c, "ERR: SSID must be 1-32 bytes\n");
            return;
        }
        copy_str(c->cfg.ssid, sizeof(c->cfg.ssid), argv[2]);
        mark_dirty(c);
    } else if (argc == 3 && is(argv[1], "pass")) {
        if (!cfg_valid_pass(argv[2])) {
            say(c, "ERR: password must be 8-64 characters (or \"\" for an open network)\n");
            return;
        }
        copy_str(c->cfg.pass, sizeof(c->cfg.pass), argv[2]);
        mark_dirty(c);
    } else {
        say(c, "usage: wifi ssid <name> | wifi pass <password>\n");
    }
}

static void cmd_ip(cli_t *c, int argc, char **argv)
{
    if (argc == 1) {
        print_ip(c, &c->cfg);
    } else if (argc == 2 && is(argv[1], "dhcp")) {
        c->cfg.dhcp = true;
        mark_dirty(c);
    } else if ((argc == 5 || argc == 6) && is(argv[1], "static")) {
        uint32_t ip, mask, gw, dns;
        if (!cfg_parse_ipv4(argv[2], &ip)) { say(c, "ERR: bad IP address '%s'\n", argv[2]); return; }
        if (!cfg_parse_netmask(argv[3], &mask)) { say(c, "ERR: bad netmask '%s' (use 255.255.255.0 or /24)\n", argv[3]); return; }
        if (!cfg_parse_ipv4(argv[4], &gw)) { say(c, "ERR: bad gateway '%s' (0.0.0.0 = none)\n", argv[4]); return; }
        if (argc == 6) {
            if (!cfg_parse_ipv4(argv[5], &dns)) { say(c, "ERR: bad DNS '%s'\n", argv[5]); return; }
        } else {
            dns = gw;
        }
        const char *why = cfg_check_static(ip, mask, gw);
        if (why) {
            say(c, "ERR: %s\n", why);
            return;
        }
        c->cfg.dhcp = false;
        c->cfg.ip = ip;
        c->cfg.mask = mask;
        c->cfg.gw = gw;
        c->cfg.dns = dns;
        mark_dirty(c);
    } else {
        say(c, "usage: ip dhcp | ip static <ip> <mask> <gw> [dns]\n");
    }
}

static void do_save(cli_t *c)
{
    if (!c->ops->save(&c->cfg)) {
        say(c, "ERR: could not write flash\n");
        return;
    }
    c->dirty = false;
    say(c, "Saved.\n");
}

void cli_execute(cli_t *c, char *line)
{
    char *argv[8];
    int argc = cfg_tokenize(line, argv, 8);

    if (argc == -1) { say(c, "ERR: unterminated quote\n"); return; }
    if (argc == -2) { say(c, "ERR: too many arguments\n"); return; }
    if (argc == 0) return;

    const char *cmd = argv[0];

    if (is(cmd, "help") || is(cmd, "?")) {
        help(c);
    } else if (is(cmd, "show")) {
        show(c);
    } else if (is(cmd, "status")) {
        c->ops->status(c->io);
    } else if (is(cmd, "scan")) {
        c->ops->scan(c->io);
    } else if (is(cmd, "setup")) {
        wizard(c);
    } else if (is(cmd, "wifi")) {
        cmd_wifi(c, argc, argv);
    } else if (is(cmd, "ip")) {
        cmd_ip(c, argc, argv);
    } else if (is(cmd, "port")) {
        uint16_t p;
        if (argc == 2 && cfg_parse_port(argv[1], &p)) {
            c->cfg.port = p;
            mark_dirty(c);
        } else {
            say(c, "usage: port <1-65535>\n");
        }
    } else if (is(cmd, "hostname")) {
        if (argc == 2 && cfg_valid_hostname(argv[1])) {
            copy_str(c->cfg.hostname, sizeof(c->cfg.hostname), argv[1]);
            mark_dirty(c);
        } else {
            say(c, "usage: hostname <name>   (letters, digits, '-', max 31 chars)\n");
        }
    } else if (is(cmd, "save")) {
        do_save(c);
        if (!c->dirty) say(c, "Reboot ('reboot' or 'apply') to use the new settings.\n");
    } else if (is(cmd, "apply")) {
        do_save(c);
        if (!c->dirty) {
            say(c, "Rebooting...\n");
            c->ops->reboot();
        }
    } else if (is(cmd, "reboot")) {
        bool force = (argc == 2 && is(argv[1], "force"));
        if (c->dirty && !force) {
            say(c, "ERR: unsaved changes. Use 'save' first, or 'reboot force' to discard them.\n");
        } else {
            say(c, "Rebooting...\n");
            c->ops->reboot();
        }
    } else if (is(cmd, "factory-reset")) {
        char buf[32];
        if (ask(c, "Erase ALL settings? Type YES to confirm: ", buf, sizeof(buf), false) == ASK_CANCEL || strcmp(buf, "YES") != 0) {
            say(c, "Cancelled. Nothing changed.\n");
            return;
        }
        if (!c->ops->erase()) {
            say(c, "ERR: could not erase flash\n");
            return;
        }
        cfg_set_defaults(&c->cfg);
        c->dirty = false;
        say(c, "Settings erased. Rebooting...\n");
        c->ops->reboot();
    } else {
        say(c, "ERR: unknown command '%s'. Type 'help'.\n", cmd);
    }
}
