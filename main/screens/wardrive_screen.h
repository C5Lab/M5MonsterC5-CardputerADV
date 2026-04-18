/**
 * @file wardrive_screen.h
 * @brief Wardrive screen header
 */

#ifndef WARDRIVE_SCREEN_H
#define WARDRIVE_SCREEN_H

#include "screen_manager.h"
#include <stdbool.h>

typedef struct {
    bool trace;
} wardrive_run_params_t;

/**
 * @brief Create the Wardrive screen
 * @param params Optional wardrive_run_params_t*
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* wardrive_screen_create(void *params);

#endif // WARDRIVE_SCREEN_H










