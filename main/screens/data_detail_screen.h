/**
 * @file data_detail_screen.h
 * @brief Detail view screen for displaying full text content with scrolling
 */

#ifndef DATA_DETAIL_SCREEN_H
#define DATA_DETAIL_SCREEN_H

#include "screen_manager.h"

// Maximum lengths for parameters
#define DETAIL_MAX_TITLE_LEN    64
#define DETAIL_MAX_CONTENT_LEN  256

// Parameters for detail screen
typedef struct {
    char title[DETAIL_MAX_TITLE_LEN];      // SSID or header
    char content[DETAIL_MAX_CONTENT_LEN];  // Full content to display
} data_detail_params_t;

/**
 * @brief Create the data detail screen
 * @param params Pointer to data_detail_params_t (will be freed by screen)
 * @return Created screen or NULL on failure
 */
screen_t* data_detail_screen_create(void *params);

#endif // DATA_DETAIL_SCREEN_H



