#pragma once

#include <stdint.h>

// ============================================================
// M5AtomS3R Board Pin Definitions
// Reference: https://docs.m5stack.com/en/core/AtomS3R
// ============================================================

// --- Atomic Echo Base (ES8311 I2S Codec) ---
// The base connects through the bottom expansion port
#define ES8311_I2C_SDA          GPIO_NUM_38
#define ES8311_I2C_SCL          GPIO_NUM_39
#define ES8311_I2C_ADDR         0x18
#define ES8311_PCA9557_ADDR     0x43

#define ES8311_I2S_BCK_PIN      GPIO_NUM_8   // I2S bit clock
#define ES8311_I2S_WS_PIN       GPIO_NUM_6   // I2S word select
#define ES8311_I2S_DIN_PIN      GPIO_NUM_7   // I2S mic data in
#define ES8311_I2S_DOUT_PIN     GPIO_NUM_5   // I2S speaker data out

// ---- Built-in PDM Microphone (MSM381A3729H9BPC) - DEPRECATED: use ES8311 on Atomic Echo Base
// Previously: CLK=GPIO1, DATA=GPIO2
// Keep pins commented for reference only
//#define BOARD_PDM_CLK_PIN       GPIO_NUM_1
//#define BOARD_PDM_DATA_PIN      GPIO_NUM_2

// --- User Button (press on screen area) ---
// Active low (pull-up internally)
#define BOARD_BUTTON_PIN        GPIO_NUM_41  // 官方标注，此版本按→红可工作

// --- USB / Power ---
// AtomS3 is USB-powered only; no battery/PMIC
// USB-C VBUS detect (if needed)
#define BOARD_USB_DETECT_PIN    GPIO_NUM_0   // USB detect (when connected)

// --- Audio Configuration ---
#define BOARD_AUDIO_SAMPLE_RATE 16000        // 16 kHz for ASR
#define BOARD_AUDIO_BITS        16           // 16-bit PCM
#define BOARD_AUDIO_CHANNELS    1            // Mono
#define BOARD_AUDIO_FRAME_MS    60           // 60 ms per Opus frame
#define BOARD_AUDIO_FRAME_SAMPLES \
    (BOARD_AUDIO_SAMPLE_RATE * BOARD_AUDIO_FRAME_MS / 1000)  // 960 samples

// ---- PDM Clock Configuration (deprecated - ES8311 I2S used instead) ---
//#define BOARD_PDM_CLK_DIV       80

// --- BLE Configuration ---
#define BOARD_BLE_DEVICE_PREFIX "VS-"        // Must match voicestick prefix
