#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Display state indicators (same role as old LED states)
 */
typedef enum {
    DISP_STATE_IDLE,              // Blue screen
    DISP_STATE_RECORDING,         // Red screen
    DISP_STATE_THINKING,          // Yellow screen
    DISP_STATE_CONFIRMED,         // Green flash x2
    DISP_STATE_CANCELLED,         // Red X flash
    DISP_STATE_ERROR,             // Red blinking
} disp_state_t;

/**
 * @brief Initialize GC9107 LCD (128x128 color IPS) on AtomS3R
 * SPI: SCK=GPIO15, MOSI=GPIO21, CS=GPIO14
 * DC=GPIO42, RST=GPIO48
 * Backlight: LP5562 I2C (SDA=GPIO45, SCL=GPIO0)
 */
esp_err_t display_init(void);

/**
 * @brief Set current display state (triggers animation)
 */
void display_set_state(disp_state_t state);

/**
 * @brief Show/hide BLE connected indicator (small green dot)
 */
void display_set_ble_connected(bool connected);

/**
 * @brief Set brightness (0-100)
 */
void display_set_brightness(uint8_t percent);

/**
 * @brief Deinitialize
 */
void display_deinit(void);

#ifdef __cplusplus
}
#endif
