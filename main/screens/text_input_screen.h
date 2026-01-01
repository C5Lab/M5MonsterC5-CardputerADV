/**
 * @file text_input_screen.h
 * @brief Reusable text input screen with keyboard support
 */

#ifndef TEXT_INPUT_SCREEN_H
#define TEXT_INPUT_SCREEN_H

#include "screen_manager.h"

// Maximum input length
#define TEXT_INPUT_MAX_LEN 32

// Callback type for when text is submitted
typedef void (*text_input_callback_t)(const char *text, void *user_data);

// Parameters for text input screen
typedef struct {
    const char *title;              // Screen title
    const char *hint;               // Hint text below input
    text_input_callback_t on_submit; // Called when ENTER pressed
    void *user_data;                // Passed to callback
} text_input_params_t;

/**
 * @brief Create the text input screen
 * @param params text_input_params_t pointer (takes ownership)
 * @return Screen instance
 */
screen_t* text_input_screen_create(void *params);

#endif // TEXT_INPUT_SCREEN_H


