/**
 * @file wardrive_upload_screen.h
 * @brief Wardrive upload screen (WiGLE/WDGWars)
 */

#ifndef WARDRIVE_UPLOAD_SCREEN_H
#define WARDRIVE_UPLOAD_SCREEN_H

#include "screen_manager.h"

typedef enum {
    WARDRIVE_UPLOAD_TARGET_WIGLE = 0,
    WARDRIVE_UPLOAD_TARGET_WDGWARS = 1
} wardrive_upload_target_t;

typedef struct {
    wardrive_upload_target_t target;
} wardrive_upload_params_t;

/**
 * @brief Create wardrive upload screen
 * @param params Optional wardrive_upload_params_t*
 * @return Screen instance
 */
screen_t* wardrive_upload_screen_create(void *params);

#endif // WARDRIVE_UPLOAD_SCREEN_H
