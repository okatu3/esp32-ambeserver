/* Host unit test for cfg_parse.c + cli_core.c (no ESP-IDF needed).
 *   make -f Makefile.cli test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg_parse.h"
#include "cli_core.h"

static int g_fail, g_n;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- fake platform ---- */
static char out[16384];
static const char *script[32];       /* scripted readline answers; "\x03" = Ctrl-C */
static int script_pos, script_len;
static int saved, erased, rebooted;
static app_config_t last_saved;

static void f_puts(const char *s) { strncat(out, s, sizeof(out) - strlen(out) - 1); }
static int f_readline(const char *prompt, char *buf, size_t n, bool secret)
{
    (void)secret;
    f_puts(prompt);
    if (script_pos >= script_len) return -1;
    const char *a = script[script_pos++];
    if (a[0] == 3) return -1;
    snprintf(buf, n, "%s", a);
    return (int)strlen(buf);
}
static bool f_save(const app_config_t *c) { saved++; last_saved = *c; return true; }
static bool f_erase(void) { erased++; return true; }
static void f_reboot(void) { rebooted++; }
static void f_status(const cli_io_t *io) { io->puts("STATUS\n"); }
static void f_scan(const cli_io_t *io) { io->puts("SCAN\n"); }

static const cli_io_t IO = { f_puts, f_readline };
static const cli_ops_t OPS = { f_save, f_erase, f_reboot, f_status, f_scan };

static cli_t cli;
static void reset_env(void)
{
    app_config_t d;
    cfg_set_defaults(&d);
    cli_init(&cli, &d, &IO, &OPS);
    out[0] = 0; script_pos = script_len = 0; saved = erased = rebooted = 0;
}
static void run(const char *line)
{
    char b[256];
    snprintf(b, sizeof(b), "%s", line);
    cli_execute(&cli, b);
}
#define SCRIPT(...) do { static const char *s_[] = { __VA_ARGS__ }; \
    script_len = (int)(sizeof(s_) / sizeof(s_[0])); script_pos = 0; \
    for (int i_ = 0; i_ < script_len; i_++) script[i_] = s_[i_]; } while (0)

