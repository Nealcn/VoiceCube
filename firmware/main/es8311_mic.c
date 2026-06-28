#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "es8311_mic.h"
#include "board_config.h"
#include "es8311_audio.h"

static const char *TAG = "es8311_mic";

/* ---- State ---- */
static es8311_codec_handle_t s_codec = NULL;   /* handle from es8311_audio_init() */
static bool s_capturing = false;
static uint32_t s_session_id = 0;
static es8311_mic_data_cb_t s_data_cb = NULL;

#define DMA_BUF_SIZE    2048
typedef struct { int16_t buf[DMA_BUF_SIZE / 2]; } rbuf_t;

/* ---- Public API ---- */

esp_err_t es8311_mic_init(es8311_mic_data_cb_t data_cb)
{
    ESP_RETURN_ON_FALSE(data_cb, ESP_ERR_INVALID_ARG, TAG, "cb NULL");
    s_data_cb = data_cb;

    /* Initialize ES8311 codec via esp_codec_dev */
    esp_err_t err = es8311_audio_init(&s_codec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ES8311 mic initialized: %d Hz 16-bit mono",
             BOARD_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t es8311_mic_start(uint32_t session_id)
{
    if (s_capturing) return ESP_OK;
    if (!s_codec) return ESP_ERR_INVALID_STATE;

    s_session_id = session_id;
    s_capturing = true;

    esp_err_t err = es8311_audio_start_capture(s_codec);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "started session=%u", session_id);
    else
        s_capturing = false;
    return err;
}

esp_err_t es8311_mic_stop(void)
{
    if (!s_capturing) return ESP_OK;
    s_capturing = false;

    if (s_codec)
        es8311_audio_stop_capture(s_codec);

    ESP_LOGI(TAG, "stopped session=%u", s_session_id);
    return ESP_OK;
}

bool es8311_mic_is_capturing(void) { return s_capturing; }

esp_err_t es8311_mic_deinit(void)
{
    s_capturing = false;
    if (s_codec) {
        es8311_audio_deinit(s_codec);
        s_codec = NULL;
    }
    s_data_cb = NULL;
    return ESP_OK;
}

/* ---- Read task: read 16 kHz PCM from ES8311 via esp_codec_dev ---- */

void es8311_mic_read_task(void *arg)
{
    ESP_LOGI(TAG, "read task started (esp_codec_dev)");
    rbuf_t *rb = malloc(sizeof(*rb));
    if (!rb) { vTaskDelete(NULL); return; }

    /* PCM accumulation: 60 ms @ 16 kHz = 960 samples */
    static int16_t pcm[960];
    static int pcnt = 0;
    bool was_paused = true;  /* reset pcnt on first capture start */

    while (1) {
        if (!s_capturing || !s_codec) {
            was_paused = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Drain stale DMA data accumulated while paused (prevents burst on resume) */
        if (was_paused) {
            pcnt = 0;
            was_paused = false;
            size_t dummy = 0;
            es8311_audio_read(s_codec, rb->buf, sizeof(rb->buf), &dummy, 0);
        }

        size_t br = 0;
        esp_err_t err = es8311_audio_read(s_codec, rb->buf, sizeof(rb->buf),
                                           &br, 100 /* ms */);
        if (err == ESP_ERR_TIMEOUT) {
            static int tc = 0;
            if (++tc % 10 == 1) ESP_LOGW(TAG, "timeout (%d)", tc);
            vTaskDelay(1);
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "read err: %s", esp_err_to_name(err));
            vTaskDelay(1);
            continue;
        }

        /* Reset timeout counter on successful read */
        static int _unused = 0; (void)_unused;

        size_t ns = br / 2;  /* 16-bit samples */
        /* Clamp to avoid overwriting pcm[960] when i2s returns >1920 bytes */
        if (ns > (size_t)(960 - pcnt)) {
            ns = 960 - pcnt;
        }
        for (size_t i = 0; i < ns; i++) {
            pcm[pcnt++] = rb->buf[i];
            if (pcnt >= 960) {
                if (s_data_cb) s_data_cb(pcm, 960, s_session_id);
                pcnt = 0;
            }
        }
    }
    free(rb);
    vTaskDelete(NULL);
}
