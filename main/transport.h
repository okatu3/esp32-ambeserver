#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

bool transport_init(void);

/* Blocks until at least 1 byte is available (or timeout); returns the number of bytes read, 0 on timeout. */
int  transport_read(uint8_t *buf, size_t max, TickType_t timeout);

/* Sends all bytes (blocking). Returns false if the link is stalled. */
bool transport_write(const uint8_t *data, size_t len);
