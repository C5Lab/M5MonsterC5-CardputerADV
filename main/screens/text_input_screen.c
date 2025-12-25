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
 * Returns lowercase by default, uppercase/special when shift held
 */
static char key_to_char(key_code_t key)
{
    bool shift = keyboard_is_shift_held();
    
    // Letters A-Z - lowercase by default, uppercase with shift
    switch (key) {
        case KEY_Q: return shift ? 'Q' : 'q';
        case KEY_W: return shift ? 'W' : 'w';
        case KEY_E: return shift ? 'E' : 'e';
        case KEY_R: return shift ? 'R' : 'r';
        case KEY_T: return shift ? 'T' : 't';
        case KEY_Y: return shift ? 'Y' : 'y';
        case KEY_U: return shift ? 'U' : 'u';
        case KEY_I: return shift ? 'I' : 'i';
        case KEY_O: return shift ? 'O' : 'o';
        case KEY_P: return shift ? 'P' : 'p';
        case KEY_A: return shift ? 'A' : 'a';
        case KEY_S: return shift ? 'S' : 's';
        case KEY_D: return shift ? 'D' : 'd';
        case KEY_F: return shift ? 'F' : 'f';
        case KEY_G: return shift ? 'G' : 'g';
        case KEY_H: return shift ? 'H' : 'h';
        case KEY_J: return shift ? 'J' : 'j';
        case KEY_K: return shift ? 'K' : 'k';
        case KEY_L: return shift ? 'L' : 'l';
        case KEY_Z: return shift ? 'Z' : 'z';
        case KEY_X: return shift ? 'X' : 'x';
        case KEY_C: return shift ? 'C' : 'c';
        case KEY_V: return shift ? 'V' : 'v';
        case KEY_B: return shift ? 'B' : 'b';
        case KEY_N: return shift ? 'N' : 'n';
        case KEY_M: return shift ? 'M' : 'm';
        
        // Numbers - special chars with shift
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
        
        case KEY_SPACE: return ' ';
        
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
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Text input screen created");
    return screen;
}

