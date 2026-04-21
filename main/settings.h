/**
 * @file settings.h
 * @brief Application settings stored in NVS
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Default UART pin values
#define DEFAULT_UART_TX_PIN     2
#define DEFAULT_UART_RX_PIN     1

// Default screen settings
#define DEFAULT_SCREEN_TIMEOUT_MS   30000   // 30 seconds
#define DEFAULT_SCREEN_BRIGHTNESS   100     // 100%
#define DEFAULT_SOUND_ENABLED       true

// Valid GPIO pin range for ESP32-S3
#define MIN_GPIO_PIN            0
#define MAX_GPIO_PIN            48

/**
 * @brief Initialize settings module and NVS
 * @return ESP_OK on success
 */
esp_err_t settings_init(void);

/**
 * @brief Get UART TX pin
 * @return GPIO pin number for UART TX
 */
int settings_get_uart_tx_pin(void);

/**
 * @brief Get UART RX pin
 * @return GPIO pin number for UART RX
 */
int settings_get_uart_rx_pin(void);

/**
 * @brief Set UART pins with validation
 * @param tx_pin TX GPIO pin number
 * @param rx_pin RX GPIO pin number
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if pin is invalid
 */
esp_err_t settings_set_uart_pins(int tx_pin, int rx_pin);

/**
 * @brief Check if a GPIO pin number is valid for UART
 * @param pin GPIO pin number
 * @return true if valid, false otherwise
 */
bool settings_is_valid_gpio_pin(int pin);

/**
 * @brief Get Red Team mode enabled status
 * @return true if Red Team features are enabled
 */
bool settings_get_red_team_enabled(void);

/**
 * @brief Set Red Team mode enabled status
 * @param enabled true to enable Red Team features
 * @return ESP_OK on success
 */
esp_err_t settings_set_red_team_enabled(bool enabled);

/**
 * @brief Get screen dimming timeout in milliseconds
 * @return Timeout in ms (0 = stays on, never dims)
 */
uint32_t settings_get_screen_timeout_ms(void);

/**
 * @brief Set screen dimming timeout
 * @param timeout_ms Timeout in ms (0 = stays on)
 * @return ESP_OK on success
 */
esp_err_t settings_set_screen_timeout_ms(uint32_t timeout_ms);

/**
 * @brief Get screen brightness percentage
 * @return Brightness 1-100
 */
uint8_t settings_get_screen_brightness(void);

/**
 * @brief Set screen brightness percentage
 * @param brightness 1-100
 * @return ESP_OK on success
 */
esp_err_t settings_set_screen_brightness(uint8_t brightness);

/**
 * @brief Get sound enabled status
 * @return true if UI sounds are enabled
 */
bool settings_get_sound_enabled(void);

/**
 * @brief Set sound enabled status
 * @param enabled true to enable UI sounds
 * @return ESP_OK on success
 */
esp_err_t settings_set_sound_enabled(bool enabled);

// GPS module types
typedef enum {
    GPS_TYPE_ATGM = 0,
    GPS_TYPE_M5 = 1,
    GPS_TYPE_CAP = 2,
} gps_type_t;

/**
 * @brief Get selected GPS module type
 * @return GPS module type
 */
gps_type_t settings_get_gps_type(void);

/**
 * @brief Set GPS module type
 * @param type GPS module type
 * @return ESP_OK on success
 */
esp_err_t settings_set_gps_type(gps_type_t type);

#endif // SETTINGS_H


