#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"

static const char *TAG = "display";

#define LCD_HOST        SPI3_HOST
#define PIN_SCLK        15
#define PIN_MOSI        21
#define PIN_CS          14
#define PIN_DC          42
#define PIN_RST         48
#define W               128
#define H               128
#define LCD_ROW_OFFSET  32   // GC9107 visible area starts at row 32 (160-row RAM)

// LP5562 I2C
#define I2C_SDA         45
#define I2C_SCL         0
#define LP5562_ADDR     0x30

static spi_device_handle_t s_spi = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;   // LP5562 I2C bus handle (must outlive device)
static i2c_master_dev_handle_t s_lp5562 = NULL;
static disp_state_t s_state = DISP_STATE_IDLE;
static bool s_ble_connected = false;
static TaskHandle_t s_anim = NULL;
static uint8_t *s_fb = NULL;  // MSB-first byte pairs, DMA-aligned

// ---- SPI with DC callback ----
static void spi_pre_cb(spi_transaction_t *t) {
    gpio_set_level(PIN_DC, (int)t->user);
}

// ---- SPI helpers with optional CS hold ----
static void spi_cmd_hold(uint8_t cmd, bool hold_cs) {
    uint32_t flags = SPI_TRANS_USE_TXDATA;
    if (hold_cs) flags |= SPI_TRANS_CS_KEEP_ACTIVE;
    spi_transaction_t t = { .length = 8, .flags = flags,
        .tx_data = {cmd}, .user = (void*)0 };
    spi_device_transmit(s_spi, &t);
}

static void spi_data_hold(const uint8_t *d, size_t len, bool hold_cs) {
    if (len == 0) return;
    spi_transaction_t t = { .length = len * 8, .tx_buffer = d,
        .user = (void*)1,
        .flags = hold_cs ? SPI_TRANS_CS_KEEP_ACTIVE : 0 };
    spi_device_transmit(s_spi, &t);
}

// Shorthand wrappers matching original API
static void spi_cmd(uint8_t cmd) { spi_cmd_hold(cmd, false); }
static void spi_data(const uint8_t *d, size_t len) { spi_data_hold(d, len, false); }

// ---- GC9107 init (from xiaozhi-esp32) ----
static void lcd_init(void) {
    gpio_set_level(PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    spi_cmd(0xFE); spi_cmd(0xEF);
    spi_cmd(0xB0); spi_data((uint8_t[]){0xC0},1);
    spi_cmd(0xB2); spi_data((uint8_t[]){0x2F},1);
    spi_cmd(0xB3); spi_data((uint8_t[]){0x03},1);
    spi_cmd(0xB6); spi_data((uint8_t[]){0x19},1);
    spi_cmd(0xB7); spi_data((uint8_t[]){0x01},1);
    spi_cmd(0xAC); spi_data((uint8_t[]){0xCB},1);
    spi_cmd(0xAB); spi_data((uint8_t[]){0x0E},1);
    spi_cmd(0xB4); spi_data((uint8_t[]){0x04},1);
    spi_cmd(0xA8); spi_data((uint8_t[]){0x19},1);
    spi_cmd(0xB8); spi_data((uint8_t[]){0x08},1);
    spi_cmd(0xE8); spi_data((uint8_t[]){0x24},1);
    spi_cmd(0xE9); spi_data((uint8_t[]){0x48},1);
    spi_cmd(0xEA); spi_data((uint8_t[]){0x22},1);
    spi_cmd(0xC6); spi_data((uint8_t[]){0x30},1);
    spi_cmd(0xC7); spi_data((uint8_t[]){0x18},1);
    spi_cmd(0xF0); spi_data((uint8_t[]){0x1F,0x28,0x04,0x3E,0x2A,0x2E,0x20,0x00,0x0C,0x06,0x00,0x1C,0x1F,0x0F},14);
    spi_cmd(0xF1); spi_data((uint8_t[]){0x00,0x2D,0x2F,0x3C,0x6F,0x1C,0x0B,0x00,0x00,0x00,0x07,0x0D,0x11,0x0F},14);
    spi_cmd(0x36); spi_data((uint8_t[]){0x08},1);  // BGR
    spi_cmd(0x3A); spi_data((uint8_t[]){0x05},1);  // RGB565
    spi_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));
    spi_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(20));
}

