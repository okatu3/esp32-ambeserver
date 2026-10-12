/* NVS persistence of app_config_t (ESP-IDF only). */
#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "cfg_parse.h"

static const char *TAG = "config";

#define NS          "ambe"
#define KEY         "cfg"
#define CFG_VERSION 1u

typedef struct {
    uint32_t     version;
    app_config_t cfg;
} stored_t;

bool app_config_nvs_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing (%s)", esp_err_to_name(e));
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    return e == ESP_OK;
}

bool app_config_load(app_config_t *c)
{
    cfg_set_defaults(c);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;                                   /* first boot: nothing stored */
    }
    stored_t s;
    size_t len = sizeof(s);
    esp_err_t e = nvs_get_blob(h, KEY, &s, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(s) || s.version != CFG_VERSION) {
        return false;
    }
    if (!cfg_validate(&s.cfg)) {
        ESP_LOGW(TAG, "stored settings are invalid, using defaults");
        return false;
    }
    *c = s.cfg;
    return true;
}

bool app_config_save(const app_config_t *c)
{
    stored_t s;
    memset(&s, 0, sizeof(s));
    s.version = CFG_VERSION;
    s.cfg = *c;

    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(e));
        return false;
    }
    e = nvs_set_blob(h, KEY, &s, sizeof(s));
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(e));
    }
    return e == ESP_OK;
}

bool app_config_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e = nvs_erase_all(h);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e == ESP_OK;
}
