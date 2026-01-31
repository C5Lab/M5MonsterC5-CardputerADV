/**
 * @file gps_raw_screen.h
 * @brief GPS raw output screen
 */

#ifndef GPS_RAW_SCREEN_H
#define GPS_RAW_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the GPS raw output screen
 * @param params Unused
 * @return Screen instance
 */
screen_t* gps_raw_screen_create(void *params);

#endif // GPS_RAW_SCREEN_H
