#pragma once
/* Logging that never touches the protocol stream: ESP_LOG goes to the console UART
 * (or nowhere when the console is disabled); the host build prints to stderr. */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#define DV_LOGI(...) ESP_LOGI("dv3000", __VA_ARGS__)
#define DV_LOGW(...) ESP_LOGW("dv3000", __VA_ARGS__)
#define DV_LOGE(...) ESP_LOGE("dv3000", __VA_ARGS__)
#else
#include <stdio.h>
#define DV_LOGI(...) do { fputs("[I] ", stderr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DV_LOGW(...) do { fputs("[W] ", stderr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DV_LOGE(...) do { fputs("[E] ", stderr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif
