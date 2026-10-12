/* Native USB CDC-ACM transport (ESP32-S2/S3), esp_tinyusb 2.x API. */
#include "transport.h"

#include "esp_log.h"
#include "freertos/stream_buffer.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

static const char *TAG = "usb";

#define RX_STREAM_BYTES 4096        /* holds >12 AUDIO packets: absorbs AMBETools' 4-frame pipelining */

static StreamBufferHandle_t s_rx;
static uint8_t s_cb_buf[512];
static volatile uint32_t s_dropped;

/* Runs in the TinyUSB task: only move bytes, never run the codec here. */
static void rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    size_t n = 0;
    if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, s_cb_buf, sizeof(s_cb_buf), &n) != ESP_OK || n == 0) {
        return;
    }
    size_t sent = xStreamBufferSend(s_rx, s_cb_buf, n, 0);
    if (sent < n) {
        s_dropped += (uint32_t)(n - sent);
    }
}

bool transport_init(void)
{
    s_rx = xStreamBufferCreate(RX_STREAM_BYTES, 1);
    if (!s_rx) {
        return false;
    }

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    if (tinyusb_driver_install(&tusb_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed");
        return false;
    }

    const tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = rx_cb,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    if (tinyusb_cdcacm_init(&cdc_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_cdcacm_init failed");
        return false;
    }
    return true;
}

int transport_read(uint8_t *buf, size_t max, TickType_t timeout)
{
    static uint32_t reported;
    int n = (int)xStreamBufferReceive(s_rx, buf, max, timeout);
    if (s_dropped != reported) {
        reported = s_dropped;
        ESP_LOGW(TAG, "RX overflow, %u bytes dropped so far", (unsigned)reported);
    }
    return n;
}

bool transport_write(const uint8_t *data, size_t len)
{
    /* tinyusb_cdcacm_write_queue() returns the number of bytes queued (size_t), possibly < len. */
    size_t off = 0;
    int stalls = 0;

    while (off < len) {
        size_t n = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data + off, len - off);
        off += n;
        if (n == 0) {
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(20));
            if (++stalls > 50) {
                return false;       /* no host reading for ~1 s */
            }
        } else {
            stalls = 0;
        }
    }
    /* Called from the protocol task, not from a CDC callback, so a timeout is allowed. */
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
    return true;
}
