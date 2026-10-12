#pragma once
#include "app_config.h"

/* Starts the setup console on the serial port (CONFIG_AMBE_CLI_UART_NUM, CONFIG_AMBE_CLI_BAUD, 8N1). */
void cli_uart_start(const app_config_t *cfg);
