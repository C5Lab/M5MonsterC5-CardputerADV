/**
 * @file screen_manager.h
 * @brief Screen navigation manager with stack-based navigation
 */

#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H

#include "esp_err.h"
#include "keyboard.h"
#include <stdbool.h>

// Maximum screen stack depth
#define MAX_SCREEN_STACK    8

// Forward declaration
typedef struct screen_t screen_t;

// Screen create function type
typedef screen_t* (*screen_create_fn)(void *params);

// Screen structure
struct screen_t {
    void *user_data;                        // User data for this screen
    void (*on_key)(screen_t *self, key_code_t key);  // Key event handler
    void (*on_destroy)(screen_t *self);     // Cleanup function
    void (*on_resume)(screen_t *self);      // Called when screen becomes active again
    void (*on_draw)(screen_t *self);        // Called to redraw the screen
    void (*on_tick)(screen_t *self);        // Called periodically from main loop
};

/**
 * @brief Initialize the screen manager
 */
void screen_manager_init(void);

/**
 * @brief Push a new screen onto the stack
 * @param create_fn Function to create the screen
 * @param params Parameters to pass to create function
 * @return ESP_OK on success
 */
esp_err_t screen_manager_push(screen_create_fn create_fn, void *params);

/**
 * @brief Pop the current screen and return to previous
 * @return ESP_OK on success, ESP_FAIL if at root screen
 */
esp_err_t screen_manager_pop(void);

/**
 * @brief Replace current screen with a new one (same stack level)
 * @param create_fn Function to create the screen
 * @param params Parameters to pass to create function
 * @return ESP_OK on success
 */
esp_err_t screen_manager_replace(screen_create_fn create_fn, void *params);

/**
 * @brief Pop all screens and return to root
 */
void screen_manager_pop_to_root(void);

/**
 * @brief Get the current active screen
 * @return Pointer to current screen, or NULL if none
 */
screen_t* screen_manager_get_current(void);

/**
 * @brief Get current stack depth
 * @return Number of screens on stack
 */
int screen_manager_get_depth(void);

/**
 * @brief Forward a key event to the current screen
 * @param key Key code
 */
void screen_manager_handle_key(key_code_t key);

/**
 * @brief Allocate a screen structure
 * @return Newly allocated screen, or NULL on failure
 */
screen_t* screen_alloc(void);

/**
 * @brief Request a screen redraw
 */
void screen_manager_redraw(void);

/**
 * @brief Call current screen's tick handler (for periodic updates)
 */
void screen_manager_tick(void);

#endif // SCREEN_MANAGER_H
