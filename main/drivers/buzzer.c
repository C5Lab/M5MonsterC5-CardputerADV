/**
 * @file buzzer.c
 * @brief Audio driver for M5Stack Cardputer ADV (ES8311 codec)
 * Based on M5Unified configuration:
 * https://github.com/m5stack/M5Unified/blob/master/src/M5Unified.cpp
 */

#include "buzzer.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "BUZZER";

#define I2S_SAMPLE_RATE     48000
#define TONE_AMPLITUDE      6000
#define CHUNK_FRAMES        512   // Fixed buffer size

#define ES8311_I2C_PORT     I2C_NUM_0

static i2s_chan_handle_t tx_handle = NULL;
static bool buzzer_initialized = false;

// Fixed buffer to avoid malloc during beep
static int16_t audio_buffer[CHUNK_FRAMES * 2];

static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    esp_err_t ret = i2c_master_write_to_device(ES8311_I2C_PORT, ES8311_I2C_ADDR, 
                                                data, sizeof(data), pdMS_TO_TICKS(100));
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "ES8311 reg 0x%02X = 0x%02X", reg, val);
    }
    return ret;
}

static esp_err_t es8311_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(ES8311_I2C_PORT, ES8311_I2C_ADDR,
                                         &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static esp_err_t es8311_init_codec(void)
{
    uint8_t chip_id = 0;
    
    esp_err_t ret = es8311_read_reg(0xFD, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 not responding");
        return ret;
    }
    ESP_LOGI(TAG, "ES8311 chip ID: 0x%02X", chip_id);
    
    // Configuration from M5Unified _speaker_enabled_cb_cardputer_adv
    es8311_write_reg(0x00, 0x80);  // RESET/CSM POWER ON
    vTaskDelay(pdMS_TO_TICKS(10));
    
    es8311_write_reg(0x01, 0xB5);  // CLOCK_MANAGER/ MCLK=BCLK
    es8311_write_reg(0x02, 0x18);  // CLOCK_MANAGER/ MULT_PRE=3
    es8311_write_reg(0x0D, 0x01);  // SYSTEM/ Power up analog circuitry
    es8311_write_reg(0x12, 0x00);  // SYSTEM/ power-up DAC
    es8311_write_reg(0x13, 0x10);  // SYSTEM/ Enable output to HP drive
    es8311_write_reg(0x32, 0xBF);  // DAC/ DAC volume (0xBF == ±0 dB)
    es8311_write_reg(0x37, 0x08);  // DAC/ Bypass DAC equalizer
    
    ESP_LOGI(TAG, "ES8311 codec initialized");
    return ESP_OK;
}

static esp_err_t init_speaker_enable(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SPEAKER_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(SPEAKER_EN_PIN, 1);
    ESP_LOGI(TAG, "Speaker amplifier enabled (GPIO %d)", SPEAKER_EN_PIN);
    return ESP_OK;
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_LRCK_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S std mode: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return ret;
    }
    
    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return ret;
    }
    
    ESP_LOGI(TAG, "I2S initialized (BCLK=%d, WS=%d, DOUT=%d)", 
             I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
    return ESP_OK;
}

esp_err_t buzzer_init(void)
{
    ESP_LOGI(TAG, "Initializing Cardputer ADV audio...");
    
    init_speaker_enable();
    vTaskDelay(pdMS_TO_TICKS(50));
    
    esp_err_t ret = init_i2s();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(20));
    
    ret = es8311_init_codec();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 init failed");
    }
    
    // Prime DMA buffers with silence
    memset(audio_buffer, 0, sizeof(audio_buffer));
    size_t bytes_written;
    i2s_channel_write(tx_handle, audio_buffer, sizeof(audio_buffer), &bytes_written, 100);
    
    buzzer_initialized = true;
    ESP_LOGI(TAG, "Audio initialized");
    
    // Test beep
    buzzer_beep(1000, 100);
    
    return ESP_OK;
}

void buzzer_beep(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (!buzzer_initialized || !tx_handle) {
        return;
    }
    
    if (frequency_hz < 100) frequency_hz = 100;
    if (frequency_hz > 8000) frequency_hz = 8000;
    
    ESP_LOGI(TAG, "Beep: %lu Hz, %lu ms", (unsigned long)frequency_hz, (unsigned long)duration_ms);
    
    uint32_t total_frames = (I2S_SAMPLE_RATE * duration_ms) / 1000;
    uint32_t frames_done = 0;
    size_t bytes_written;
    
    // Generate and play in chunks using fixed buffer
    while (frames_done < total_frames) {
        uint32_t chunk_frames = (total_frames - frames_done > CHUNK_FRAMES) 
                               ? CHUNK_FRAMES : (total_frames - frames_done);
        
        // Generate sine wave chunk
        for (uint32_t i = 0; i < chunk_frames; i++) {
            uint32_t global_i = frames_done + i;
            float amp = TONE_AMPLITUDE;
            
            // Fade out at end
            if (global_i >= total_frames - 200 && total_frames > 200) {
                amp *= (float)(total_frames - global_i) / 200.0f;
            }
            
            int16_t val = (int16_t)(amp * sinf(2.0f * M_PI * frequency_hz * global_i / I2S_SAMPLE_RATE));
            audio_buffer[i * 2] = val;
            audio_buffer[i * 2 + 1] = val;
        }
        
        // Write with short timeout to avoid blocking too long
        i2s_channel_write(tx_handle, audio_buffer, chunk_frames * 4, &bytes_written, pdMS_TO_TICKS(50));
        frames_done += chunk_frames;
    }
    
    // Short silence at end
    memset(audio_buffer, 0, 256 * 4);
    i2s_channel_write(tx_handle, audio_buffer, 256 * 4, &bytes_written, pdMS_TO_TICKS(20));
}

void buzzer_beep_attack(void)
{
    buzzer_beep(2000, 80);
}

void buzzer_beep_success(void)
{
    buzzer_beep(1000, 100);
    vTaskDelay(pdMS_TO_TICKS(30));
    buzzer_beep(1500, 150);
}

void buzzer_beep_capture(void)
{
    buzzer_beep(1200, 60);
}

void buzzer_stop(void)
{
    if (!buzzer_initialized || !tx_handle) return;
    memset(audio_buffer, 0, sizeof(audio_buffer));
    size_t bytes_written;
    i2s_channel_write(tx_handle, audio_buffer, sizeof(audio_buffer), &bytes_written, 0);
}
