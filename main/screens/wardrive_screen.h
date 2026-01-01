/**
 * @file wardrive_screen.h
 * @brief Wardrive screen header
 */

#ifndef WARDRIVE_SCREEN_H
#define WARDRIVE_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the Wardrive screen
 * @param params Unused (pass NULL)
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* wardrive_screen_create(void *params);

#endif // WARDRIVE_SCREEN_H


