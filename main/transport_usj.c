/* DV3000 over the built-in USB Serial/JTAG port (single-connector ESP32-S3/C3 boards): the PC sees the same
 * COM port as the setup console, so this is only used in USB mode (mode pin made at boot), where the console,
 * Wi-Fi and every log line are off and the port carries nothing but the binary protocol. */
#include "transport.h"

#include "driver/usb_serial_jtag.h"

bool transport_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 4096;      /* holds >12 AUDIO packets (BlueDV sends 5 per 100 ms burst) */
    cfg.tx_buffer_size = 4096;
    return usb_serial_jtag_driver_install(&cfg) == ESP_OK;
}

int transport_read(uint8_t *buf, size_t max, TickType_t timeout)
{
    int n = usb_serial_jtag_read_bytes(buf, (uint32_t)max, timeout);
    return n > 0 ? n : 0;
}

bool transport_write(const uint8_t *data, size_t len)
{
    /* Times out instead of blocking when no program has the COM port open (the host does not drain). */
    size_t off = 0;
    while (off < len) {
        int n = usb_serial_jtag_write_bytes(data + off, len - off, pdMS_TO_TICKS(100));
        if (n <= 0) {
            return false;
        }
        off += (size_t)n;
    }
    return true;
}
