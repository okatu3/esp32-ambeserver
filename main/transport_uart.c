/* UART transport: works on every ESP32 (classic ESP32 via the board's USB-UART bridge). */
#include "transport.h"

#include "driver/uart.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_UART && (CONFIG_ESP_CONSOLE_UART_NUM == CONFIG_AMBE_UART_NUM)
#error "The console shares the protocol UART and would corrupt the binary stream. \
Set CONFIG_ESP_CONSOLE_NONE=y (see sdkconfig.defaults.esp32), or use another UART via menuconfig."
#endif

#define PORT ((uart_port_t)CONFIG_AMBE_UART_NUM)

bool transport_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_AMBE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(PORT, 4096, 2048, 0, NULL, 0) != ESP_OK) return false;
    if (uart_param_config(PORT, &cfg) != ESP_OK) return false;

    int tx = CONFIG_AMBE_UART_TX_PIN < 0 ? UART_PIN_NO_CHANGE : CONFIG_AMBE_UART_TX_PIN;
    int rx = CONFIG_AMBE_UART_RX_PIN < 0 ? UART_PIN_NO_CHANGE : CONFIG_AMBE_UART_RX_PIN;
    return uart_set_pin(PORT, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK;
}

int transport_read(uint8_t *buf, size_t max, TickType_t timeout)
{
    /* uart_read_bytes() waits for the full length, so wait for 1 byte and then drain what is buffered. */
    int n = uart_read_bytes(PORT, buf, 1, timeout);
    if (n <= 0) {
        return 0;
    }
    size_t avail = 0;
    uart_get_buffered_data_len(PORT, &avail);
    if (avail > max - 1) {
        avail = max - 1;
    }
    if (avail) {
        int m = uart_read_bytes(PORT, buf + 1, avail, 0);
        if (m > 0) {
            n += m;
        }
    }
    return n;
}

bool transport_write(const uint8_t *data, size_t len)
{
    return uart_write_bytes(PORT, data, len) == (int)len;
}
