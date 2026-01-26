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

#endif // SETTINGS_H


