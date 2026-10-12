/* Wi-Fi station + static IP / DHCP (ESP-IDF 5.x). */
#include "wifi_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

static const char *TAG = "wifi";

static esp_netif_t *s_netif;
static bool s_have_ssid;
static bool s_dhcp;
static char s_ssid[CFG_SSID_MAX + 1];
static volatile bool s_link;
static volatile bool s_got_ip;
static volatile int  s_last_reason;
static volatile uint32_t s_retries;
static volatile bool s_scanning;     /* reconnect paused: the driver refuses to scan while connecting */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_have_ssid) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_link = true;
            ESP_LOGI(TAG, "associated");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *d = data;
            s_link = false;
            s_got_ip = false;
            if (s_scanning) {
                break;                        /* our own disconnect before a scan: no retry */
            }
            s_last_reason = d ? d->reason : 0;
            s_retries++;
            if (s_retries <= 3 || (s_retries % 20) == 0) {
                ESP_LOGW(TAG, "disconnected (reason %d), retrying (#%u)", s_last_reason, (unsigned)s_retries);
            }
            if (s_have_ssid) {
                esp_wifi_connect();
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_got_ip = true;
        s_retries = 0;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

bool wifi_net_start(const app_config_t *cfg)
{
    s_have_ssid = cfg->ssid[0] != '\0';
    s_dhcp = cfg->dhcp;
    snprintf(s_ssid, sizeof(s_ssid), "%.*s", CFG_SSID_MAX, cfg->ssid);

    if (esp_netif_init() != ESP_OK) {
        return false;
    }
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return false;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        return false;
    }
    esp_netif_set_hostname(s_netif, cfg->hostname);

    if (!cfg->dhcp) {
        e = esp_netif_dhcpc_stop(s_netif);
        if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGE(TAG, "dhcpc_stop: %s", esp_err_to_name(e));
            return false;
        }
        esp_netif_ip_info_t info;
        memset(&info, 0, sizeof(info));
        info.ip.addr = htonl(cfg->ip);
        info.netmask.addr = htonl(cfg->mask);
        info.gw.addr = htonl(cfg->gw);
        e = esp_netif_set_ip_info(s_netif, &info);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "set_ip_info: %s", esp_err_to_name(e));
            return false;
        }
        if (cfg->dns) {
            esp_netif_dns_info_t dns;
            memset(&dns, 0, sizeof(dns));
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = htonl(cfg->dns);
            esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
        }
        ESP_LOGI(TAG, "static IP %u.%u.%u.%u", (unsigned)(cfg->ip >> 24), (unsigned)((cfg->ip >> 16) & 255),
                 (unsigned)((cfg->ip >> 8) & 255), (unsigned)(cfg->ip & 255));
    }

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wic) != ESP_OK) {
        return false;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    if (s_have_ssid) {
        wifi_config_t wc;
        memset(&wc, 0, sizeof(wc));
        memcpy(wc.sta.ssid, cfg->ssid, strlen(cfg->ssid));          /* <= 32, no NUL needed */
        memcpy(wc.sta.password, cfg->pass, strlen(cfg->pass));      /* <= 64 */
        wc.sta.threshold.authmode = cfg->pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        wc.sta.pmf_cfg.capable = true;
        wc.sta.pmf_cfg.required = false;
        if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
            return false;
        }
    }
    if (esp_wifi_start() != ESP_OK) {
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);          /* modem sleep adds tens of ms of jitter: bad for 20 ms voice frames */
    return true;
}

void wifi_net_print_status(net_puts_fn puts)
{
    char b[160];

    if (!s_netif) {
        puts("Wi-Fi: not started\n");
        return;
    }
    if (!s_have_ssid) {
        puts("Wi-Fi: no SSID configured (run 'setup')\n");
    } else if (s_got_ip || (s_link && !s_dhcp)) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(b, sizeof(b), "Wi-Fi: connected to \"%s\"  ch %u  RSSI %d dBm\n", (const char *)ap.ssid, ap.primary, ap.rssi);
        } else {
            snprintf(b, sizeof(b), "Wi-Fi: connected to \"%s\"\n", s_ssid);
        }
        puts(b);
    } else if (s_link) {
        puts("Wi-Fi: associated, waiting for an IP address (DHCP)...\n");
    } else {
        snprintf(b, sizeof(b), "Wi-Fi: NOT connected to \"%s\" (last reason %d, %u retries)\n", s_ssid, s_last_reason, (unsigned)s_retries);
        puts(b);
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(b, sizeof(b), "IP    : " IPSTR "  mask " IPSTR "  gw " IPSTR "  (%s)\n", IP2STR(&ip.ip), IP2STR(&ip.netmask),
                 IP2STR(&ip.gw), s_dhcp ? "DHCP" : "static");
        puts(b);
    }
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        snprintf(b, sizeof(b), "DNS   : " IPSTR "\n", IP2STR(&dns.ip.u_addr.ip4));
        puts(b);
    }
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(b, sizeof(b), "MAC   : %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        puts(b);
    }
}

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    default:                        return "other";
    }
}

void wifi_net_scan_print(net_puts_fn puts)
{
    if (!s_netif) {
        puts("Wi-Fi: not started\n");
        return;
    }
    puts("Scanning (a few seconds; the link may drop briefly)...\n");

    /* While not associated the station keeps retrying, and the driver rejects a scan during a connect
     * attempt (ESP_ERR_WIFI_STATE). Pause the retries for the scan, then resume them. */
    bool paused = !s_link;
    if (paused) {
        s_scanning = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e == ESP_ERR_WIFI_STATE) {
        vTaskDelay(pdMS_TO_TICKS(500));     /* a connect attempt was still finishing */
        e = esp_wifi_scan_start(&sc, true);
    }

    static wifi_ap_record_t recs[20];
    uint16_t n = 0;
    if (e == ESP_OK) {
        esp_wifi_scan_get_ap_num(&n);
        if (n > 20) {
            n = 20;
        }
        esp_wifi_scan_get_ap_records(&n, recs);   /* before reconnecting: its own scan would replace the list */
    }
    if (paused) {
        s_scanning = false;
        if (s_have_ssid) {
            esp_wifi_connect();
        }
    }

    char b[96];
    if (e != ESP_OK) {
        snprintf(b, sizeof(b), "ERR: scan failed (%s)\n", esp_err_to_name(e));
        puts(b);
        return;
    }
    snprintf(b, sizeof(b), "%u access point(s):\n", (unsigned)n);
    puts(b);
    for (uint16_t i = 0; i < n; i++) {
        snprintf(b, sizeof(b), "  %2u  ch%-2u  %4d dBm  %-6s  %s\n", (unsigned)(i + 1), recs[i].primary, recs[i].rssi,
                 auth_name(recs[i].authmode), (const char *)recs[i].ssid);
        puts(b);
    }
}
