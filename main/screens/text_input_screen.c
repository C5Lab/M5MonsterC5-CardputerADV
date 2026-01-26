/**
 * @file text_input_screen.c
 * @brief Reusable text input screen with keyboard support
 */

#include "text_input_screen.h"
#include "text_ui.h"
#include "keyboard.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TEXT_INPUT";

// Screen user data
typedef struct {
    char title[32];
    char hint[48];
    char input[TEXT_INPUT_MAX_LEN + 1];
    int cursor_pos;
    text_input_callback_t on_submit;
    void *callback_user_data;
} text_input_data_t;

/**
 * @brief Convert key code to character
 * Returns lowercase by default, uppercase/special when shift or capslock held
 * Based on M5Stack official keyboard layout
 */
static char key_to_char(key_code_t key)
{
    bool shift = keyboard_is_shift_held();
    bool caps = keyboard_is_capslock_held();
    
    // For letters: shift XOR capslock for uppercase
    bool upper = shift ^ caps;
    
    // Letters A-Z - lowercase by default, uppercase with shift or capslock
    switch (key) {
        case KEY_Q: return upper ? 'Q' : 'q';
        case KEY_W: return upper ? 'W' : 'w';
        case KEY_E: return upper ? 'E' : 'e';
        case KEY_R: return upper ? 'R' : 'r';
        case KEY_T: return upper ? 'T' : 't';
        case KEY_Y: return upper ? 'Y' : 'y';
        case KEY_U: return upper ? 'U' : 'u';
        case KEY_I: return upper ? 'I' : 'i';
        case KEY_O: return upper ? 'O' : 'o';
        case KEY_P: return upper ? 'P' : 'p';
        case KEY_A: return upper ? 'A' : 'a';
        case KEY_S: return upper ? 'S' : 's';
        case KEY_D: return upper ? 'D' : 'd';
        case KEY_F: return upper ? 'F' : 'f';
        case KEY_G: return upper ? 'G' : 'g';
        case KEY_H: return upper ? 'H' : 'h';
        case KEY_J: return upper ? 'J' : 'j';
        case KEY_K: return upper ? 'K' : 'k';
        case KEY_L: return upper ? 'L' : 'l';
        case KEY_Z: return upper ? 'Z' : 'z';
        case KEY_X: return upper ? 'X' : 'x';
        case KEY_C: return upper ? 'C' : 'c';
        case KEY_V: return upper ? 'V' : 'v';
        case KEY_B: return upper ? 'B' : 'b';
        case KEY_N: return upper ? 'N' : 'n';
        case KEY_M: return upper ? 'M' : 'm';
        
        // Numbers - special chars with shift only (not capslock)
        case KEY_1: return shift ? '!' : '1';
        case KEY_2: return shift ? '@' : '2';
        case KEY_3: return shift ? '#' : '3';
        case KEY_4: return shift ? '$' : '4';
        case KEY_5: return shift ? '%' : '5';
        case KEY_6: return shift ? '^' : '6';
        case KEY_7: return shift ? '&' : '7';
        case KEY_8: return shift ? '*' : '8';
        case KEY_9: return shift ? '(' : '9';
        case KEY_0: return shift ? ')' : '0';
        
        // Symbol keys - special chars with shift only
        case KEY_GRAVE:      return shift ? '~' : '`';
        case KEY_MINUS:      return shift ? '_' : '-';
        case KEY_EQUAL:      return shift ? '+' : '=';
        case KEY_LBRACKET:   return shift ? '{' : '[';
        case KEY_RBRACKET:   return shift ? '}' : ']';
        case KEY_BACKSLASH:  return shift ? '|' : '\\';
        case KEY_SEMICOLON:  return shift ? ':' : ';';
        case KEY_APOSTROPHE: return shift ? '"' : '\'';
        case KEY_COMMA:      return shift ? '<' : ',';
        case KEY_DOT:        return shift ? '>' : '.';
        case KEY_SLASH:      return shift ? '?' : '/';
        
        case KEY_SPACE: return ' ';
        case KEY_TAB:   return '\t';
        
        default: break;
    }
    
    return 0;  // No valid character
}

static void draw_screen(screen_t *self)
{
    text_input_data_t *data = (text_input_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title(data->title);
    
    int row = 2;
    
    // Draw input field with cursor
    char display[TEXT_INPUT_MAX_LEN + 2];
    snprintf(display, sizeof(display), "%s_", data->input);
    ui_print(0, row, display, UI_COLOR_HIGHLIGHT);
    row += 2;
    
    // Draw hint
    if (data->hint[0]) {
        ui_print(0, row, data->hint, UI_COLOR_DIMMED);
    }
    
    // Draw status bar
    ui_draw_status("ENTER:OK ESC:Cancel");
}

static void on_key(screen_t *self, key_code_t key)
{
    text_input_data_t *data = (text_input_data_t *)self->user_data;
    
    switch (key) {
        case KEY_ENTER:
            // Submit if we have input
            if (data->cursor_pos > 0 && data->on_submit) {
                data->on_submit(data->input, data->callback_user_data);
            }
            break;
            
        case KEY_ESC:
            // Cancel - just pop
            screen_manager_pop();
            break;
            
        case KEY_BACKSPACE:
        case KEY_DEL:
            // Delete last character
            if (data->cursor_pos > 0) {
                data->cursor_pos--;
                data->input[data->cursor_pos] = '\0';
                draw_screen(self);
            }
            break;
            
        default:
            {
                // Try to add character
                char ch = key_to_char(key);
                if (ch && data->cursor_pos < TEXT_INPUT_MAX_LEN) {
                    data->input[data->cursor_pos++] = ch;
                    data->input[data->cursor_pos] = '\0';
                    draw_screen(self);
                }
            }
            break;
    }
}

static void on_destroy(screen_t *self)
{
    // Disable text input mode when leaving
    keyboard_set_text_input_mode(false);
    
    if (self->user_data) {
        free(self->user_data);
    }
}

screen_t* text_input_screen_create(void *params)
{
    text_input_params_t *input_params = (text_input_params_t *)params;
    
    if (!input_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating text input screen: %s", input_params->title ? input_params->title : "");
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(input_params);
        return NULL;
    }
    
    // Allocate user data
    text_input_data_t *data = calloc(1, sizeof(text_input_data_t));
    if (!data) {
        free(screen);
        free(input_params);
        return NULL;
    }
    
    // Copy parameters
    if (input_params->title) {
        strncpy(data->title, input_params->title, sizeof(data->title) - 1);
    }
    if (input_params->hint) {
        strncpy(data->hint, input_params->hint, sizeof(data->hint) - 1);
    }
    data->on_submit = input_params->on_submit;
    data->callback_user_data = input_params->user_data;
    data->cursor_pos = 0;
    data->input[0] = '\0';
    
    free(input_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Enable text input mode - arrows require Fn, keys produce characters
    keyboard_set_text_input_mode(true);
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Text input screen created");
    return screen;
}

