/**
 * @file screenshot.h
 * @brief Screenshot functionality with SD card storage
 */

#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Initialize screenshot module (mounts SD card)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t screenshot_init(void);

/**
 * @brief Take a screenshot and save to SD card
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t screenshot_take(void);

/**
 * @brief Check if SD card is available
 * @return true if SD card is mounted
 */
bool screenshot_is_available(void);

#endif // SCREENSHOT_H




