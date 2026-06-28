#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback for captured PCM audio data (from ES8311 codec)
 * @param data  Pointer to 16-bit mono PCM data
 * @param samples Number of samples in this buffer
 * @param session_id Current recording session ID
 */
typedef void (*es8311_mic_data_cb_t)(const int16_t *data, size_t samples, uint32_t session_id);

/**
 * @brief Initialize the ES8311 codec mic (I2S + I2C)
 * @param data_cb Callback invoked when PCM data is ready
 * @return ESP_OK on success
 */
esp_err_t es8311_mic_init(es8311_mic_data_cb_t data_cb);

/**
 * @brief Start ES8311 mic capture
 * @param session_id Recording session identifier
 * @return ESP_OK on success
 */
esp_err_t es8311_mic_start(uint32_t session_id);

/**
 * @brief Stop ES8311 mic capture
 * @return ESP_OK on success
 */
esp_err_t es8311_mic_stop(void);

/**
 * @brief Check if the mic is currently capturing
 */
bool es8311_mic_is_capturing(void);

/**
 * @brief Deinitialize and free resources
 */
esp_err_t es8311_mic_deinit(void);

#ifdef __cplusplus
}
#endif
