/**
 * @file text_ui.c
 * @brief Simple text-based UI implementation
 */

#include "text_ui.h"
#include "font8x16.h"
#include "battery.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

// Battery voltage cache (refresh every 30 seconds)
#define BATTERY_CACHE_INTERVAL_US   (30 * 1000000)  // 30 seconds in microseconds
static int cached_voltage_mv = 0;
static int cached_level = 0;
static int64_t last_battery_read_time = 0;

/**
 * @brief Update cached battery values if interval has passed
 */
static void update_battery_cache(void)
{
    int64_t now = esp_timer_get_time();
    if (cached_voltage_mv == 0 || (now - last_battery_read_time) >= BATTERY_CACHE_INTERVAL_US) {
        cached_voltage_mv = battery_get_voltage_mv();
        cached_level = battery_get_level();
        last_battery_read_time = now;
    }
}

void ui_init(void)
{
    ui_clear();
}

void ui_clear(void)
{
    display_clear(UI_COLOR_BG);
}

void ui_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    // Bounds check
    if (x < 0 || y < 0 || x + FONT_WIDTH > DISPLAY_WIDTH || y + FONT_HEIGHT > DISPLAY_HEIGHT) {
        return;
    }
    
    // Get character index
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) {
        c = ' ';  // Replace unprintable with space
    }
    
    int char_index = c - FONT_FIRST_CHAR;
    const uint8_t *char_data = &font8x16_data[char_index * FONT_HEIGHT];
    
    // Draw background first (if not transparent)
    if (bg != UI_COLOR_BG) {
        display_fill_rect(x, y, FONT_WIDTH, FONT_HEIGHT, bg);
    } else {
        display_fill_rect(x, y, FONT_WIDTH, FONT_HEIGHT, UI_COLOR_BG);
    }
    
    // Draw character pixels
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t row_data = char_data[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (0x80 >> col)) {
                display_draw_pixel(x + col, y + row, fg);
            }
        }
    }
}

void ui_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg)
{
    if (!text) return;
    
    int start_x = x;
    
    while (*text) {
        if (*text == '\n') {
            x = start_x;
            y += FONT_HEIGHT;
        } else {
            ui_draw_char(x, y, *text, fg, bg);
            x += FONT_WIDTH;
        }
        text++;
        
        // Wrap check
        if (x + FONT_WIDTH > DISPLAY_WIDTH) {
            x = start_x;
            y += FONT_HEIGHT;
        }
        
        if (y + FONT_HEIGHT > DISPLAY_HEIGHT) {
            break;  // Stop if we go off screen
        }
    }
}

void ui_print(int col, int row, const char *text, uint16_t fg)
{
    if (col < 0 || col >= UI_COLS || row < 0 || row >= UI_ROWS) return;
    
    int x = col * FONT_WIDTH;
    int y = row * FONT_HEIGHT;
    
    ui_draw_text(x, y, text, fg, UI_COLOR_BG);
}

void ui_print_center(int row, const char *text, uint16_t fg)
{
    if (!text) return;
    
    int len = strlen(text);
    int col = (UI_COLS - len) / 2;
    if (col < 0) col = 0;
    
    ui_print(col, row, text, fg);
}

void ui_draw_line(int row, uint16_t color)
{
    int y = row * FONT_HEIGHT + FONT_HEIGHT / 2;
    display_draw_hline(0, y, DISPLAY_WIDTH, color);
}

/**
 * @brief Draw battery icon with level indicator (icon only, no voltage text)
 * @param x X position (right edge of icon)
 * @param y Y position
 * @param level Battery level 0-100
 * @param bg Background color
 */
