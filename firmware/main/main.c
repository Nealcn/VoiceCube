/**
 * @file main.c
 * @brief M5AtomS3 Voice Stick - Main application state machine
 *
 * States:
 *   ADVERTISING → IDLE → RECORDING → WAITING_ASR → COMPLETE → IDLE
 *                                    ↑                │
 *                                    └── Cancel ──────┘
 *
 * Architecture:
 *   PDM Mic → Opus encode → BLE notify → Windows Desktop → ASR → Paste
 *
 * Single button:
 *   Press down  → start recording, LED red
 *   Release     → stop, send end-of-session, LED yellow (waiting ASR)
 *   Double-click → cancel recording or discard ASR result, LED red X
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_config.h"
#include "es8311_mic.h"
#include "opus_encoder.h"
#include "ble_service.h"
#include "display.h"
#include "button.h"

static const char *TAG = "atom_voicestick";

// ---- Application states ----
typedef enum {
    APP_STATE_ADVERTISING,       // BLE advertising, not connected
    APP_STATE_IDLE,              // Connected, ready to record
    APP_STATE_RECORDING,         // Capturing and transmitting audio
    APP_STATE_WAITING_ASR,       // Audio sent, waiting for ASR result
    APP_STATE_COMPLETE,          // Received result → will return to idle
} app_state_t;

static app_state_t s_state = APP_STATE_ADVERTISING;
static uint32_t s_session_id = 0;
static uint32_t s_seq = 0;
static bool s_asr_success = false;

// PCM accumulation buffer (Opus frame buffer)
static int16_t *s_pcm_buffer = NULL;
static size_t s_pcm_count = 0;
static size_t s_pcm_capacity = 0;

// Recording wall-clock start (for toggle-mode duration check)
static int64_t s_rec_start_us = 0;
/* ES8311 init result — stored for on-demand display */
static esp_err_t s_mic_init_result = ESP_FAIL;

/* PM lock to prevent CPU freq change / light sleep during recording */
static esp_pm_lock_handle_t s_pm_lock = NULL;

// Timeout timer for WAITING_ASR state (prevents permanent hang)
static esp_timer_handle_t s_asr_timeout_timer = NULL;
#define ASR_TIMEOUT_MS 15000  // 15 seconds

// Forward declarations
static void set_state(app_state_t new_state);
static void stop_asr_timeout(void);
static void start_asr_timeout(void);
static void stop_recording(void);
static void start_recording(void);

static void asr_timeout_cb(void *arg)
{
    (void)arg;
    if (s_state == APP_STATE_WAITING_ASR || s_state == APP_STATE_COMPLETE) {
        ESP_LOGW(TAG, "ASR timeout: returning to IDLE");
        set_state(APP_STATE_IDLE);
        display_set_state(DISP_STATE_IDLE);
    }
}

static void start_asr_timeout(void)
{
    if (s_asr_timeout_timer) {
        esp_timer_stop(s_asr_timeout_timer);
        esp_timer_start_once(s_asr_timeout_timer, ASR_TIMEOUT_MS * 1000);
    }
}

static void stop_asr_timeout(void)
{
    if (s_asr_timeout_timer) {
        esp_timer_stop(s_asr_timeout_timer);
    }
}

// ---- State name helper ----
static const char *state_name(app_state_t state)
{
    switch (state) {
    case APP_STATE_ADVERTISING:  return "ADVERTISING";
    case APP_STATE_IDLE:         return "IDLE";
    case APP_STATE_RECORDING:    return "RECORDING";
    case APP_STATE_WAITING_ASR:  return "WAITING_ASR";
    case APP_STATE_COMPLETE:     return "COMPLETE";
    default:                     return "UNKNOWN";
    }
}

static void set_state(app_state_t new_state)
{
    if (s_state != new_state) {
        ESP_LOGI(TAG, "APP state: %s → %s", state_name(s_state), state_name(new_state));
        s_state = new_state;
    }
}

