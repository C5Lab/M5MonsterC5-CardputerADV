/**
 * @file bt_menu_screen.h
 * @brief Bluetooth menu screen header
 */

#ifndef BT_MENU_SCREEN_H
#define BT_MENU_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the Bluetooth menu screen
 * @param params Unused (pass NULL)
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* bt_menu_screen_create(void *params);

#endif // BT_MENU_SCREEN_H


