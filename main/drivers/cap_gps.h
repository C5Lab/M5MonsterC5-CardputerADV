/**
 * @file cap_gps.h
 * @brief LoRa CAP GPS driver - reads NMEA from CAP device via UART2
 */

#ifndef CAP_GPS_H
#define CAP_GPS_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize CAP GPS UART2 and start reading task
 * @return ESP_OK on success
 */
esp_err_t cap_gps_init(void);

/**
 * @brief Stop reading task and deinit UART2
 */
void cap_gps_deinit(void);

/**
 * @brief Check if CAP GPS has a valid fix
 * @return true if fix is valid and recent (within 2s)
 */
bool cap_gps_has_fix(void);

/**
 * @brief Get current GPS position (decimal degrees)
 * @param lat Output latitude
 * @param lon Output longitude
 * @param alt Output altitude in meters
 * @param hdop Output HDOP (horizontal dilution of precision)
 * @return true if position is valid
 */
bool cap_gps_get_position(double *lat, double *lon, double *alt, double *hdop);

/**
 * @brief Get satellite count
 * @return Number of satellites in view, -1 if no data
 */
int cap_gps_get_satellites(void);

#endif // CAP_GPS_H