// ---- PCM accumulation (callback from ES8311 mic) ----
static void on_pcm_data(const int16_t *data, size_t samples, uint32_t session_id)
{
    if (s_state != APP_STATE_RECORDING || !s_pcm_buffer || !ble_service_is_connected()) {
        return;
    }

    if (s_pcm_count == 0) {
        ESP_LOGD(TAG, "PCM data arriving: %u samples, session=%u", (unsigned)samples, session_id);
    }

    // Accumulate PCM samples
    size_t space = s_pcm_capacity - s_pcm_count;
    if (samples > space) {
        ESP_LOGW(TAG, "PCM buffer overflow, dropping %u samples", (unsigned)(samples - space));
        samples = space;
    }

    memcpy(s_pcm_buffer + s_pcm_count, data, samples * sizeof(int16_t));
    s_pcm_count += samples;

    // When we have a full Opus frame, encode and send
    size_t frame_samples = audio_encoder_frame_samples();
    int frames_sent = 0;
    while (s_pcm_count >= frame_samples) {
        // Yield every 5 frames to prevent TWDT / give NimBLE time to process
        if (++frames_sent > 5) {
            frames_sent = 0;
            vTaskDelay(1);
        }

        // Encode to Opus
        static uint8_t opus_data[400]; // Max Opus frame size (BSS, not stack)
        size_t opus_len = 0;

        esp_err_t err = audio_encode_frame(s_pcm_buffer, frame_samples,
                                          opus_data, sizeof(opus_data), &opus_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Opus encode error: %s", esp_err_to_name(err));
        } else if (opus_len > 0) {
            // Send via BLE
            uint8_t flags = 0;
            if (s_seq == 0) {
                flags |= AUDIO_FRAME_FLAG_START;
            }
            ble_service_send_audio(opus_data, opus_len, session_id, s_seq, flags);
            s_seq++;
        }

        // Shift remaining data
        size_t remaining = s_pcm_count - frame_samples;
        if (remaining > 0) {
            memmove(s_pcm_buffer, s_pcm_buffer + frame_samples, remaining * sizeof(int16_t));
        }
        s_pcm_count = remaining;
    }
}

// ---- Send end-of-stream frame ----
static void send_audio_end(uint32_t session_id)
{
    if (ble_service_is_connected()) {
        ble_service_send_audio(NULL, 0, session_id, s_seq, AUDIO_FRAME_FLAG_END);
    }
}

// ---- Send button state over BLE ----
static void send_button_down(const char *button, uint32_t session_id)
{
    char json[128];
    snprintf(json, sizeof(json),
             "{\"event\":\"button_down\",\"button\":\"%s\",\"session_id\":%lu}",
             button, (unsigned long)session_id);
    ble_service_send_state(json);
}

static void send_button_up(const char *button, uint32_t duration_ms, uint32_t session_id)
{
    char json[128];
    snprintf(json, sizeof(json),
             "{\"event\":\"button_up\",\"button\":\"%s\",\"duration_ms\":%lu,\"session_id\":%lu}",
             button, (unsigned long)duration_ms, (unsigned long)session_id);
    ble_service_send_state(json);
}

static void send_button_click(const char *button, uint32_t duration_ms, uint32_t session_id)
{
    char json[128];
    snprintf(json, sizeof(json),
             "{\"event\":\"button_down\",\"button\":\"%s\",\"session_id\":%lu}",
             button, (unsigned long)session_id);
    ble_service_send_state(json);
    snprintf(json, sizeof(json),
             "{\"event\":\"button_up\",\"button\":\"%s\",\"duration_ms\":%lu,\"session_id\":%lu}",
             button, (unsigned long)duration_ms, (unsigned long)session_id);
    ble_service_send_state(json);
}

// ---- Start recording session ----
static void start_recording(void)
{
    s_session_id++;
    s_seq = 0;
    s_pcm_count = 0;
    s_asr_success = false;
    s_rec_start_us = esp_timer_get_time();

    // Lazily allocate PCM buffer on first use
    if (!s_pcm_buffer) {
        size_t max_samples = BOARD_AUDIO_SAMPLE_RATE * 2; // 2s @ 16kHz
        s_pcm_buffer = malloc(max_samples * sizeof(int16_t));
        s_pcm_capacity = s_pcm_buffer ? max_samples : 0;
        if (!s_pcm_buffer) {
            ESP_LOGW(TAG, "PCM buffer alloc failed, recording without accumulation");
        }
    }

    // Lock CPU frequency to prevent PM light sleep during recording
    if (s_pm_lock) {
        esp_pm_lock_acquire(s_pm_lock);
    }

    // Start mic capture (ADC + I2S)
    esp_err_t err = es8311_mic_start(s_session_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mic start failed: %s — continuing without mic", esp_err_to_name(err));
    }

    // Reset Opus encoder for new session
    audio_encoder_reset();

    // Set display to RED first, then do non-critical work
    set_state(APP_STATE_RECORDING);
    display_set_state(DISP_STATE_RECORDING);

    // BLE notify (non-critical, may fail silently)
    ble_service_send_state("{\"event\":\"button_down\",\"button\":\"primary\"}");

    ESP_LOGI(TAG, "recording started: session=%u mic=%s", s_session_id,
             err == ESP_OK ? "ok" : "FAIL");
}

