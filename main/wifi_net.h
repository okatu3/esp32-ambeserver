#pragma once
#include <stdbool.h>
#include "app_config.h"

typedef void (*net_puts_fn)(const char *text);

/* Brings up Wi-Fi station mode with the given settings (static IP or DHCP). NVS must be initialised. */
bool wifi_net_start(const app_config_t *cfg);
void wifi_net_print_status(net_puts_fn puts);
void wifi_net_scan_print(net_puts_fn puts);
