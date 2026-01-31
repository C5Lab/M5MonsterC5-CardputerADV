/**
 * @file text_ui.h
 * @brief Simple text-based UI library for Cardputer
 * 
 * Terminal-style UI with:
 * - 8x16 monospace font
 * - Green on black theme
 * - Simple menu system
 * - Checkbox lists
 */

#ifndef TEXT_UI_H
#define TEXT_UI_H

#include "display.h"
#include <stdbool.h>

// Text grid dimensions (30 chars x 8 lines for 240x135 with 8x16 font)
#define UI_COLS (DISPLAY_WIDTH / 8)   // 30 columns
#define UI_ROWS (DISPLAY_HEIGHT / 16) // 8 rows

// Theme colors
#define UI_COLOR_BG         COLOR_BLACK
#define UI_COLOR_TEXT       COLOR_GREEN
#define UI_COLOR_TITLE      RGB565(0, 255, 136)  // Bright green
#define UI_COLOR_SELECTED   RGB565(40, 80, 40)   // Dark green bg
#define UI_COLOR_BORDER     RGB565(0, 200, 100)  // Border green
#define UI_COLOR_DIMMED     RGB565(80, 120, 80)  // Dimmed text
#define UI_COLOR_HIGHLIGHT  RGB565(0, 255, 0)    // Bright highlight

/**
 * @brief Initialize the text UI system
 */
void ui_init(void);

/**
 * @brief Clear the entire screen
 */
void ui_clear(void);

/**
 * @brief Draw a single character at pixel position
 * @param x X pixel position
 * @param y Y pixel position
 * @param c Character to draw
 * @param fg Foreground color
 * @param bg Background color
 */
void ui_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg);

/**
 * @brief Draw text at pixel position
 * @param x X pixel position
 * @param y Y pixel position
 * @param text Text string
 * @param fg Foreground color
 * @param bg Background color
 */
void ui_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);

/**
 * @brief Draw text at grid position (column, row)
 * @param col Column (0-29)
 * @param row Row (0-7)
 * @param text Text string
 * @param fg Foreground color
 */
void ui_print(int col, int row, const char *text, uint16_t fg);

/**
 * @brief Draw centered text on a row
 * @param row Row number
 * @param text Text string
 * @param fg Foreground color
 */
void ui_print_center(int row, const char *text, uint16_t fg);

/**
 * @brief Draw a horizontal line
 * @param row Row number
 * @param color Line color
 */
void ui_draw_line(int row, uint16_t color);

/**
 * @brief Draw title bar at top
 * @param title Title text
 */
void ui_draw_title(const char *title);

/**
 * @brief Draw status bar at bottom
 * @param status Status text
 */
void ui_draw_status(const char *status);

/**
 * @brief Draw a menu item
 * @param row Row number
 * @param text Item text
 * @param selected Whether item is selected
 * @param has_checkbox Whether to show checkbox
 * @param checked Whether checkbox is checked
 */
void ui_draw_menu_item(int row, const char *text, bool selected, bool has_checkbox, bool checked);

/**
 * @brief Draw a complete menu
 * @param items Array of menu item strings
 * @param count Number of items
 * @param selected Currently selected index
 * @param scroll_offset Scroll offset for long menus
 */
void ui_draw_menu(const char **items, int count, int selected, int scroll_offset);

/**
 * @brief Draw a progress bar
 * @param row Row number
 * @param progress Progress (0-100)
 * @param text Optional label text
 */
void ui_draw_progress(int row, int progress, const char *text);

/**
 * @brief Draw a box outline
 * @param x X pixel position
 * @param y Y pixel position
 * @param w Width
 * @param h Height
 * @param color Box color
 */
void ui_draw_box(int x, int y, int w, int h, uint16_t color);

/**
 * @brief Show a message box
 * @param title Title text
 * @param message Message text
 */
void ui_show_message(const char *title, const char *message);
void ui_show_message_tall(const char *title, const char *message);

#endif // TEXT_UI_H