// ---- Stop recording and send audio ----
static void stop_recording(void)
{
    if (s_state != APP_STATE_RECORDING) {
        return;
    }

    // Use wall-clock recording duration (not button press duration)
    uint32_t duration_ms = (uint32_t)((esp_timer_get_time() - s_rec_start_us) / 1000);
    uint32_t session = s_session_id;

    // Stop ES8311 mic capture
    es8311_mic_stop();

    // Release PM lock so light sleep / freq scaling can resume
    if (s_pm_lock) {
        esp_pm_lock_release(s_pm_lock);
    }

    vTaskDelay(2);  // yield: let read task stop before we touch PCM buffer

    // Encode any remaining PCM in the buffer
    if (s_pcm_count > 0 && s_pcm_buffer) {
        size_t frame_samples = audio_encoder_frame_samples();
        // Pad with silence if needed
        while (s_pcm_count < frame_samples && s_pcm_count < s_pcm_capacity) {
            s_pcm_buffer[s_pcm_count] = 0;
            s_pcm_count++;
        }
        if (s_pcm_count >= frame_samples) {
            uint8_t opus_data[400];
            size_t opus_len = 0;
            if (audio_encode_frame(s_pcm_buffer, frame_samples,
                                  opus_data, sizeof(opus_data), &opus_len) == ESP_OK
                && opus_len > 0) {
                ble_service_send_audio(opus_data, opus_len, session, s_seq, 0);
                s_seq++;
            }
        }
    }

    vTaskDelay(1);  // yield before BLE sends

    // Send end-of-session marker
    send_audio_end(session);
    vTaskDelay(1);

    // Send button up event
    send_button_up("primary", duration_ms, session);
    vTaskDelay(1);

    // Go directly back to IDLE (blue) — no yellow WAITING_ASR state
    set_state(APP_STATE_IDLE);
    display_set_state(DISP_STATE_IDLE);

    ESP_LOGI(TAG, "recording stopped: session=%u, duration=%u ms", session, duration_ms);
}

// ---- Cancel current recording / waiting state ----
static void cancel_action(void)
{
    if (s_state == APP_STATE_RECORDING) {
        es8311_mic_stop();
        s_pcm_count = 0;
        ESP_LOGI(TAG, "recording cancelled");
    }

    send_button_click("secondary", 0, 0);
    set_state(APP_STATE_IDLE);
    display_set_state(DISP_STATE_CANCELLED);
}

// ---- Button callback (hold-to-talk: press to start, release to stop) ----
static void on_button_event(button_event_t event, uint32_t duration_ms)
{
    (void)duration_ms;

    // Print init error at first press for visibility
    if (event == BUTTON_EVENT_PRESS_DOWN && s_mic_init_result != ESP_OK) {
        ESP_LOGE(TAG, "MIC init failed at boot: %s", esp_err_to_name(s_mic_init_result));
    }

    switch (event) {
    case BUTTON_EVENT_PRESS_DOWN:
        ESP_LOGI(TAG, "btn down state=%s", state_name(s_state));
        if (s_state == APP_STATE_IDLE || s_state == APP_STATE_ADVERTISING) {
            start_recording();
        }
        break;

    case BUTTON_EVENT_PRESS_UP:
        if (s_state == APP_STATE_RECORDING) {
            stop_recording();
        }
        break;

    case BUTTON_EVENT_DOUBLE_CLICK:
        cancel_action();
        break;

    case BUTTON_EVENT_LONG_PRESS:
        break;
    }
}