static void draw_battery_icon(int x, int y, int level, uint16_t bg)
{
    // Battery icon dimensions
    const int bat_width = 18;
    const int bat_height = 10;
    const int tip_width = 2;
    const int tip_height = 4;
    
    // Determine color based on level
    uint16_t fill_color;
    if (level > 50) {
        fill_color = COLOR_GREEN;
    } else if (level > 20) {
        fill_color = COLOR_YELLOW;
    } else {
        fill_color = COLOR_RED;
    }
    
    // Battery body position (icon drawn from right edge)
    int bx = x - bat_width - tip_width;
    int by = y;
    
    // Draw battery body outline
    display_draw_rect(bx, by, bat_width, bat_height, UI_COLOR_TEXT);
    
    // Draw battery tip (positive terminal)
    int tip_y = by + (bat_height - tip_height) / 2;
    display_fill_rect(bx + bat_width, tip_y, tip_width, tip_height, UI_COLOR_TEXT);
    
    // Clear inside of battery
    display_fill_rect(bx + 1, by + 1, bat_width - 2, bat_height - 2, bg);
    
    // Draw fill level (inside battery body)
    int clamped_level = (level < 0) ? 0 : (level > 100) ? 100 : level;
    int fill_width = ((bat_width - 4) * clamped_level) / 100;
    if (fill_width > 0) {
        display_fill_rect(bx + 2, by + 2, fill_width, bat_height - 4, fill_color);
    }
}

/**
 * @brief Draw voltage text in the top left corner
 * @param voltage_mv Battery voltage in millivolts
 * @param bg Background color
 */
static void draw_voltage_text(int voltage_mv, uint16_t bg)
{
    char volt_str[12];
    if (voltage_mv > 0 && voltage_mv < 10000) {
        int volts = voltage_mv / 1000;
        int decimals = (voltage_mv % 1000) / 10;
        snprintf(volt_str, sizeof(volt_str), "%d.%02dV", volts, decimals);
    } else {
        snprintf(volt_str, sizeof(volt_str), "?.??V");
    }
    
    // Draw in top left corner with small margin
    ui_draw_text(2, 1, volt_str, UI_COLOR_DIMMED, bg);
}

void ui_draw_title(const char *title)
{
    uint16_t title_bg = RGB565(0, 60, 30);
    
    // Draw title bar background
    display_fill_rect(0, 0, DISPLAY_WIDTH, FONT_HEIGHT + 2, title_bg);
    
    // Draw title text centered
    if (title) {
        int len = strlen(title);
        int x = (DISPLAY_WIDTH - len * FONT_WIDTH) / 2;
        ui_draw_text(x, 1, title, UI_COLOR_TITLE, title_bg);
    }
    
    // Draw battery indicators (uses cached values, refreshed every 30s)
    if (battery_is_available()) {
        update_battery_cache();
        if (cached_level >= 0 && cached_voltage_mv > 0) {
            // Voltage text in top left corner
            draw_voltage_text(cached_voltage_mv, title_bg);
            // Battery icon at right edge
            draw_battery_icon(DISPLAY_WIDTH - 4, 4, cached_level, title_bg);
        }
    }
    
    // Draw bottom line
    display_draw_hline(0, FONT_HEIGHT + 2, DISPLAY_WIDTH, UI_COLOR_BORDER);
}

void ui_draw_status(const char *status)
{
    int y = DISPLAY_HEIGHT - FONT_HEIGHT - 2;
    
    // Draw status bar background
    display_fill_rect(0, y, DISPLAY_WIDTH, FONT_HEIGHT + 2, RGB565(0, 40, 20));
    
    // Draw top line
    display_draw_hline(0, y, DISPLAY_WIDTH, UI_COLOR_BORDER);
    
    // Draw status text
    if (status) {
        ui_draw_text(4, y + 1, status, UI_COLOR_DIMMED, RGB565(0, 40, 20));
    }
}

void ui_draw_menu_item(int row, const char *text, bool selected, bool has_checkbox, bool checked)
{
    int y = row * FONT_HEIGHT;
    int x = 4;
    
    // Calculate background color
    uint16_t bg = selected ? UI_COLOR_SELECTED : UI_COLOR_BG;
    uint16_t fg = selected ? UI_COLOR_HIGHLIGHT : UI_COLOR_TEXT;
    
    // Draw background
    display_fill_rect(0, y, DISPLAY_WIDTH, FONT_HEIGHT, bg);
    
    // Draw selection indicator
    if (selected) {
        display_draw_vline(0, y, FONT_HEIGHT, UI_COLOR_HIGHLIGHT);
        display_draw_vline(1, y, FONT_HEIGHT, UI_COLOR_HIGHLIGHT);
    }
    
    // Draw checkbox if needed
    if (has_checkbox) {
        // Draw checkbox box
        display_draw_rect(x, y + 2, 12, 12, UI_COLOR_BORDER);
        
        // Draw check mark if checked
        if (checked) {
            display_fill_rect(x + 3, y + 5, 6, 6, UI_COLOR_HIGHLIGHT);
        }
        
        x += 16;
    }
    
    // Draw text
    if (text) {
        ui_draw_text(x, y, text, fg, bg);
    }
}

