/**
 * @file screen_manager.c
 * @brief Screen navigation manager implementation
 */

#include "screen_manager.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SCREEN_MGR";

// Screen stack
static screen_t *screen_stack[MAX_SCREEN_STACK];
static int stack_depth = 0;

// Key callback forward declaration
static void key_event_handler(key_code_t key, bool pressed);

void screen_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing screen manager...");
    
    memset(screen_stack, 0, sizeof(screen_stack));
    stack_depth = 0;
    
    // Register for keyboard events
    keyboard_register_callback(key_event_handler);
    
    // Initialize UI
    ui_init();
    
    ESP_LOGI(TAG, "Screen manager initialized");
}

static void key_event_handler(key_code_t key, bool pressed)
{
    // Only handle key press, not release
    if (!pressed) return;
    
    screen_manager_handle_key(key);
}

void screen_manager_handle_key(key_code_t key)
{
    screen_t *current = screen_manager_get_current();
    
    // Always let the screen handle the key first
    if (current && current->on_key) {
        current->on_key(current, key);
    }
}

esp_err_t screen_manager_push(screen_create_fn create_fn, void *params)
{
    if (stack_depth >= MAX_SCREEN_STACK) {
        ESP_LOGE(TAG, "Screen stack overflow!");
        return ESP_FAIL;
    }
    
    if (!create_fn) {
        ESP_LOGE(TAG, "No create function provided");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Create new screen
    screen_t *new_screen = create_fn(params);
    if (!new_screen) {
        ESP_LOGE(TAG, "Failed to create screen");
        return ESP_FAIL;
    }
    
    // Push onto stack
    screen_stack[stack_depth++] = new_screen;
    
    ESP_LOGI(TAG, "Pushed screen, depth now: %d", stack_depth);
    return ESP_OK;
}

esp_err_t screen_manager_pop(void)
{
    if (stack_depth <= 1) {
        ESP_LOGW(TAG, "Cannot pop root screen");
        return ESP_FAIL;
    }
    
    // Get current screen
    screen_t *current = screen_stack[--stack_depth];
    
    // Destroy current screen
    if (current) {
        if (current->on_destroy) {
            current->on_destroy(current);
        }
        free(current);
    }
    
    // Redraw previous screen
    screen_t *prev = screen_manager_get_current();
    if (prev) {
        if (prev->on_resume) {
            prev->on_resume(prev);
        }
        if (prev->on_draw) {
            ui_clear();
            prev->on_draw(prev);
        }
    }
    
    ESP_LOGI(TAG, "Popped screen, depth now: %d", stack_depth);
    return ESP_OK;
}

esp_err_t screen_manager_replace(screen_create_fn create_fn, void *params)
{
    if (stack_depth < 1) {
        // No screen to replace, just push
        return screen_manager_push(create_fn, params);
    }
    
    // Get current screen
    screen_t *current = screen_stack[stack_depth - 1];
    
    // Create new screen first
    screen_t *new_screen = create_fn(params);
    if (!new_screen) {
        ESP_LOGE(TAG, "Failed to create replacement screen");
        return ESP_FAIL;
    }
    
    // Destroy old screen
    if (current) {
        if (current->on_destroy) {
            current->on_destroy(current);
        }
        free(current);
    }
    
    // Replace on stack
    screen_stack[stack_depth - 1] = new_screen;
    
    ESP_LOGI(TAG, "Replaced screen at depth: %d", stack_depth);
    return ESP_OK;
}

void screen_manager_pop_to_root(void)
{
    while (stack_depth > 1) {
        screen_manager_pop();
    }
}

screen_t* screen_manager_get_current(void)
{
    if (stack_depth > 0) {
        return screen_stack[stack_depth - 1];
    }
    return NULL;
}

int screen_manager_get_depth(void)
{
    return stack_depth;
}

screen_t* screen_alloc(void)
{
    screen_t *screen = calloc(1, sizeof(screen_t));
    if (!screen) {
        ESP_LOGE(TAG, "Failed to allocate screen");
        return NULL;
    }
    return screen;
}

void screen_manager_redraw(void)
{
    screen_t *current = screen_manager_get_current();
    if (current && current->on_draw) {
        current->on_draw(current);
    }
}

void screen_manager_tick(void)
{
    screen_t *current = screen_manager_get_current();
    if (current && current->on_tick) {
        current->on_tick(current);
    }
}
