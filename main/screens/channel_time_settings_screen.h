/**
 * @file channel_time_settings_screen.h
 * @brief Channel time (min/max scan time per channel) settings screen
 */

#ifndef CHANNEL_TIME_SETTINGS_SCREEN_H
#define CHANNEL_TIME_SETTINGS_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the channel time settings screen
 * @param params Unused
 * @return Screen instance
 */
screen_t* channel_time_settings_screen_create(void *params);

#endif // CHANNEL_TIME_SETTINGS_SCREEN_H
