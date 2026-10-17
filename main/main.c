/* ESP32 (classic) DV3000-compatible AMBE vocoder over USB: the board's USB-UART bridge carries the DV3000
 * serial protocol and the codec is the published mbelib-neo with no pre-processing. Reference build to compare
 * against the Wi-Fi lines (Desktop\ambe_wifi / Desktop\ambe_wifi_s3). */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ambe_codec.h"
#include "dv3000_protocol.h"
#include "transport.h"

static void protocol_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];

    for (;;) {
        int n = transport_read(buf, sizeof(buf), portMAX_DELAY);
        if (n > 0) {
            dv3000_protocol_feed(buf, (size_t)n);
        }
    }
}

void app_main(void)
{
    /* No console: on failure there is nobody to tell, so just stop (the host never gets READY). */
    if (!ambe_codec_init() || !transport_init()) {
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    dv3000_protocol_init(transport_write);

    /* Core 1, above the system housekeeping tasks. */
    xTaskCreatePinnedToCore(protocol_task, "dv3000", 16384, NULL, 10, NULL, 1);
}
