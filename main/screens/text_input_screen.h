/**
 * @file text_input_screen.h
 * @brief Reusable text input screen with keyboard support
 */

#ifndef TEXT_INPUT_SCREEN_H
#define TEXT_INPUT_SCREEN_H

#include "screen_manager.h"
#include <stddef.h>

// Maximum input length the buffer can hold (WPA2 passphrases need 63)
#define TEXT_INPUT_MAX_LEN 63

// Length applied when params->max_length is left at 0
#define TEXT_INPUT_DEFAULT_MAX_LEN 32

// Callback type for when text is submitted
typedef void (*text_input_callback_t)(const char *text, void *user_data);

// Parameters for text input screen
typedef struct {
    const char *title;              // Screen title
    const char *hint;               // Hint text below input
    text_input_callback_t on_submit; // Called when ENTER pressed
    void *user_data;                // Passed to callback
    bool allow_empty;               // Allow submitting empty input
    size_t max_length;              // 0 uses TEXT_INPUT_DEFAULT_MAX_LEN
    bool masked;                    // Render characters as '*' (for passwords)
} text_input_params_t;

/**
 * @brief Create the text input screen
 * @param params text_input_params_t pointer (takes ownership)
 * @return Screen instance
 */
screen_t* text_input_screen_create(void *params);

#endif // TEXT_INPUT_SCREEN_H










