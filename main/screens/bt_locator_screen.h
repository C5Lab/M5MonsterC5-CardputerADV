/**
 * @file bt_locator_screen.h
 * @brief BT Locator device selection screen header
 */

#ifndef BT_LOCATOR_SCREEN_H
#define BT_LOCATOR_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the BT Locator device selection screen
 * @param params Unused (pass NULL)
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* bt_locator_screen_create(void *params);

#endif // BT_LOCATOR_SCREEN_H


