/**
 * @file display.c
 * @brief Simple ST7789 display driver for Cardputer
 */

#include "display.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include <string.h>

static const char *TAG = "DISPLAY";

// LEDC PWM configuration for backlight
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#if defined(BOARD_K132)
// Match M5GFX's Cardputer backlight frequency; validate dimming on K132 hardware.
#define BL_LEDC_FREQ_HZ     256
#else
#define BL_LEDC_FREQ_HZ     5000
#endif
#define BL_LEDC_RESOLUTION  LEDC_TIMER_13_BIT  // 0-8191 duty (13-bit for fine control)

#define BL_DUTY_MAX         8191
#if defined(BOARD_K132)
// At 256 Hz, use a ~6.25% floor based on M5GFX's Cardputer offset of 16/256.
// The legacy ~88% floor leaves almost no dimming range at this frequency.
#define BL_DUTY_MIN         512
#else
// Preserve the existing ADV mapping at 5 kHz.
#define BL_DUTY_MIN         7200
#endif

static esp_lcd_panel_handle_t panel_handle = NULL;

// Small line buffer for drawing operations
#define LINE_BUFFER_SIZE (DISPLAY_WIDTH * 2)  // One line in RGB565
static uint16_t line_buffer[DISPLAY_WIDTH];

// Framebuffer for screenshot functionality
static uint16_t framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing ST7789 display...");

    // Configure backlight via LEDC PWM for brightness control
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = BL_LEDC_MODE,
        .timer_num        = BL_LEDC_TIMER,
        .duty_resolution  = BL_LEDC_RESOLUTION,
        .freq_hz          = BL_LEDC_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = BL_LEDC_MODE,
        .channel        = BL_LEDC_CHANNEL,
        .timer_sel      = BL_LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = DISPLAY_PIN_BL,
        .duty           = BL_DUTY_MAX,  // Start at full brightness
        .hpoint         = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .sclk_io_num = DISPLAY_PIN_SCLK,
        .mosi_io_num = DISPLAY_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Configure LCD panel IO
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DISPLAY_PIN_DC,
        .cs_gpio_num = DISPLAY_PIN_CS,
        .pclk_hz = DISPLAY_SPI_FREQ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_config, &io_handle));

    // Configure LCD panel - BGR order for ST7789
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    // Initialize panel
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    // Cardputer ST7789 display orientation
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    
    // Set display offset
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y));
    
    // Turn on display
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // Clear screen to black
    display_clear(COLOR_BLACK);

    ESP_LOGI(TAG, "Display initialized successfully (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return ESP_OK;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0 || y < 0 || x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT) {
        // Clip to bounds
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > DISPLAY_WIDTH) w = DISPLAY_WIDTH - x;
        if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
    }
    
    if (w <= 0 || h <= 0) return;

    // Swap bytes for ST7789 (big-endian)
    uint16_t swapped = ((color >> 8) & 0xFF) | ((color & 0xFF) << 8);
    
    // Fill line buffer
    for (int i = 0; i < w && i < DISPLAY_WIDTH; i++) {
        line_buffer[i] = swapped;
    }
    
    // Draw line by line and update framebuffer
    for (int row = y; row < y + h; row++) {
        esp_lcd_panel_draw_bitmap(panel_handle, x, row, x + w, row + 1, line_buffer);
        // Update framebuffer (store original color, not swapped)
        for (int col = x; col < x + w; col++) {
            framebuffer[row * DISPLAY_WIDTH + col] = color;
        }
    }
}

void display_draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    
    uint16_t swapped = ((color >> 8) & 0xFF) | ((color & 0xFF) << 8);
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 1, y + 1, &swapped);
    // Update framebuffer
    framebuffer[y * DISPLAY_WIDTH + x] = color;
}

void display_draw_hline(int x, int y, int w, uint16_t color)
{
    display_fill_rect(x, y, w, 1, color);
}

void display_draw_vline(int x, int y, int h, uint16_t color)
{
    display_fill_rect(x, y, 1, h, color);
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    display_draw_hline(x, y, w, color);
    display_draw_hline(x, y + h - 1, w, color);
    display_draw_vline(x, y, h, color);
    display_draw_vline(x + w - 1, y, h, color);
}

void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

void display_set_backlight(uint8_t brightness)
{
    // Map 0% to off, 1-100% to the board's duty range (BL_DUTY_MIN..BL_DUTY_MAX).
    uint32_t duty = 0;
    if (brightness > 0) {
        if (brightness >= 100) {
            duty = BL_DUTY_MAX;
        } else {
            duty = BL_DUTY_MIN + (uint32_t)(brightness - 1) * (BL_DUTY_MAX - BL_DUTY_MIN) / 99;
        }
    }
#if defined(BOARD_K132)
    esp_err_t ret = ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "K132 backlight: set duty failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "K132 backlight: update duty failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "K132 backlight: brightness=%u%%, requested duty=%lu/%u, PWM=%lu Hz",
             (unsigned)brightness, (unsigned long)duty, (unsigned)BL_DUTY_MAX,
             (unsigned long)ledc_get_freq(BL_LEDC_MODE, BL_LEDC_TIMER));
#else
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
#endif
}

void display_flush(void)
{
    // No-op for direct drawing mode
}

const uint16_t* display_get_framebuffer(void)
{
    return framebuffer;
}
