/* Setup console on the serial port the PC sees as a COM port: a UART (USB-UART bridge) or, on chips that
 * have one (ESP32-S3/C3/...), the built-in USB Serial/JTAG port (boards with a single USB connector). */
#include "cli_uart.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_AMBE_CLI_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#define CLI_UART ((uart_port_t)CONFIG_AMBE_CLI_UART_NUM)
#endif

#include "app_config.h"
#include "cli_core.h"
#include "dv3000_protocol.h"
#include "noise_reduce.h"
#include "tcp_server.h"
#include "wifi_net.h"

static cli_t s_cli;
static uint16_t s_run_port;      /* port the server is actually bound to (unsaved edits don't change it) */

/* ---------------------------------------------------------------- port backend */
#if CONFIG_AMBE_CLI_USB_SERIAL_JTAG
static void io_raw(const char *s, size_t n)
{
    /* Short timeout: with no terminal open the host does not drain the FIFO and a blocking write would
     * stall the console task. */
    usb_serial_jtag_write_bytes(s, n, pdMS_TO_TICKS(50));
}

static bool io_getc(uint8_t *ch)
{
    return usb_serial_jtag_read_bytes(ch, 1, portMAX_DELAY) == 1;
}

static void io_flush(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));      /* let the host fetch the last bytes before a reboot */
}

static bool io_port_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    cfg.tx_buffer_size = 2048;
    return usb_serial_jtag_driver_install(&cfg) == ESP_OK;
}
#else
static void io_raw(const char *s, size_t n)
{
    uart_write_bytes(CLI_UART, s, n);
}

static bool io_getc(uint8_t *ch)
{
    return uart_read_bytes(CLI_UART, ch, 1, portMAX_DELAY) == 1;
}

static void io_flush(void)
{
    uart_wait_tx_done(CLI_UART, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(100));
}

