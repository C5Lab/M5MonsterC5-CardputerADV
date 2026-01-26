/**
 * @file buzzer.h
 * @brief Audio driver for M5Stack Cardputer ADV (ES8311 codec + NS4150B amp)
 */

#ifndef BUZZER_H
#define BUZZER_H

#include "esp_err.h"
#include <stdint.h>

// M5Stack Cardputer ADV I2S pins (from M5Unified)
// https://github.com/m5stack/M5Unified/blob/master/src/M5Unified.cpp
#define I2S_BCLK_PIN    41
#define I2S_LRCK_PIN    43
#define I2S_DOUT_PIN    42

// ES8311 uses same I2C bus as keyboard (I2C_NUM_0, SDA=8, SCL=9)
// ES8311 I2C address
#define ES8311_I2C_ADDR 0x18

// Speaker amplifier enable pin (active high)
#define SPEAKER_EN_PIN  46

/**
 * @brief Initialize the audio driver (ES8311 + I2S)
 * @return ESP_OK on success
 */
esp_err_t buzzer_init(void);

/**
 * @brief Play a tone at specified frequency for specified duration
 */
void buzzer_beep(uint32_t frequency_hz, uint32_t duration_ms);

/**
 * @brief Play a short beep when attack starts
 */
void buzzer_beep_attack(void);

/**
 * @brief Play a success beep
 */
void buzzer_beep_success(void);

/**
 * @brief Play a notification beep when data is captured
 */
void buzzer_beep_capture(void);

/**
 * @brief Stop any currently playing tone
 */
void buzzer_stop(void);

#endif // BUZZER_H
