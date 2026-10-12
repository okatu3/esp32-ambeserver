#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "ambe_codec.h"
#include "dv3000_protocol.h"

#if CONFIG_AMBE_TRANSPORT_WIFI_TCP
#include "app_config.h"
#include "cli_uart.h"
#include "tcp_server.h"
#include "wifi_net.h"
#else
#include "transport.h"
#endif

static const char *TAG = "main";

/* Boot diagnostics written straight to the ROM console (UART0), bypassing the log system.
 * OFF by default: on the UART transport they would corrupt the binary stream. */
#if CONFIG_AMBE_BOOT_DIAG
#include "esp_idf_version.h"
#include "esp_rom_uart.h"
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
#define esp_rom_output_putc esp_rom_uart_putc     /* renamed in IDF 5.2 */
#endif
static void diagnostic_puts(const char *message)
{
    while (*message) {
        esp_rom_output_putc(*message++);
    }
}
#else
#define diagnostic_puts(s) ((void)0)
#endif

#if !CONFIG_AMBE_TRANSPORT_WIFI_TCP
static void protocol_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];

    for (;;) {
        int n = transport_read(buf, sizeof(buf), portMAX_DELAY);
        if (n > 0) {
            dv3000_protocol_feed(buf, (size_t)n);
        }
    }
}
#endif

void app_main(void)
{
    diagnostic_puts("\r\nAPP: app_main entered\r\n");
    if (!ambe_codec_init()) {
        diagnostic_puts("APP: codec init failed\r\n");
        ESP_LOGE(TAG, "codec init failed");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    diagnostic_puts("APP: codec init OK\r\n");

#if CONFIG_AMBE_TRANSPORT_WIFI_TCP
    /* Wi-Fi server: settings from flash, setup console on the serial port, DV3000 over TCP. */
    if (!app_config_nvs_init()) {
        ESP_LOGE(TAG, "NVS init failed (settings cannot be saved)");
    }
    app_config_t cfg;
    bool stored = app_config_load(&cfg);
    ESP_LOGI(TAG, "settings %s", stored ? "loaded from flash" : "not found, using defaults");

    cli_uart_start(&cfg);               /* first: the console must work even if Wi-Fi fails */
    if (!wifi_net_start(&cfg)) {
        ESP_LOGE(TAG, "Wi-Fi start failed: check the settings with 'show'");
    }
    tcp_server_start(cfg.port);
    ESP_LOGI(TAG, "ready (TCP port %u)", (unsigned)cfg.port);
#else
    diagnostic_puts("APP: transport init starting\r\n");
    if (!transport_init()) {
        diagnostic_puts("APP: transport init failed\r\n");
        ESP_LOGE(TAG, "transport init failed");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    dv3000_protocol_init(transport_write);
    ESP_LOGI(TAG, "ready");

    if (xTaskCreate(protocol_task, "dv3000", 16384, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "protocol task creation failed");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}
