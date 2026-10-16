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
#endif
#if !CONFIG_AMBE_TRANSPORT_WIFI_TCP || CONFIG_AMBE_USB_MODE_PIN
#include "transport.h"
#endif
#if CONFIG_AMBE_USB_MODE_PIN
#include "driver/gpio.h"
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

#if !CONFIG_AMBE_TRANSPORT_WIFI_TCP || CONFIG_AMBE_USB_MODE_PIN
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

#if CONFIG_AMBE_USB_MODE_PIN
/* Mode pin made (to GND) at boot = USB mode. Logging is switched off before anything else can print, because
 * in USB mode the console COM port carries the binary protocol (gpio_config itself logs at INFO level). */
static bool usb_mode_selected(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_AMBE_USB_MODE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(10));          /* let the pull-up settle */
    bool made = gpio_get_level(CONFIG_AMBE_USB_MODE_GPIO) == 0;
    if (!made) {
        esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
    }
    return made;
}

/* Wi-Fi mode only: report every change of the mode pin on the console, so the right pin can be found by
 * touching the jumper to it (the mode itself only changes at the next boot). */
static void mode_pin_watch_task(void *arg)
{
    (void)arg;
    int last = gpio_get_level(CONFIG_AMBE_USB_MODE_GPIO);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int now = gpio_get_level(CONFIG_AMBE_USB_MODE_GPIO);
        if (now != last) {
            last = now;
            ESP_LOGW(TAG, "mode pin GPIO%d %s", CONFIG_AMBE_USB_MODE_GPIO,
                     now ? "OPEN (Wi-Fi mode at next boot)" : "MADE to GND (USB mode at next boot)");
        }
    }
}

static void usb_mode_run(void)
{
    if (!transport_init()) {
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    dv3000_protocol_init(transport_write);
    /* Same placement as the Wi-Fi server task: core 1, above the USB driver's housekeeping. */
    if (xTaskCreatePinnedToCore(protocol_task, "dv3000_usb", 16384, NULL, 10, NULL, 1) != pdPASS) {
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

void app_main(void)
{
#if CONFIG_AMBE_USB_MODE_PIN
    bool usb_mode = usb_mode_selected();
#endif
    diagnostic_puts("\r\nAPP: app_main entered\r\n");
    if (!ambe_codec_init()) {
        diagnostic_puts("APP: codec init failed\r\n");
        ESP_LOGE(TAG, "codec init failed");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    diagnostic_puts("APP: codec init OK\r\n");

#if CONFIG_AMBE_USB_MODE_PIN
    if (usb_mode) {
        usb_mode_run();                     /* no Wi-Fi, no console: the COM port is the DV3000 link */
        return;
    }
    xTaskCreate(mode_pin_watch_task, "mode_pin", 3072, NULL, 1, NULL);
#endif
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
