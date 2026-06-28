#include "es8311_audio.h"
#include "board_config.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s.h"

static const char *TAG = "es8311";
static i2c_master_bus_handle_t s_i2c_bus = NULL;

static void power_pca9557(void)
{
    i2c_master_dev_handle_t dev;
    i2c_device_config_t cfg = { .device_address = ES8311_PCA9557_ADDR, .scl_speed_hz = 100000 };
    if (i2c_master_probe(s_i2c_bus, ES8311_PCA9557_ADDR, 100) != ESP_OK) return;
    if (i2c_master_bus_add_device(s_i2c_bus, &cfg, &dev) == ESP_OK) {
        uint8_t c[] = {0x03, 0xF8};
        i2c_master_transmit(dev, c, sizeof(c), pdMS_TO_TICKS(100));
        uint8_t o[] = {0x01, 0x00};  // ALL outputs LOW (amp off, everything off)
        i2c_master_transmit(dev, o, sizeof(o), pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGI(TAG, "PCA9557: all outputs LOW");
    }
}

static void es8311_reg_init(i2c_master_dev_handle_t dev)
{
    /* ADC only - mute DAC output to prevent hiss */
    uint8_t regs[][2] = {
        {0x00, 0x1F}, {0x00, 0x00},           // reset
        {0x01, 0xBF},                          // BCLK as MCLK
        {0x02, 0x18},  // pre_div=1, pre_multi=8 (BCLK as MCLK mode, was 0x20)
        {0x03, 0x10},                          // ADC OSR
        {0x04, 0x20},                          // DAC OSR
        {0x05, 0x00},                          // dividers
        {0x06, 0x03},  // BCLK div=4 (value 4-1=3), was 0x04
        {0x07, 0x00}, {0x08, 0xFF},  // LRCK dividers
        {0x09, 0x0C}, {0x0A, 0x0C},            // 16-bit I2S
        {0x0D, 0x01},                          // analog power (official driver value)
        {0x0E, 0x02},                          // PGA enable (official driver value)
        {0x14, 0x3A},  // Analog mic +6dB gain (bits[6:5]=01, not PDM mode!)
        {0x15, 0x40},                          // mic bias voltage (was 0x00!)
        {0x16, 0x00},                          // MIC gain scale
        {0x17, 0xBF},  // ADC PGA max gain (official driver: 0xBF, bit7=1 is normal)
        {0x1C, 0x6A},                          // HPF, EQ bypass
        {0x31, 0x60},                          // DAC mute (bits 6+5)
        {0x00, 0x80},                          // power on
    };
    for (int i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        i2c_master_transmit(dev, regs[i], 2, pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "ES8311 OK (ADC, DAC muted)");
}

esp_err_t es8311_audio_init(es8311_codec_handle_t *out_handle)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "=== init ===");

    i2c_master_bus_config_t bcfg = {
        .i2c_port = I2C_NUM_1, .sda_io_num = ES8311_I2C_SDA,
        .scl_io_num = ES8311_I2C_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bcfg, &s_i2c_bus) != ESP_OK) return ESP_FAIL;
    power_pca9557();

    i2s_config_t i2s_cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = BOARD_AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };
    if (i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL) != ESP_OK) goto fail;
    i2s_pin_config_t pin_cfg = {
        .bck_io_num = ES8311_I2S_BCK_PIN,
        .ws_io_num = ES8311_I2S_WS_PIN,
        .data_in_num = ES8311_I2S_DIN_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
    };
    if (i2s_set_pin(I2S_NUM_0, &pin_cfg) != ESP_OK) goto fail;

    i2c_master_dev_handle_t es8311_dev;
    i2c_device_config_t edev = { .device_address = ES8311_I2C_ADDR, .scl_speed_hz = 100000 };
    if (i2c_master_bus_add_device(s_i2c_bus, &edev, &es8311_dev) != ESP_OK) goto fail;
    es8311_reg_init(es8311_dev);

    *out_handle = (es8311_codec_handle_t)1;
    ESP_LOGI(TAG, "=== init DONE ===");
    return ESP_OK;

fail:
    if (s_i2c_bus) i2c_del_master_bus(s_i2c_bus);
    i2s_driver_uninstall(I2S_NUM_0);
    return ESP_FAIL;
}

esp_err_t es8311_audio_start_capture(es8311_codec_handle_t h) { (void)h; return ESP_OK; }
esp_err_t es8311_audio_stop_capture(es8311_codec_handle_t h)  { (void)h; return ESP_OK; }

esp_err_t es8311_audio_read(es8311_codec_handle_t h, void *buf, size_t len, size_t *br, int tmo)
{
    if (!buf || !br) return ESP_ERR_INVALID_ARG;
    (void)h;
    esp_err_t r = i2s_read(I2S_NUM_0, buf, len, br, pdMS_TO_TICKS(tmo));
    if (r == ESP_ERR_TIMEOUT) return ESP_ERR_TIMEOUT;
    if (r != ESP_OK) { *br = 0; return ESP_FAIL; }
    return ESP_OK;
}

esp_err_t es8311_audio_deinit(es8311_codec_handle_t h)
{
    (void)h;
    i2s_driver_uninstall(I2S_NUM_0);
    if (s_i2c_bus) i2c_del_master_bus(s_i2c_bus);
    return ESP_OK;
}
