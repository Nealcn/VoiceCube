#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for the ES8311 codec device (from esp_codec_dev)
 */
typedef struct esp_codec_dev *es8311_codec_handle_t;

/**
 * @brief Initialize ES8311 codec via esp_codec_dev
 *
 * Sets up I2S RX channel, I2C control interface, and creates
 * the esp_codec_dev handle. No external MCLK is required
 * (BCLK is used as clock source, matching Atomic Echo Base wiring).
 *
 * @param[out] out_handle  Codec device handle for subsequent operations
 * @return ESP_OK on success
 */
esp_err_t es8311_audio_init(es8311_codec_handle_t *out_handle);

/**
 * @brief Open codec for capture (starts the I2S stream)
 * @param handle  Codec handle from es8311_audio_init()
 * @return ESP_OK on success
 */
esp_err_t es8311_audio_start_capture(es8311_codec_handle_t handle);

/**
 * @brief Stop capture and close codec
 * @param handle  Codec handle
 * @return ESP_OK on success
 */
esp_err_t es8311_audio_stop_capture(es8311_codec_handle_t handle);

/**
 * @brief Read PCM data from the codec
 *
 * Wrapper around esp_codec_dev_read().
 *
 * @param handle      Codec handle
 * @param buf         Destination buffer
 * @param len         Buffer size in bytes
 * @param[out] bytes_read  Number of bytes actually read
 * @param timeout_ms  Read timeout in milliseconds (ignored: esp_codec_dev_read() has no timeout param)
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if no data arrived
 */
esp_err_t es8311_audio_read(es8311_codec_handle_t handle, void *buf,
                             size_t len, size_t *bytes_read, int timeout_ms);

/**
 * @brief Deinitialize codec and free all resources
 * @param handle  Codec handle (NULL-safe)
 * @return ESP_OK
 */
esp_err_t es8311_audio_deinit(es8311_codec_handle_t handle);

#ifdef __cplusplus
}
#endif
