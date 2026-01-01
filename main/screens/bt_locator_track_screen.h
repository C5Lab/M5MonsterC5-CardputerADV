/**
 * @file bt_locator_track_screen.h
 * @brief BT Locator tracking screen header
 */

#ifndef BT_LOCATOR_TRACK_SCREEN_H
#define BT_LOCATOR_TRACK_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Parameters for BT locator tracking screen
 */
typedef struct {
    char mac[18];    // MAC address to track
    char name[24];   // Device name (may be empty)
} bt_locator_track_params_t;

/**
 * @brief Create the BT Locator tracking screen
 * @param params Pointer to bt_locator_track_params_t
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* bt_locator_track_screen_create(void *params);

#endif // BT_LOCATOR_TRACK_SCREEN_H


