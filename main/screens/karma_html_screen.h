/**
 * @file karma_html_screen.h
 * @brief Karma HTML portal selection screen header
 */

#ifndef KARMA_HTML_SCREEN_H
#define KARMA_HTML_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Parameters for karma HTML screen
 */
typedef struct {
    int probe_index;     // 1-based probe index for start_karma command
    char ssid[33];       // SSID name for display
} karma_html_params_t;

/**
 * @brief Create the karma HTML selection screen
 * @param params Pointer to karma_html_params_t
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* karma_html_screen_create(void *params);

#endif // KARMA_HTML_SCREEN_H


