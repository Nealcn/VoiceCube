#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "opus.h"
#include "opus_encoder.h"
#include "board_config.h"

static const char *TAG = "opus_enc";

static OpusEncoder *s_encoder = NULL;
static uint32_t s_sample_rate = 0;
static uint8_t s_channels = 0;
static uint32_t s_frame_ms = 0;
static size_t s_frame_samples = 0;

#define OPUS_BITRATE 28000

esp_err_t audio_encoder_init(uint32_t sample_rate, uint8_t channels, uint32_t frame_ms)
{
    ESP_RETURN_ON_FALSE(sample_rate > 0, ESP_ERR_INVALID_ARG, TAG, "invalid sample rate");
    ESP_RETURN_ON_FALSE(channels == 1, ESP_ERR_INVALID_ARG, TAG, "only mono supported");
    ESP_RETURN_ON_FALSE(s_encoder == NULL, ESP_ERR_INVALID_STATE, TAG, "encoder already initialized");

    s_sample_rate = sample_rate;
    s_channels = channels;
    s_frame_ms = frame_ms;
    s_frame_samples = (sample_rate * frame_ms) / 1000;

    ESP_LOGI(TAG, "Getting encoder size...");
    int enc_size = opus_encoder_get_size(channels);
    ESP_LOGI(TAG, "Encoder size: %d bytes", enc_size);

    ESP_LOGI(TAG, "Allocating %d bytes from internal RAM...", enc_size);
    OpusEncoder *enc = (OpusEncoder *)heap_caps_calloc(1, enc_size, MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Allocated at %p", enc);
    if (!enc) {
        ESP_LOGE(TAG, "heap_caps_calloc failed (trying PSRAM)...");
        enc = (OpusEncoder *)heap_caps_calloc(1, enc_size, MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "PSRAM alloc at %p", enc);
    }
    if (!enc) {
        ESP_LOGE(TAG, "all memory allocation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Calling opus_encoder_init...");
    int opus_err = opus_encoder_init(enc, sample_rate, channels, OPUS_APPLICATION_AUDIO);
    ESP_LOGI(TAG, "opus_encoder_init returned: %d", opus_err);
    if (opus_err != OPUS_OK) {
        free(enc);
        ESP_LOGE(TAG, "opus_encoder_init failed: %d", opus_err);
        return ESP_ERR_NO_MEM;
    }
    s_encoder = enc;

    ESP_LOGI(TAG, "Configuring encoder...");
    opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    ESP_LOGI(TAG, "Set bitrate done");
    opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(3));
    ESP_LOGI(TAG, "Set complexity done");
    opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(s_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(s_encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(s_encoder, OPUS_SET_PACKET_LOSS_PERC(0));
    ESP_LOGI(TAG, "Encoder configured");

    ESP_LOGI(TAG, "Opus encoder initialized: %d Hz, %d ch, %d ms, %d bps",
             sample_rate, channels, frame_ms, OPUS_BITRATE);
    return ESP_OK;
}

esp_err_t audio_encode_frame(const int16_t *pcm, size_t pcm_samples,
                            uint8_t *output, size_t output_size,
                            size_t *output_len)
{
    ESP_RETURN_ON_FALSE(s_encoder != NULL, ESP_ERR_INVALID_STATE, TAG, "encoder not initialized");
    ESP_RETURN_ON_FALSE(pcm != NULL, ESP_ERR_INVALID_ARG, TAG, "pcm is NULL");
    ESP_RETURN_ON_FALSE(output != NULL, ESP_ERR_INVALID_ARG, TAG, "output is NULL");

    opus_int32 encoded = opus_encode(s_encoder, pcm, (int)pcm_samples, output, (opus_int32)output_size);
    if (encoded < 0) {
        ESP_LOGE(TAG, "opus_encode failed: %d", (int)encoded);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *output_len = (size_t)encoded;
    return ESP_OK;
}

void audio_encoder_reset(void)
{
    if (s_encoder) {
        opus_encoder_ctl(s_encoder, OPUS_RESET_STATE);
    }
}

void audio_encoder_deinit(void)
{
    if (s_encoder) {
        opus_encoder_destroy(s_encoder);
        s_encoder = NULL;
    }
    s_sample_rate = 0;
    s_channels = 0;
    s_frame_ms = 0;
    s_frame_samples = 0;
    ESP_LOGI(TAG, "Opus encoder deinitialized");
}

size_t audio_encoder_frame_samples(void)
{
    return s_frame_samples;
}
