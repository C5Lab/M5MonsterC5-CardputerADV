/**
 * @file data_detail_screen.c
 * @brief Detail view screen for displaying full text content with scrolling
 * 
 * Displays a title (SSID) and full content with automatic line wrapping
 * and vertical scrolling support.
 */

#include "data_detail_screen.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "DATA_DETAIL";

// Display constants
#define CHARS_PER_LINE  28  // Leave some margin
#define CONTENT_ROWS    5   // Rows available for content (rows 1-5, row 0=title, row 7=status)
#define MAX_LINES       32  // Maximum wrapped lines to support

// Screen user data
typedef struct {
    char title[DETAIL_MAX_TITLE_LEN];
    char *lines[MAX_LINES];  // Pointers to wrapped lines
    int line_count;
    int scroll_offset;
} data_detail_data_t;

/**
 * @brief Wrap content into lines
 * Splits content by comma/space and wraps long segments
 */
static void wrap_content(data_detail_data_t *data, const char *content)
{
    data->line_count = 0;
    
    if (!content || strlen(content) == 0) {
        return;
    }
    
    // Make a working copy
    char *work = strdup(content);
    if (!work) return;
    
    const char *p = work;
    
    while (*p && data->line_count < MAX_LINES) {
        // Skip leading whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        
        // Find segment end (comma or end of string)
        const char *seg_start = p;
        const char *seg_end = p;
        
        // Look for comma separator or end
        while (*seg_end && *seg_end != ',') seg_end++;
        
        size_t seg_len = seg_end - seg_start;
        
        // Trim trailing whitespace from segment
        while (seg_len > 0 && (seg_start[seg_len-1] == ' ' || seg_start[seg_len-1] == '\t')) {
            seg_len--;
        }
        
        if (seg_len > 0) {
            // If segment fits on one line
            if (seg_len <= CHARS_PER_LINE) {
                char *line = malloc(seg_len + 1);
                if (line) {
                    strncpy(line, seg_start, seg_len);
                    line[seg_len] = '\0';
                    data->lines[data->line_count++] = line;
                }
            } else {
                // Need to wrap this segment
                size_t offset = 0;
                while (offset < seg_len && data->line_count < MAX_LINES) {
                    size_t chunk = seg_len - offset;
                    if (chunk > CHARS_PER_LINE) {
                        chunk = CHARS_PER_LINE;
                        // Try to break at space
                        size_t break_at = chunk;
                        for (size_t i = chunk; i > chunk/2; i--) {
                            if (seg_start[offset + i] == ' ') {
                                break_at = i;
                                break;
                            }
                        }
                        chunk = break_at;
                    }
                    
                    char *line = malloc(chunk + 1);
                    if (line) {
                        strncpy(line, seg_start + offset, chunk);
                        line[chunk] = '\0';
                        // Trim leading space on continuation
                        char *trimmed = line;
                        while (*trimmed == ' ') trimmed++;
                        if (trimmed != line) {
                            memmove(line, trimmed, strlen(trimmed) + 1);
                        }
                        if (strlen(line) > 0) {
                            data->lines[data->line_count++] = line;
                        } else {
                            free(line);
                        }
                    }
                    offset += chunk;
                }
            }
        }
        
        // Move past segment
        p = seg_end;
        if (*p == ',') {
            p++;  // Skip comma
            // Skip space after comma
            while (*p == ' ') p++;
        }
    }
    
    free(work);
}

static void draw_screen(screen_t *self)
{
    data_detail_data_t *data = (data_detail_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title (truncated if needed)
    char title_display[32];
    snprintf(title_display, sizeof(title_display), "%.28s", data->title);
    ui_draw_title(title_display);
    
    if (data->line_count == 0) {
        ui_print_center(3, "No data", UI_COLOR_DIMMED);
    } else {
        // Draw visible content lines
        for (int i = 0; i < CONTENT_ROWS; i++) {
            int line_idx = data->scroll_offset + i;
            int row = i + 1;  // Start from row 1 (after title)
            
            if (line_idx < data->line_count) {
                ui_print(0, row, data->lines[line_idx], UI_COLOR_TEXT);
            }
        }
        
        // Scroll indicators
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + CONTENT_ROWS < data->line_count) {
            ui_print(UI_COLS - 2, CONTENT_ROWS, "v", UI_COLOR_DIMMED);
        }
    }
    
    // Draw status bar
    if (data->line_count > CONTENT_ROWS) {
        ui_draw_status("UP/DOWN:Scroll ESC:Back");
    } else {
        ui_draw_status("ESC:Back");
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    data_detail_data_t *data = (data_detail_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->scroll_offset > 0) {
                data->scroll_offset--;
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->scroll_offset + CONTENT_ROWS < data->line_count) {
                data->scroll_offset++;
                draw_screen(self);
            }
            break;
            
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    data_detail_data_t *data = (data_detail_data_t *)self->user_data;
    
    if (data) {
        // Free all wrapped lines
        for (int i = 0; i < data->line_count; i++) {
            if (data->lines[i]) {
                free(data->lines[i]);
            }
        }
        free(data);
    }
}

screen_t* data_detail_screen_create(void *params)
{
    data_detail_params_t *detail_params = (data_detail_params_t *)params;
    
    if (!detail_params) {
        ESP_LOGE(TAG, "No parameters provided");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating data detail screen for '%s'...", detail_params->title);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(detail_params);
        return NULL;
    }
    
    data_detail_data_t *data = calloc(1, sizeof(data_detail_data_t));
    if (!data) {
        free(screen);
        free(detail_params);
        return NULL;
    }
    
    // Copy title
    strncpy(data->title, detail_params->title, DETAIL_MAX_TITLE_LEN - 1);
    data->title[DETAIL_MAX_TITLE_LEN - 1] = '\0';
    
    // Wrap content into lines
    wrap_content(data, detail_params->content);
    
    // Free params (we've copied what we need)
    free(detail_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Data detail screen created with %d lines", data->line_count);
    return screen;
}



