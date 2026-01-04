/**
 * @file battery.h
 * @brief Battery level monitoring for M5Stack Cardputer-Adv
 * 
 * Uses ADC on GPIO10 to read battery voltage
 */

#ifndef BATTERY_H
#define BATTERY_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize battery monitoring
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_init(void);

/**
 * @brief Get battery level as percentage
 * @return Battery level 0-100%, or -1 if not available
 */
int battery_get_level(void);

/**
 * @brief Get raw battery voltage in millivolts
 * @return Voltage in mV, or -1 if not available
 */
int battery_get_voltage_mv(void);

/**
 * @brief Check if battery monitoring is available
 * @return true if initialized and working
 */
bool battery_is_available(void);

#endif // BATTERY_H




