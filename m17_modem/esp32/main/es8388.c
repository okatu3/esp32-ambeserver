#include "es8388.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8388";
#define PORT I2C_NUM_0

esp_err_t es_i2c_init(void)
{
    i2c_config_t c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = A1S_I2C_SDA,
        .scl_io_num = A1S_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t e = i2c_param_config(PORT, &c);
    return e ? e : i2c_driver_install(PORT, I2C_MODE_MASTER, 0, 0, 0);
}

int es_i2c_scan(uint8_t *found, int max)
{
    int n = 0;
    for (uint8_t a = 0x08; a < 0x78 && n < max; ++a) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)(a << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        if (i2c_master_cmd_begin(PORT, cmd, pdMS_TO_TICKS(20)) == ESP_OK) {
            found[n++] = a;
        }
        i2c_cmd_link_delete(cmd);
    }
    return n;
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    esp_err_t e = i2c_master_write_to_device(PORT, ES8388_ADDR, b, 2, pdMS_TO_TICKS(50));
    if (e) {
        ESP_LOGE(TAG, "write %02x=%02x failed: %s", reg, val, esp_err_to_name(e));
    }
    return e;
}

esp_err_t es8388_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(PORT, ES8388_ADDR, &reg, 1, val, 1, pdMS_TO_TICKS(50));
}

esp_err_t es8388_set_input(es_input_t in, int gain_db)
{
    int g = gain_db / 3;
    g = g < 0 ? 0 : (g > 8 ? 8 : g);
    esp_err_t e = wr(0x09, (uint8_t)(g << 4 | g));   /* ADCCONTROL1: left/right PGA gain */
    return e ? e : wr(0x0A, (uint8_t)in);           /* ADCCONTROL2: input select */
}

static int clamp_g(int db)
{
    int g = db / 3;
    return g < 0 ? 0 : (g > 8 ? 8 : g);
}

esp_err_t es8388_set_gains(int left_db, int right_db)
{
    return wr(0x09, (uint8_t)(clamp_g(left_db) << 4 | clamp_g(right_db)));
}

esp_err_t es8388_set_out(int lout1, int rout1, int lout2, int rout2)
{
    int v[4] = { lout1, rout1, lout2, rout2 };
    esp_err_t e = 0;
    for (int i = 0; i < 4; ++i) {
        int x = v[i] < 0 ? 0 : (v[i] > 33 ? 33 : v[i]);
        e |= wr((uint8_t)(0x2E + i), (uint8_t)x);
    }
    return e;
}

esp_err_t es8388_set_out_volume(int vol)
{
    vol = vol < 0 ? 0 : (vol > 33 ? 33 : vol);
    esp_err_t e = 0;
    for (uint8_t r = 0x2E; r <= 0x31; ++r) {         /* LOUT1, ROUT1, LOUT2, ROUT2 */
        e |= wr(r, (uint8_t)vol);
    }
    return e;
}

/* Slave mode, 16-bit I2S both ways, LRCK = MCLK / 256 (12.288 MHz -> 48 kHz); the sequence of ESP-ADF's driver. */
esp_err_t es8388_init(es_input_t in, int mic_gain_db)
{
    esp_err_t e = 0;
    e |= wr(0x19, 0x04);   /* DACCONTROL3: mute */
    e |= wr(0x01, 0x50);   /* CONTROL2 */
    e |= wr(0x02, 0x00);   /* CHIPPOWER: all on */
    e |= wr(0x35, 0xA0);   /* no internal DLL */
    e |= wr(0x37, 0xD0);
    e |= wr(0x39, 0xD0);
    e |= wr(0x08, 0x00);   /* MASTERMODE: slave */
    e |= wr(0x04, 0xC0);   /* DACPOWER: off while configuring */
    e |= wr(0x00, 0x12);   /* CONTROL1: play and record */
    e |= wr(0x17, 0x18);   /* DACCONTROL1: 16 bit I2S */
    e |= wr(0x18, 0x02);   /* DACCONTROL2: DACLRCK = MCLK/256 */
    e |= wr(0x26, 0x00);   /* DACCONTROL16 */
    e |= wr(0x27, 0x90);   /* left DAC to left mixer */
    e |= wr(0x2A, 0x90);   /* right DAC to right mixer */
    e |= wr(0x2B, 0x80);   /* DACCONTROL21: ADC and DAC share LRCK */
    e |= wr(0x2D, 0x00);
    e |= wr(0x1A, 0x00);   /* DAC volume 0 dB */
    e |= wr(0x1B, 0x00);
    e |= wr(0x04, 0x3C);   /* DACPOWER: LOUT1/ROUT1/LOUT2/ROUT2 on */
    e |= wr(0x03, 0xFF);   /* ADCPOWER: off while configuring */
    e |= es8388_set_input(in, mic_gain_db);
    e |= wr(0x0B, 0x02);   /* ADCCONTROL3 */
    e |= wr(0x0C, 0x0C);   /* ADCCONTROL4: 16 bit I2S */
    e |= wr(0x0D, 0x02);   /* ADCCONTROL5: ADCLRCK = MCLK/256 */
    e |= wr(0x10, 0x00);   /* ADC volume 0 dB */
    e |= wr(0x11, 0x00);
    e |= wr(0x12, 0x38);   /* ADCCONTROL10: ALC off */
    e |= wr(0x03, 0x09);   /* ADCPOWER: ADC and inputs on, mic bias on */
    e |= es8388_set_out_volume(30);
    e |= wr(0x19, 0x00);   /* unmute */
    vTaskDelay(pdMS_TO_TICKS(20));
    return e;
}
