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
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_ESC,
    KEY_SPACE,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_Q,
    KEY_A,
    KEY_Z,
    KEY_W,
    KEY_S,
    KEY_X,
    KEY_E,
    KEY_D,
    KEY_C,
    KEY_R,
    KEY_F,
    KEY_V,
    KEY_T,
    KEY_G,
    KEY_B,
    KEY_Y,
    KEY_H,
    KEY_N,
    KEY_U,
    KEY_J,
    KEY_M,
    KEY_I,
    KEY_K,
    KEY_O,
    KEY_L,
    KEY_P,
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
    KEY_FN,
    KEY_OPT,
    KEY_ALT,
    KEY_DEL,
    KEY_SHIFT,
    KEY_MAX
} key_code_t;

/**
 * @brief Check if shift key is currently held
 * @return true if shift is pressed
 */
bool keyboard_is_shift_held(void);

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



