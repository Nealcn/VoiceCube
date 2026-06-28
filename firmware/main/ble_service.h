#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- VoiceStick-compatible BLE Protocol (NimBLE version) ----
// Service:    8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
// audio_tx:   8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101  (notify)
// state_tx:   8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102  (notify)
// control_rx: 8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103  (write without rsp)

// ---- Audio BLE Frame (from voicestick protocol, little-endian) ----
typedef struct __attribute__((packed)) {
    uint8_t  version;       // 1
    uint8_t  type;          // 0x01 audio
    uint16_t header_len;    // 16
    uint32_t session_id;
    uint32_t seq;
    uint8_t  flags;         // bit0=start, bit1=end
    uint8_t  reserved;      // 0
    uint16_t payload_len;
    uint8_t  payload[0];    // Opus packet data
} audio_ble_frame_t;

#define AUDIO_FRAME_HEADER_SIZE  16
#define AUDIO_FRAME_TYPE         0x01
#define AUDIO_FRAME_FLAG_START   0x01
#define AUDIO_FRAME_FLAG_END     0x02

// ---- State BLE Frame ----
typedef struct __attribute__((packed)) {
    uint8_t  version;       // 1
    uint8_t  type;          // 0x10 state
    uint16_t payload_len;
    uint8_t  payload[0];    // JSON string
} state_ble_frame_t;

#define STATE_FRAME_TYPE 0x10

// ---- Callbacks ----
typedef void (*ble_control_cb_t)(const char *json);
typedef void (*ble_connect_cb_t)(bool connected);

// ---- Public API ----

esp_err_t ble_service_init(const char *device_name_prefix);
void ble_service_set_control_callback(ble_control_cb_t cb);
void ble_service_set_connect_callback(ble_connect_cb_t cb);

esp_err_t ble_service_send_audio(const uint8_t *data, uint16_t len,
                                 uint32_t session_id, uint32_t seq, uint8_t flags);
esp_err_t ble_service_send_state(const char *json);

bool ble_service_is_connected(void);
const char *ble_service_device_name(void);

#ifdef __cplusplus
}
#endif
