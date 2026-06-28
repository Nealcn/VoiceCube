/**
 * @file button.c
 * @brief GPIO falling-edge ISR → wakes dedicated task for debounce + callback.
 * Supports PRESS_DOWN, PRESS_UP, DOUBLE_CLICK, and duration tracking.
 */
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "button.h"
#include "board_config.h"

static const char *TAG = "button";
static button_cb_t s_cb = NULL;
static SemaphoreHandle_t s_sem = NULL;
static TaskHandle_t s_task = NULL;
static int64_t s_press_start_us = 0;  // 0 = not pressed

#define DOUBLE_CLICK_WINDOW_MS   300

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(s_sem, &wake);
    if (wake) portYIELD_FROM_ISR();
}

static void button_task(void *arg)
{
    (void)arg;

    // Configure GPIO falling-edge interrupt
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOARD_BUTTON_PIN, gpio_isr_handler, NULL);

    ESP_LOGI(TAG, "ready GPIO%d", BOARD_BUTTON_PIN);

    while (1) {
        // Wait for ISR to signal a potential press
        xSemaphoreTake(s_sem, portMAX_DELAY);

        // Debounce: wait 50ms then re-check
        vTaskDelay(pdMS_TO_TICKS(50));

        if (gpio_get_level(BOARD_BUTTON_PIN) == 0) {
            // Still LOW — confirmed press
            s_press_start_us = esp_timer_get_time();
            ESP_LOGI(TAG, "PRESS_DOWN");
            if (s_cb) s_cb(BUTTON_EVENT_PRESS_DOWN, 0);

            // Wait for release (poll with delay)
            while (gpio_get_level(BOARD_BUTTON_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // Button released
            uint32_t duration_ms = (uint32_t)((esp_timer_get_time() - s_press_start_us) / 1000);
            s_press_start_us = 0;
            ESP_LOGI(TAG, "PRESS_UP duration=%u ms", duration_ms);
            if (s_cb) s_cb(BUTTON_EVENT_PRESS_UP, duration_ms);

            // Wait for double-click window: 300ms for second press
            TickType_t start = xTaskGetTickCount();
            bool double_click = false;
            while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(DOUBLE_CLICK_WINDOW_MS)) {
                if (gpio_get_level(BOARD_BUTTON_PIN) == 0) {
                    // Second press detected within window
                    vTaskDelay(pdMS_TO_TICKS(50));  // debounce
                    if (gpio_get_level(BOARD_BUTTON_PIN) == 0) {
                        double_click = true;
                        // Wait for release and consume semaphore counts
                        while (gpio_get_level(BOARD_BUTTON_PIN) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                        break;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (double_click) {
                ESP_LOGI(TAG, "DOUBLE_CLICK");
                if (s_cb) s_cb(BUTTON_EVENT_DOUBLE_CLICK, 0);
            }
        }

        // Consume any stale semaphore counts
        while (xSemaphoreTake(s_sem, 0) == pdTRUE);
    }
}

esp_err_t button_init(button_cb_t cb)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "cb required");
    s_cb = cb;

    s_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_sem, ESP_ERR_NO_MEM, TAG, "semaphore");

    BaseType_t ok = xTaskCreate(button_task, "btn", 6144, NULL, 7, &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");

    return ESP_OK;
}

uint32_t button_get_press_duration_ms(void)
{
    if (s_press_start_us == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_press_start_us) / 1000);
}

bool button_is_pressed(void)
{
    return gpio_get_level(BOARD_BUTTON_PIN) == 0;
}

void button_deinit(void) { s_cb = NULL; }
