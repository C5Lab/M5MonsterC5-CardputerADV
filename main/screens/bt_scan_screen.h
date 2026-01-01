/**
 * @file bt_scan_screen.h
 * @brief BT scan screen header
 */

#ifndef BT_SCAN_SCREEN_H
#define BT_SCAN_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the BT scan screen
 * @param params Unused (pass NULL)
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* bt_scan_screen_create(void *params);

#endif // BT_SCAN_SCREEN_H


