set(srcs main.c dv3000_protocol.c noise_reduce.c ambe_codec.c)

# REQUIRES must not depend on CONFIG_ values: ESP-IDF resolves component requirements before
# sdkconfig is loaded, so the Wi-Fi components are always listed (unused ones are dropped at link time).
set(reqs mbelib_neo driver esp_timer nvs_flash esp_wifi esp_netif esp_event lwip esp_system)

if(CONFIG_AMBE_TRANSPORT_WIFI_TCP)
    list(APPEND srcs cfg_parse.c cli_core.c cli_uart.c app_config_nvs.c wifi_net.c tcp_server.c)
elseif(CONFIG_AMBE_TRANSPORT_USB_CDC)
    list(APPEND srcs transport_usb.c)
else()
    list(APPEND srcs transport_uart.c)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES ${reqs}
)