// ---- BLE control callback ----
static void on_ble_control(const char *json)
{
    ESP_LOGI(TAG, "control: %s", json);

    // Parse JSON for ui_state updates from Windows app
    // Format: {"event":"ui_state","state":"ready","text":"..."}
    // We use this to know when ASR completes
    if (strstr(json, "ui_state")) {
        if (strstr(json, "\"state\":\"ready\"")) {
            stop_asr_timeout();
            if (s_state == APP_STATE_WAITING_ASR || s_state == APP_STATE_COMPLETE) {
                set_state(APP_STATE_IDLE);
                display_set_state(DISP_STATE_IDLE);
            }
        } else if (strstr(json, "\"state\":\"pending_confirmation\"")) {
            stop_asr_timeout();
            // ASR result received and ready to paste
            if (s_state == APP_STATE_WAITING_ASR) {
                set_state(APP_STATE_COMPLETE);
                display_set_state(DISP_STATE_CONFIRMED);
            }
        } else if (strstr(json, "\"state\":\"error\"")) {
            stop_asr_timeout();
            display_set_state(DISP_STATE_ERROR);
            // Return to idle after a moment
            set_state(APP_STATE_IDLE);
        }
    } else if (strstr(json, "\"event\":\"interaction_mode\"")) {
        // Could handle interaction mode switching in the future
        ESP_LOGI(TAG, "interaction mode update ignored (hold-to-talk only)");
    }
}

// ---- BLE connection callback (from ble_service via state events) ----
// We handle connection state through the app lifecycle.
// The ble_service notifies us via control events and we track is_connected().

// ---- Entry point ----
void app_main(void)
{
    ESP_LOGI(TAG, "===== M5AtomS3 Voice Stick booting =====");

    // Initialize NVS (needed for BLE)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize BLE service FIRST (before other drivers to avoid heap conflicts)
    esp_err_t ble_err = ble_service_init(BOARD_BLE_DEVICE_PREFIX);
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s, continuing without BLE", esp_err_to_name(ble_err));
    } else {
        ble_service_set_control_callback(on_ble_control);
        ble_service_set_connect_callback(display_set_ble_connected);
    }

    // Create PM lock to prevent light sleep during recording
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "recording", &s_pm_lock);

    // Create ASR timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = asr_timeout_cb,
        .name = "asr_timeout"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_asr_timeout_timer));

    // Initialize LED matrix (visual feedback)
    ESP_ERROR_CHECK(display_init());
    display_set_state(DISP_STATE_IDLE);

    // Initialize ES8311 mic FIRST (before Opus to avoid heap corruption)
    ESP_LOGI(TAG, "Initializing ES8311 mic...");
    s_mic_init_result = es8311_mic_init(on_pcm_data);
    if (s_mic_init_result != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 mic init failed: %s", esp_err_to_name(s_mic_init_result));
    }

    // Initialize Opus encoder (now uses internal RAM to avoid PSRAM issue)
    ESP_LOGI(TAG, "Initializing Opus encoder...");
    esp_err_t opus_err = audio_encoder_init(
        BOARD_AUDIO_SAMPLE_RATE,
        BOARD_AUDIO_CHANNELS,
        BOARD_AUDIO_FRAME_MS
    );
    if (opus_err != ESP_OK) {
        ESP_LOGW(TAG, "Opus init failed: %s, continuing without encoding", esp_err_to_name(opus_err));
    }
    // Note: es8311_mic is started/stopped per recording session

    // Initialize button
    ESP_ERROR_CHECK(button_init(on_button_event));

    ESP_LOGI(TAG, "creating ES8311 mic read task...");
    extern void es8311_mic_read_task(void *arg);
    BaseType_t task_ok = xTaskCreate(es8311_mic_read_task, "es8311_read", 32768, NULL, 6, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create ES8311 read task");
    }

    set_state(APP_STATE_ADVERTISING);
    display_set_state(DISP_STATE_IDLE);

    ESP_LOGI(TAG, "===== M5AtomS3 Voice Stick ready =====");
    ESP_LOGI(TAG, "Device: %s", ble_service_device_name());

    // Enter advertising state - wait for BLE connection
    // The main work is done in tasks/ISRs; app_main just sets up and returns.
    // State transitions happen in button and BLE callbacks.
}
