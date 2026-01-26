/**
 * @file keyboard.h
 * @brief Keyboard driver for Cardputer matrix keyboard
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Key codes
typedef enum {
    KEY_NONE = 0,
    // Navigation keys
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    // Control keys
    KEY_ENTER,
    KEY_ESC,
    KEY_SPACE,
    KEY_BACKSPACE,
    KEY_TAB,
    // Letter keys
    KEY_A,
    KEY_B,
    KEY_C,
    KEY_D,
    KEY_E,
    KEY_F,
    KEY_G,
    KEY_H,
    KEY_I,
    KEY_J,
    KEY_K,
    KEY_L,
    KEY_M,
    KEY_N,
    KEY_O,
    KEY_P,
    KEY_Q,
    KEY_R,
    KEY_S,
    KEY_T,
    KEY_U,
    KEY_V,
    KEY_W,
    KEY_X,
    KEY_Y,
    KEY_Z,
    // Number keys
    KEY_0,
    KEY_1,
    KEY_2,
    KEY_3,
    KEY_4,
    KEY_5,
    KEY_6,
    KEY_7,
    KEY_8,
    KEY_9,
    // Symbol keys
    KEY_GRAVE,        // ` ~
    KEY_MINUS,        // - _
    KEY_EQUAL,        // = +
    KEY_LBRACKET,     // [ {
    KEY_RBRACKET,     // ] }
    KEY_BACKSLASH,    // \ |
    KEY_SEMICOLON,    // ; :
    KEY_APOSTROPHE,   // ' "
    KEY_COMMA,        // , <
    KEY_DOT,          // . >
    KEY_SLASH,        // / ?
    // Modifier keys
    KEY_SHIFT,
    KEY_CTRL,
    KEY_ALT,
    KEY_OPT,
    KEY_FN,
    KEY_CAPSLOCK,
    KEY_DEL,
    KEY_MAX
} key_code_t;

/**
 * @brief Check if shift key is currently held
 * @return true if shift is pressed
 */
bool keyboard_is_shift_held(void);

/**
 * @brief Check if ctrl key is currently held
 * @return true if ctrl is pressed
 */
bool keyboard_is_ctrl_held(void);

/**
 * @brief Check if capslock key is currently held
 * @return true if capslock is pressed
 */
bool keyboard_is_capslock_held(void);

/**
 * @brief Check if Fn key is currently held
 * @return true if Fn is pressed
 */
bool keyboard_is_fn_held(void);

/**
 * @brief Set text input mode
 * When enabled, arrow keys (,;./) require Fn to work as arrows
 * When disabled (default), arrow keys work without Fn (for menu navigation)
 * @param enabled true for text input mode, false for navigation mode
 */
void keyboard_set_text_input_mode(bool enabled);

// Key event callback type
typedef void (*key_event_callback_t)(key_code_t key, bool pressed);

/**
 * @brief Initialize the keyboard
 * @return ESP_OK on success
 */
esp_err_t keyboard_init(void);

/**
 * @brief Process keyboard input (call from main loop)
 */
void keyboard_process(void);

/**
 * @brief Register a callback for key events
 * @param callback Function to call on key events
 */
void keyboard_register_callback(key_event_callback_t callback);

/**
 * @brief Get last pressed key
 * @return Key code of last pressed key, or KEY_NONE
 */
key_code_t keyboard_get_key(void);

/**
 * @brief Check if a specific key is currently pressed
 * @param key Key to check
 * @return true if pressed
 */
bool keyboard_is_pressed(key_code_t key);

#endif // KEYBOARD_H