// ---- LP5562 backlight (from xiaozhi-esp32) ----
static esp_err_t lp5562_init(void) {
    i2c_master_bus_config_t b = { .i2c_port = I2C_NUM_0, .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = 1 };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&b, &s_i2c_bus), TAG, "i2c");
    i2c_device_config_t d = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LP5562_ADDR, .scl_speed_hz = 100000 };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &d, &s_lp5562), TAG, "lp5562");
    i2c_master_transmit(s_lp5562, (uint8_t[]){0x00,0x40}, 2, 100); vTaskDelay(5);
    i2c_master_transmit(s_lp5562, (uint8_t[]){0x08,0x01}, 2, 100);
    i2c_master_transmit(s_lp5562, (uint8_t[]){0x70,0x00}, 2, 100);
    i2c_master_transmit(s_lp5562, (uint8_t[]){0x08,0x41}, 2, 100); vTaskDelay(5);
    i2c_master_transmit(s_lp5562, (uint8_t[]){0x0E,0x50}, 2, 100); // Dimmer (~31%)
    return ESP_OK;
}

// ---- Drawing ----
#define RGB565(r,g,b) ((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3))
#define C_BLACK   RGB565(0,0,0)
#define C_WHITE   RGB565(255,255,255)
#define C_RED     RGB565(255,0,0)
#define C_GREEN   RGB565(0,255,0)
#define C_BLUE    RGB565(0,0,200)
#define C_DBLUE   RGB565(0,0,80)
#define C_YELLOW_BG RGB565(200,180,0)

static void px(int x, int y, uint16_t c) {
    if (x<0||x>=W||y<0||y>=H) return;
    int i = (y*W+x)*2;
    s_fb[i] = c>>8; s_fb[i+1] = c&0xFF;
}
static void rect(int x,int y,int w,int h,uint16_t c,int f) {
    for (int j=y;j<y+h&&j<H;j++) for (int i=x;i<x+w&&i<W;i++) if(f||j==y||j==y+h-1||i==x||i==x+w-1) px(i,j,c);
}
static void circle(int cx,int cy,int r,uint16_t c,int f) {
    for (int y=cy-r;y<=cy+r;y++) for (int x=cx-r;x<=cx+r;x++) {
        int d=(x-cx)*(x-cx)+(y-cy)*(y-cy);
        if (f?d<=r*r:d>=(r-1)*(r-1)&&d<=(r+1)*(r+1)) px(x,y,c);
    }
}
static void clear(uint16_t c) {
    uint8_t hi=c>>8, lo=c&0xFF;
    for (int i=0;i<W*H;i++) { s_fb[i*2]=hi; s_fb[i*2+1]=lo; }
}
static void flush(void) {
    // Acquire bus — no other SPI devices can interrupt
    spi_device_acquire_bus(s_spi, portMAX_DELAY);

    // CASET: full visible width
    spi_cmd_hold(0x2A, true);
    spi_data_hold((uint8_t[]){0x00,0x00,0x00,0x7F}, 4, true);
    // RASET: GC9107 has 160 rows of internal RAM; visible 128 rows
    // start at offset 32. Write to rows 32..159.
    spi_cmd_hold(0x2B, true);
    spi_data_hold((uint8_t[]){0x00,LCD_ROW_OFFSET,0x00,LCD_ROW_OFFSET+H-1}, 4, true);
    spi_cmd_hold(0x2C, true);

    // Pixel data in one burst, CS de-asserted after last byte
    spi_data_hold(s_fb, W * H * 2, false);

    spi_device_release_bus(s_spi);
}
static void mic(int cx,int cy,int sz,uint16_t c) {
    int bw=sz*5/16, bh=sz*4/10, r=bw/2;
    if (bw<6) bw=6;
    int btx=cx-bw/2, bty=cy+r;
    circle(cx,cy-r/2,r,c,1);
    rect(btx,bty,bw,bh,c,1);
    int sw=bw/4; if(sw<2) sw=2;
    rect(cx-sw/2,bty+bh-2,sw,sz/8+2,c,1);
}

// ---- Screens ----
static void s_idle(void) {
    clear(C_DBLUE);
    // Large centered circle (looks like a record button)
    circle(64,55,25,RGB565(0,180,0),1);
    circle(64,55,15,RGB565(0,100,0),1);
    // BLE connection indicator: small green dot in top-right
    if (s_ble_connected) {
        circle(120, 8, 4, RGB565(0, 255, 0), 1);
        circle(120, 8, 3, RGB565(0, 200, 0), 1);
    }
}
static void s_rec(int ph) {
    clear(C_RED); mic(64,55,60,C_WHITE);
    for(int i=0;i<6;i++){ int bh=10+(ph+i*4)%25; rect(95,65-bh+i*5,6,bh,i%2?C_WHITE:RGB565(200,200,200),1); }
}
static void s_think(int ph) {
    clear(C_YELLOW_BG); mic(64,55,60,RGB565(0,0,0));
    for (int a = 0; a < 360; a += 30) {
        int d = (a / 30 + ph / 3) % 12;
        if (d < 6) {
            float r = a * (M_PI / 180.0f);
            int sx = 64 + (int)(35 * cosf(r));
            int sy = 50 + (int)(35 * sinf(r));
            circle(sx, sy, 3, RGB565(0, 0, 0), 1);
        }
    }
}

