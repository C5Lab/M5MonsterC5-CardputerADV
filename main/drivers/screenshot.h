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

/**
 * Ensure SPI3 is initialized for the shared SD + CC1101 Cap bus.
 * Safe to call more than once. Holds the SX1262 in reset on GPIO5.
 */
esp_err_t screenshot_ensure_spi_bus(void);

/** Nested-safe SPI3 lock for SD vs CC1101. */
void screenshot_spi_acquire(void);
void screenshot_spi_release(void);

#endif // SCREENSHOT_H