void ui_draw_menu(const char **items, int count, int selected, int scroll_offset)
{
    // Available rows for menu (after title, before status)
    int start_row = 1;
    int visible_rows = UI_ROWS - 2;  // Leave room for title and status
    
    // Calculate visible range
    int first_visible = scroll_offset;
    int last_visible = scroll_offset + visible_rows - 1;
    if (last_visible >= count) last_visible = count - 1;
    
    // Draw visible items
    for (int i = first_visible; i <= last_visible && i < count; i++) {
        int display_row = start_row + (i - first_visible);
        ui_draw_menu_item(display_row, items[i], i == selected, false, false);
    }
    
    // Draw scroll indicators if needed
    if (scroll_offset > 0) {
        ui_print(UI_COLS - 2, start_row, "^", UI_COLOR_DIMMED);
    }
    if (last_visible < count - 1) {
        ui_print(UI_COLS - 2, start_row + visible_rows - 1, "v", UI_COLOR_DIMMED);
    }
}

void ui_draw_progress(int row, int progress, const char *text)
{
    int y = row * FONT_HEIGHT;
    int bar_y = y + 4;
    int bar_height = 8;
    int bar_width = DISPLAY_WIDTH - 8;
    
    // Draw label if provided
    if (text) {
        ui_draw_text(4, y, text, UI_COLOR_TEXT, UI_COLOR_BG);
        bar_y = y + FONT_HEIGHT + 2;
    }
    
    // Draw progress bar outline
    display_draw_rect(4, bar_y, bar_width, bar_height, UI_COLOR_BORDER);
    
    // Draw progress fill
    if (progress > 0) {
        int fill_width = (bar_width - 4) * progress / 100;
        if (fill_width > 0) {
            display_fill_rect(6, bar_y + 2, fill_width, bar_height - 4, UI_COLOR_HIGHLIGHT);
        }
    }
}

void ui_draw_box(int x, int y, int w, int h, uint16_t color)
{
    display_draw_rect(x, y, w, h, color);
}

void ui_show_message(const char *title, const char *message)
{
    // Calculate box dimensions
    int box_w = DISPLAY_WIDTH - 20;
    int box_h = 60;
    int box_x = 10;
    int box_y = (DISPLAY_HEIGHT - box_h) / 2;
    
    // Draw box background
    display_fill_rect(box_x, box_y, box_w, box_h, RGB565(0, 40, 20));
    
    // Draw box border
    display_draw_rect(box_x, box_y, box_w, box_h, UI_COLOR_BORDER);
    display_draw_rect(box_x + 1, box_y + 1, box_w - 2, box_h - 2, UI_COLOR_BORDER);
    
    // Draw title
    if (title) {
        int title_len = strlen(title);
        int title_x = box_x + (box_w - title_len * FONT_WIDTH) / 2;
        ui_draw_text(title_x, box_y + 6, title, UI_COLOR_TITLE, RGB565(0, 40, 20));
    }
    
    // Draw message
    if (message) {
        if (strchr(message, '\n') != NULL) {
            char msg_buf[192];
            snprintf(msg_buf, sizeof(msg_buf), "%s", message);
            int line = 0;
            char *token = strtok(msg_buf, "\n");
            while (token != NULL) {
                int msg_len = strlen(token);
                int msg_x = box_x + (box_w - msg_len * FONT_WIDTH) / 2;
                if (msg_x < box_x + 4) {
                    msg_x = box_x + 4;
                }
                ui_draw_text(msg_x,
                             box_y + 24 + line * (FONT_HEIGHT + 2),
                             token,
                             UI_COLOR_TEXT,
                             RGB565(0, 40, 20));
                line++;
                token = strtok(NULL, "\n");
            }
        } else {
            int msg_len = strlen(message);
            int msg_x = box_x + (box_w - msg_len * FONT_WIDTH) / 2;
            ui_draw_text(msg_x, box_y + 28, message, UI_COLOR_TEXT, RGB565(0, 40, 20));
        }
    }
}