// ---- Animation ----
static void anim(void *p) {
    int ph = 0;
    TickType_t delay_ms = pdMS_TO_TICKS(100);
    while (1) {
        disp_state_t s = s_state;
        switch (s) {
        case DISP_STATE_IDLE:
            s_idle(); flush();
            delay_ms = pdMS_TO_TICKS(100);   // 10 fps — low CPU when idle
            break;

        case DISP_STATE_RECORDING:
            s_rec(ph++); flush();
            delay_ms = pdMS_TO_TICKS(60);    // ~16 fps
            break;

        case DISP_STATE_THINKING:
            s_think(ph++); flush();
            delay_ms = pdMS_TO_TICKS(50);    // 20 fps
            break;

        case DISP_STATE_CONFIRMED:
            clear(C_GREEN);
            for (int i = 0; i < 20; i++) {
                px(50 + i, 60 + i / 2, C_WHITE);
                px(50 + i, 61 + i / 2, C_WHITE);
                px(105 - i, 70 + i / 2, C_WHITE);
                px(105 - i, 71 + i / 2, C_WHITE);
            }
            flush(); vTaskDelay(pdMS_TO_TICKS(800));
            s_state = DISP_STATE_IDLE;
            continue;  // ← re-check state immediately, skip delay

        case DISP_STATE_CANCELLED:
            clear(C_RED);
            for (int i = 0; i < 40; i++) {
                px(44 + i, 44 + i, C_WHITE);
                px(84 - i, 44 + i, C_WHITE);
            }
            flush(); vTaskDelay(pdMS_TO_TICKS(800));
            s_state = DISP_STATE_IDLE;
            continue;

        case DISP_STATE_ERROR:
            for (int i = 0; i < 4; i++) {
                clear(i % 2 ? C_BLACK : C_RED);
                flush();
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            s_state = DISP_STATE_IDLE;
            continue;
        }
        vTaskDelay(delay_ms);
    }
}

// ---- API ----
esp_err_t display_init(void) {
    // FB: aligned DMA memory (16-byte alignment for S3 cache/DMA)
    s_fb = heap_caps_aligned_alloc(16, W*H*2, MALLOC_CAP_DMA);
    if (!s_fb) {
        ESP_LOGE(TAG, "failed to allocate framebuffer");
        return ESP_ERR_NO_MEM;
    }

    lp5562_init();

    // Verify backlight is on by re-applying brightness after setup
    if (s_lp5562) {
        display_set_brightness(31);  // ~31% (same as init value 0x50)
    } else {
        ESP_LOGW(TAG, "backlight not available — screen may appear dark");
    }

    spi_bus_config_t b = { .mosi_io_num=PIN_MOSI, .miso_io_num=-1,
        .sclk_io_num=PIN_SCLK, .quadwp_io_num=-1, .quadhd_io_num=-1,
        .max_transfer_sz=W*H*2 };
    spi_bus_initialize(LCD_HOST, &b, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t d = { .clock_speed_hz=20*1000*1000,
        .mode=0, .spics_io_num=PIN_CS, .queue_size=2, .pre_cb=spi_pre_cb };
    spi_bus_add_device(LCD_HOST, &d, &s_spi);

    gpio_set_direction(PIN_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT);

    lcd_init();

    // Clear + draw
    clear(C_BLACK); flush();
    s_idle(); flush();

    xTaskCreate(anim, "disp", 8192, NULL, 5, &s_anim);   // 8K — cosf/sinf need stack
    ESP_LOGI(TAG, "Display ready");
    return ESP_OK;
}

void display_set_ble_connected(bool connected) {
    s_ble_connected = connected;
}

void display_set_state(disp_state_t state) {
    static const char *names[] = {"IDLE","REC","THINK","OK","CANCEL","ERR"};
    ESP_LOGI(TAG, "state => %s", state < 6 ? names[state] : "?");
    s_state = state;
}
void display_set_brightness(uint8_t p) {
    if (!s_lp5562) return;
    i2c_master_transmit(s_lp5562, (uint8_t[]){0x0E, p>100?255:p*255/100}, 2, 100);
}
void display_deinit(void) {
    if (s_anim) vTaskDelete(s_anim);
    if (s_fb) free(s_fb);
}