int main(void)
{
    uint32_t v; uint16_t p;

    /* ---- ip / mask / port parsing ---- */
    CHECK(cfg_parse_ipv4("192.168.1.50", &v) && v == 0xC0A80132u);
    CHECK(cfg_parse_ipv4("0.0.0.0", &v) && v == 0);
    CHECK(!cfg_parse_ipv4("256.1.1.1", &v));
    CHECK(!cfg_parse_ipv4("1.2.3", &v));
    CHECK(!cfg_parse_ipv4("1.2.3.4.5", &v));
    CHECK(!cfg_parse_ipv4("01.2.3.4", &v));
    CHECK(!cfg_parse_ipv4(" 1.2.3.4", &v));
    CHECK(!cfg_parse_ipv4("1.2.3.4 ", &v));
    CHECK(!cfg_parse_ipv4("1..3.4", &v));
    CHECK(!cfg_parse_ipv4("", &v));
    CHECK(!cfg_parse_ipv4("a.b.c.d", &v));
    CHECK(!cfg_parse_ipv4("1.2.3.1000", &v));
    CHECK(cfg_parse_netmask("255.255.255.0", &v) && v == 0xFFFFFF00u);
    CHECK(cfg_parse_netmask("/24", &v) && v == 0xFFFFFF00u);
    CHECK(cfg_parse_netmask("/30", &v) && v == 0xFFFFFFFCu);
    CHECK(cfg_parse_netmask("/8", &v) && v == 0xFF000000u);
    CHECK(!cfg_parse_netmask("/31", &v));
    CHECK(!cfg_parse_netmask("/0", &v));
    CHECK(!cfg_parse_netmask("/", &v));
    CHECK(!cfg_parse_netmask("/24x", &v));
    CHECK(!cfg_parse_netmask("255.0.255.0", &v));
    CHECK(!cfg_parse_netmask("255.255.255.255", &v));
    CHECK(!cfg_parse_netmask("0.0.0.0", &v));
    CHECK(cfg_parse_port("2460", &p) && p == 2460);
    CHECK(!cfg_parse_port("0", &p));
    CHECK(!cfg_parse_port("65536", &p));
    CHECK(!cfg_parse_port("-1", &p));
    CHECK(!cfg_parse_port("12ab", &p));
    CHECK(!cfg_parse_port("", &p));
    CHECK(cfg_parse_port("65535", &p) && p == 65535);

    /* ---- static-config sanity ---- */
    uint32_t M = 0xFFFFFF00u, IP = 0xC0A80132u, GW = 0xC0A80101u;
    CHECK(cfg_check_static(IP, M, GW) == NULL);
    CHECK(cfg_check_static(IP, M, 0) == NULL);                      /* no gateway allowed */
    CHECK(cfg_check_static(IP, M, 0xC0A80201u) != NULL);            /* gw other subnet */
    CHECK(cfg_check_static(IP, M, IP) != NULL);                     /* gw == ip */
    CHECK(cfg_check_static(0xC0A80100u, M, GW) != NULL);            /* network address */
    CHECK(cfg_check_static(0xC0A801FFu, M, GW) != NULL);            /* broadcast */
    CHECK(cfg_check_static(0x7F000001u, 0xFF000000u, 0) != NULL);   /* loopback */
    CHECK(cfg_check_static(0xE0000001u, 0xF0000000u, 0) != NULL);   /* multicast */
    CHECK(cfg_check_static(0, M, 0) != NULL);
    CHECK(cfg_check_static(IP, 0xFF00FF00u, GW) != NULL);           /* bad mask */
    CHECK(cfg_check_static(IP, M, 0xC0A80100u) != NULL);            /* gw = network addr */

    /* ---- misc validators ---- */
    CHECK(cfg_valid_hostname("ambe3000") && cfg_valid_hostname("a-b"));
    CHECK(!cfg_valid_hostname("-ab") && !cfg_valid_hostname("ab-") && !cfg_valid_hostname("a b") && !cfg_valid_hostname(""));
    CHECK(!cfg_valid_hostname("this-hostname-is-way-too-long-for-us"));
    CHECK(cfg_valid_pass("") && cfg_valid_pass("12345678") && !cfg_valid_pass("1234567"));
    CHECK(cfg_valid_ssid("x") && !cfg_valid_ssid("") && !cfg_valid_ssid("123456789012345678901234567890123"));

    /* ---- tokenizer ---- */
    {
        char l1[] = "wifi ssid \"My Home  WiFi\"";
        char *a[8];
        CHECK(cfg_tokenize(l1, a, 8) == 3 && !strcmp(a[2], "My Home  WiFi"));
        char l2[] = "wifi pass \"a\\\"b\\\\c\"";
        CHECK(cfg_tokenize(l2, a, 8) == 3 && !strcmp(a[2], "a\"b\\c"));
        char l3[] = "wifi pass \"\"";
        CHECK(cfg_tokenize(l3, a, 8) == 3 && a[2][0] == 0);
        char l4[] = "wifi ssid \"oops";
        CHECK(cfg_tokenize(l4, a, 8) == -1);
        char l5[] = "a b c d e f g h i";
        CHECK(cfg_tokenize(l5, a, 8) == -2);
        char l6[] = "   \t  ";
        CHECK(cfg_tokenize(l6, a, 8) == 0);
        char l7[] = "  ip   dhcp  ";
        CHECK(cfg_tokenize(l7, a, 8) == 2 && !strcmp(a[0], "ip") && !strcmp(a[1], "dhcp"));
    }

    /* ---- commands ---- */
    reset_env();
    run("ip static 192.168.1.50 255.255.255.0 192.168.1.1");
    CHECK(!cli.cfg.dhcp && cli.cfg.ip == 0xC0A80132u && cli.cfg.gw == 0xC0A80101u && cli.cfg.dns == 0xC0A80101u && cli.dirty);

    reset_env();
    run("ip static 10.0.0.5 /8 10.0.0.1 8.8.8.8");
    CHECK(!cli.cfg.dhcp && cli.cfg.ip == 0x0A000005u && cli.cfg.mask == 0xFF000000u && cli.cfg.dns == 0x08080808u);

    reset_env();
    run("ip static 192.168.1.50 255.255.255.0 192.168.2.1");       /* gw outside subnet */
    CHECK(cli.cfg.dhcp && !cli.dirty && strstr(out, "ERR"));

    reset_env();
    run("ip static 192.168.1.256 /24 192.168.1.1");
    CHECK(cli.cfg.dhcp && !cli.dirty && strstr(out, "ERR"));
    run("ip static 192.168.1.50 255.255.255.0");                   /* too few args */
    CHECK(strstr(out, "usage"));
    run("ip dhcp");
    CHECK(cli.cfg.dhcp && cli.dirty);

    reset_env();
    run("wifi ssid \"My Home WiFi\"");
    CHECK(!strcmp(cli.cfg.ssid, "My Home WiFi"));
    run("wifi pass short");
    CHECK(cli.cfg.pass[0] == 0 && strstr(out, "ERR"));
    run("wifi pass secret-pass-123");
    CHECK(!strcmp(cli.cfg.pass, "secret-pass-123"));
    out[0] = 0;
    run("show"); run("wifi");
    CHECK(strstr(out, "My Home WiFi") && !strstr(out, "secret-pass-123") && strstr(out, "********"));
    run("wifi pass \"\"");
    CHECK(cli.cfg.pass[0] == 0);

    reset_env();
    run("port 0");   CHECK(cli.cfg.port == 2460 && !cli.dirty);
    run("port 70000"); CHECK(cli.cfg.port == 2460 && !cli.dirty);
    run("port abc"); CHECK(cli.cfg.port == 2460 && !cli.dirty);
    run("port 2461"); CHECK(cli.cfg.port == 2461 && cli.dirty);
    run("hostname bad_name"); CHECK(!strcmp(cli.cfg.hostname, "ambe3000"));
    run("hostname my-ambe"); CHECK(!strcmp(cli.cfg.hostname, "my-ambe"));

    /* save / reboot / apply */
    run("reboot");
    CHECK(rebooted == 0 && strstr(out, "unsaved"));
    run("reboot force");
    CHECK(rebooted == 1);
    run("save");
    CHECK(saved == 1 && !cli.dirty && last_saved.port == 2461);
    rebooted = 0;
    run("reboot");
    CHECK(rebooted == 1);
    rebooted = 0; saved = 0;
    run("port 2470"); run("apply");
    CHECK(saved == 1 && rebooted == 1 && last_saved.port == 2470);

    /* misc commands */
    out[0] = 0;
    run("status"); run("scan"); run("help"); run("nonsense"); run("   "); run("wifi ssid \"unterminated");
    CHECK(strstr(out, "STATUS") && strstr(out, "SCAN") && strstr(out, "Commands:") && strstr(out, "unknown command") && strstr(out, "unterminated"));
    run("SHOW");    /* case-insensitive */

    /* factory reset */
    reset_env();
    run("wifi ssid Foo");
    SCRIPT("yes");
    run("factory-reset");
    CHECK(erased == 0 && rebooted == 0 && !strcmp(cli.cfg.ssid, "Foo"));
    SCRIPT("YES");
    run("factory-reset");
    CHECK(erased == 1 && rebooted == 1 && cli.cfg.ssid[0] == 0 && !cli.dirty);

    /* ---- wizard: DHCP ---- */
    reset_env();
    SCRIPT("HomeNet", "pa55word!", "y", "", "y");
    run("setup");
    CHECK(saved == 1 && rebooted == 1 && !strcmp(last_saved.ssid, "HomeNet") && !strcmp(last_saved.pass, "pa55word!") && last_saved.dhcp && last_saved.port == 2460);
    CHECK(!strstr(out, "pa55word!"));         /* password must not be echoed in the summary */

    /* ---- wizard: static with a bad entry first, then valid ---- */
    reset_env();
    SCRIPT("Net2", "", "n",
           "192.168.1.50", "/24", "192.168.2.1", "",          /* gw outside subnet -> asked again */
           "192.168.1.60", "255.255.255.0", "192.168.1.1", "8.8.4.4",
           "2461", "y");
    run("setup");
    CHECK(saved == 1 && rebooted == 1 && !last_saved.dhcp && last_saved.ip == 0xC0A8013Cu && last_saved.dns == 0x08080404u
          && last_saved.port == 2461 && strstr(out, "same subnet"));

    /* ---- wizard: invalid entries are re-asked ---- */
    reset_env();
    SCRIPT("", "short", "-", "maybe", "n", "999.1.1.1", "192.168.1.70", "bogus", "255.255.255.0", "192.168.1.1", "", "70000", "2460", "y");
    run("setup");
    CHECK(saved == 1 && last_saved.ip == 0xC0A80146u && last_saved.pass[0] == 0 && !last_saved.dhcp);

    /* ---- wizard: decline / Ctrl-C leave everything untouched ---- */
    reset_env();
    SCRIPT("X", "", "y", "", "n");
    run("setup");
    CHECK(saved == 0 && rebooted == 0 && cli.cfg.ssid[0] == 0);
    reset_env();
    SCRIPT("X", "\x03");
    run("setup");
    CHECK(saved == 0 && rebooted == 0 && cli.cfg.ssid[0] == 0 && strstr(out, "Cancelled"));

    /* ---- fuzz: random command lines must not crash (run with ASAN/UBSAN) ---- */
    srand(7);
    static const char *words[] = { "ip", "static", "dhcp", "wifi", "ssid", "pass", "port", "hostname", "save", "apply",
        "reboot", "force", "setup", "show", "scan", "status", "factory-reset", "\"", "\\", "192.168.1.1", "/24", "255.255.255.0",
        "0.0.0.0", "999", "-", "", "\"a b\"", "help", "\t" };
    for (int it = 0; it < 200000; it++) {
        char b[256]; b[0] = 0;
        int nw = rand() % 8;
        for (int i = 0; i < nw; i++) {
            strncat(b, words[rand() % (sizeof(words) / sizeof(words[0]))], sizeof(b) - strlen(b) - 1);
            strncat(b, rand() % 4 ? " " : "", sizeof(b) - strlen(b) - 1);
        }
        if (strstr(b, "setup") || strstr(b, "factory")) continue;     /* interactive */
        out[0] = 0;
        cli_execute(&cli, b);
    }
    CHECK(cfg_validate(&cli.cfg) || 1);   /* reached without a crash */

    /* config validation of stored blobs */
    app_config_t d; cfg_set_defaults(&d);
    CHECK(cfg_validate(&d));
    d.port = 0; CHECK(!cfg_validate(&d));
    cfg_set_defaults(&d); d.dhcp = false; d.gw = 0xC0A80201u; CHECK(!cfg_validate(&d));
    cfg_set_defaults(&d); memset(d.ssid, 'x', sizeof(d.ssid)); CHECK(!cfg_validate(&d));

    printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
