#pragma once
/* ES8388 audio codec of the ESP32-A1S audio kit (I2C address 0x10), slave to the ESP32's I2S at 48 kHz, 16 bit.
 * Pins of the ES8388 version of the kit: I2C SDA 33 / SCL 32; I2S MCLK 0, BCLK 27, LRCK 25, DOUT 26 (to the
 * codec), DIN 35 (from the codec); speaker amplifier enable 21. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define A1S_I2C_SDA   33
#define A1S_I2C_SCL   32
#define A1S_I2S_MCLK  0
#define A1S_I2S_BCLK  27
#define A1S_I2S_LRCK  25
#define A1S_I2S_DOUT  26
#define A1S_I2S_DIN   35
#define A1S_PA_EN     21

#define ES8388_ADDR   0x10
#define AC101_ADDR    0x1A

/* ADCCONTROL2: LIN1/RIN1 = board mics, LIN2/RIN2 = line-in jack; SPLIT = left ADC from LIN2 (line), right ADC
 * from RIN1 (mic) */
typedef enum { ES_IN_MIC = 0x00, ES_IN_LINE = 0x50, ES_IN_SPLIT = 0x40 } es_input_t;

esp_err_t es_i2c_init(void);
int es_i2c_scan(uint8_t *found, int max);            /* addresses that answer */
esp_err_t es8388_init(es_input_t in, int mic_gain_db); /* gain 0..24 dB in 3 dB steps */
esp_err_t es8388_set_input(es_input_t in, int gain_db);
esp_err_t es8388_set_out_volume(int vol);            /* 0..33, 30 = 0 dB */
esp_err_t es8388_read(uint8_t reg, uint8_t *val);
esp_err_t es8388_set_gains(int left_db, int right_db);     /* input PGA, 0..24 dB in 3 dB steps */
esp_err_t es8388_set_out(int lout1, int rout1, int lout2, int rout2);   /* 0..33 each, 30 = 0 dB, 0 = off */