static bool io_port_init(void)
{
    const uart_config_t uc = {
        .baud_rate = CONFIG_AMBE_CLI_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(CLI_UART, 1024, 2048, 0, NULL, 0) != ESP_OK ||
        uart_param_config(CLI_UART, &uc) != ESP_OK) {
        return false;
    }
    uart_set_pin(CLI_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    return true;
}
#endif

/* ---------------------------------------------------------------- terminal I/O */

static void io_puts(const char *s)
{
    char tmp[128];
    size_t k = 0;
    for (; *s; ++s) {
        if (k > sizeof(tmp) - 3) {
            io_raw(tmp, k);
            k = 0;
        }
        if (*s == '\n') {
            tmp[k++] = '\r';
        }
        tmp[k++] = *s;
    }
    if (k) {
        io_raw(tmp, k);
    }
}

static int io_readline(const char *prompt, char *buf, size_t n, bool secret)
{
    size_t len = 0;
    int esc = 0;
    bool last_cr = false;

    io_puts(prompt);
    for (;;) {
        uint8_t ch;
        if (!io_getc(&ch)) {
            continue;
        }
        if (esc == 1) {                       /* ESC [ ... : swallow cursor keys etc. */
            esc = (ch == '[' || ch == 'O') ? 2 : 0;
            continue;
        }
        if (esc == 2) {
            if (ch >= 0x40 && ch <= 0x7E) {
                esc = 0;
            }
            continue;
        }
        if (ch == 0x1B) {
            esc = 1;
            continue;
        }
        if (ch == '\n' && last_cr) {          /* CRLF from the terminal: one Enter */
            last_cr = false;
            continue;
        }
        last_cr = (ch == '\r');

        if (ch == '\r' || ch == '\n') {
            io_puts("\n");
            buf[len] = '\0';
            return (int)len;
        }
        if (ch == 0x03) {                     /* Ctrl-C */
            io_puts("^C\n");
            buf[0] = '\0';
            return -1;
        }
        if (ch == 0x08 || ch == 0x7F) {       /* backspace / DEL */
            if (len) {
                len--;
                io_raw("\b \b", 3);
            }
            continue;
        }
        if (ch == 0x15) {                     /* Ctrl-U */
            while (len) {
                len--;
                io_raw("\b \b", 3);
            }
            continue;
        }
        if (ch >= 0x20 && ch < 0x7F && len < n - 1) {
            buf[len++] = (char)ch;
            char e = secret ? '*' : (char)ch;
            io_raw(&e, 1);
        }
    }
}

static const cli_io_t IO = { io_puts, io_readline };

/* ---------------------------------------------------------------- platform actions */
static bool op_save(const app_config_t *c) { return app_config_save(c); }
static bool op_erase(void)                 { return app_config_erase(); }

static void op_reboot(void)
{
    io_flush();
    esp_restart();
}

static void op_status(const cli_io_t *io)
{
    wifi_net_print_status(io->puts);

    tcp_stats_t st;
    tcp_server_get_stats(&st);
    char b[160];
    snprintf(b, sizeof(b), "Server: TCP+UDP port %u  TCP %s  accepted %u  rx %u bytes  tx %u bytes  tx errors %u\n", (unsigned)s_run_port,
             st.connected ? "CLIENT CONNECTED" : "no client", (unsigned)st.accepted, (unsigned)st.rx_bytes,
             (unsigned)st.tx_bytes, (unsigned)st.tx_errors);
    io->puts(b);
    if (st.client_ip) {
        snprintf(b, sizeof(b), "Client: %u.%u.%u.%u:%u%s\n", (unsigned)(st.client_ip >> 24), (unsigned)((st.client_ip >> 16) & 255),
                 (unsigned)((st.client_ip >> 8) & 255), (unsigned)(st.client_ip & 255), (unsigned)st.client_port,
                 st.connected ? "" : " (last)");
        io->puts(b);
    }
    {
        /* automatic equaliser: correction per band (dB), low -> high (150 Hz .. 3.8 kHz) */
        float eq[15];
        nr_eq_info(eq);
        int k = snprintf(b, sizeof(b), "Auto EQ (dB, 150Hz..3.8kHz):");
        for (int i = 0; i < 15 && k < (int)sizeof(b) - 6; ++i) {
            k += snprintf(b + k, sizeof(b) - (size_t)k, " %+.0f", (double)eq[i]);
        }
        snprintf(b + k, sizeof(b) - (size_t)k, "\n");
        io->puts(b);
    }
    {
        uint32_t fr, avg, mx, nr, en;
        dv3000_get_proc_stats(&fr, &avg, &mx, &nr, &en);
        snprintf(b, sizeof(b), "Encode time (since last status, %u frames): avg %u us (NR %u, encoder %u), max %u us  [budget 20000]\n",
                 (unsigned)fr, (unsigned)avg, (unsigned)nr, (unsigned)en, (unsigned)mx);
        io->puts(b);
    }
    snprintf(b, sizeof(b), "Free heap: %u bytes\n", (unsigned)esp_get_free_heap_size());
    io->puts(b);
}

static void op_scan(const cli_io_t *io) { wifi_net_scan_print(io->puts); }

static const cli_ops_t OPS = { op_save, op_erase, op_reboot, op_status, op_scan };

/* ---------------------------------------------------------------- task */
static void cli_task(void *arg)
{
    (void)arg;
    static char line[160];

    io_puts("\n\n=== ESP32 AMBE3000 (DV3000) Wi-Fi server ===\n");
    io_puts("Type 'help' for commands, 'setup' for the step-by-step wizard.\n");
    if (!s_cli.cfg.ssid[0]) {
        io_puts("Wi-Fi is not configured yet: run 'setup'.\n");
    }
    io_puts("\n");

    for (;;) {
        int n = io_readline("ambe> ", line, sizeof(line), false);
        if (n > 0) {
            cli_execute(&s_cli, line);
        }
    }
}

void cli_uart_start(const app_config_t *cfg)
{
    if (!io_port_init()) {
        ESP_LOGE("cli", "console port driver setup failed");
        return;
    }

    s_run_port = cfg->port;
    cli_init(&s_cli, cfg, &IO, &OPS);
    if (xTaskCreate(cli_task, "cli", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE("cli", "could not create the CLI task");
    }
}
