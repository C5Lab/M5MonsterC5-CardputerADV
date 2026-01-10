/**
 * @file screenshot.c
 * @brief Screenshot functionality with SD card storage
 * 
 * Saves screenshots as 24-bit BMP files to /screens/ directory on SD card.
 * File naming: scr_1.bmp, scr_2.bmp, etc.
 */

#include "screenshot.h"
#include "display.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "SCREENSHOT";

// M5Stack Cardputer-Adv SD card pins (from official documentation)
#define SD_PIN_MISO     39
#define SD_PIN_MOSI     14
#define SD_PIN_CLK      40
#define SD_PIN_CS       12

// SD card mount point
#define MOUNT_POINT     "/sdcard"
#define SCREENS_DIR     MOUNT_POINT "/screens"

// Use SPI3_HOST (VSPI) for SD card - SPI2 is used by display
#define SD_SPI_HOST     SPI3_HOST

// State
static bool sd_mounted = false;
static int screenshot_counter = 1;
static sdmmc_card_t *card = NULL;

// BMP file header structures (packed for correct byte alignment)
#pragma pack(push, 1)
typedef struct {
    uint16_t type;          // Magic identifier: 0x4D42 ("BM")
    uint32_t size;          // File size in bytes
    uint16_t reserved1;     // Not used
    uint16_t reserved2;     // Not used
    uint32_t offset;        // Offset to image data in bytes
} bmp_file_header_t;

typedef struct {
    uint32_t size;          // Header size in bytes (40)
    int32_t width;          // Width of image
    int32_t height;         // Height of image (negative = top-down)
    uint16_t planes;        // Number of colour planes (1)
    uint16_t bits;          // Bits per pixel (24)
    uint32_t compression;   // Compression type (0 = none)
    uint32_t imagesize;     // Image size in bytes
    int32_t xresolution;    // Pixels per meter X
    int32_t yresolution;    // Pixels per meter Y
    uint32_t ncolours;      // Number of colours
    uint32_t importantcolours; // Important colours
} bmp_info_header_t;
#pragma pack(pop)

/**
 * @brief Find next available screenshot number by scanning existing files
 */
static void find_next_screenshot_number(void)
{
    DIR *dir = opendir(SCREENS_DIR);
    if (dir == NULL) {
        screenshot_counter = 1;
        return;
    }
    
    int max_num = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int num;
        if (sscanf(entry->d_name, "scr_%d.bmp", &num) == 1) {
            if (num > max_num) {
                max_num = num;
            }
        }
    }
    closedir(dir);
    
    screenshot_counter = max_num + 1;
    ESP_LOGI(TAG, "Next screenshot number: %d", screenshot_counter);
}

esp_err_t screenshot_init(void)
{
    ESP_LOGI(TAG, "Initializing screenshot module...");
    ESP_LOGI(TAG, "SD pins: MISO=%d, MOSI=%d, CLK=%d, CS=%d", 
             SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CLK, SD_PIN_CS);
    
    // Initialize SPI bus for SD card
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Mount SD card
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = SD_SPI_HOST;
    
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount SD card filesystem");
        } else {
            ESP_LOGW(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        spi_bus_free(SD_SPI_HOST);
        return ret;
    }
    
    sd_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully");
    
    // Print card info
    sdmmc_card_print_info(stdout, card);
    
    // Create screens directory if it doesn't exist
    struct stat st;
    if (stat(SCREENS_DIR, &st) != 0) {
        ESP_LOGI(TAG, "Creating %s directory", SCREENS_DIR);
        if (mkdir(SCREENS_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create screens directory");
        }
    }
    
    // Find next available screenshot number
    find_next_screenshot_number();
    
    ESP_LOGI(TAG, "Screenshot module initialized");
    return ESP_OK;
}

bool screenshot_is_available(void)
{
    return sd_mounted;
}

/**
 * @brief Convert RGB565 to RGB888
 */
static inline void rgb565_to_rgb888(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    // RGB565: RRRRRGGGGGGBBBBB
    *r = ((color >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits
    *g = ((color >> 5) & 0x3F) << 2;   // 6 bits -> 8 bits
    *b = (color & 0x1F) << 3;          // 5 bits -> 8 bits
}

esp_err_t screenshot_take(void)
{
    if (!sd_mounted) {
        ESP_LOGW(TAG, "SD card not mounted, cannot take screenshot");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Taking screenshot #%d...", screenshot_counter);
    
    // Generate filename
    char filename[64];
    snprintf(filename, sizeof(filename), "%s/scr_%d.bmp", SCREENS_DIR, screenshot_counter);
    
    // Open file for writing
    FILE *f = fopen(filename, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filename);
        return ESP_FAIL;
    }
    
    // Get framebuffer
    const uint16_t *fb = display_get_framebuffer();
    
    // Calculate sizes
    int width = DISPLAY_WIDTH;
    int height = DISPLAY_HEIGHT;
    int row_size = ((width * 3 + 3) / 4) * 4;  // Row size padded to 4 bytes
    int image_size = row_size * height;
    int file_size = 54 + image_size;  // Headers (14+40) + image data
    
    // Prepare BMP file header
    bmp_file_header_t file_header = {
        .type = 0x4D42,         // "BM"
        .size = file_size,
        .reserved1 = 0,
        .reserved2 = 0,
        .offset = 54            // 14 + 40
    };
    
    // Prepare BMP info header
    bmp_info_header_t info_header = {
        .size = 40,
        .width = width,
        .height = -height,      // Negative for top-down (matches our framebuffer layout)
        .planes = 1,
        .bits = 24,
        .compression = 0,
        .imagesize = image_size,
        .xresolution = 2835,    // 72 DPI
        .yresolution = 2835,
        .ncolours = 0,
        .importantcolours = 0
    };
    
    // Write headers
    fwrite(&file_header, sizeof(file_header), 1, f);
    fwrite(&info_header, sizeof(info_header), 1, f);
    
    // Write pixel data row by row
    uint8_t *row_buffer = malloc(row_size);
    if (row_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate row buffer");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    for (int y = 0; y < height; y++) {
        memset(row_buffer, 0, row_size);  // Clear padding bytes
        
        for (int x = 0; x < width; x++) {
            uint16_t pixel = fb[y * width + x];
            uint8_t r, g, b;
            rgb565_to_rgb888(pixel, &r, &g, &b);
            
            // BMP stores pixels as BGR
            row_buffer[x * 3 + 0] = b;
            row_buffer[x * 3 + 1] = g;
            row_buffer[x * 3 + 2] = r;
        }
        
        fwrite(row_buffer, row_size, 1, f);
    }
    
    free(row_buffer);
    fclose(f);
    
    ESP_LOGI(TAG, "Screenshot saved: %s", filename);
    screenshot_counter++;
    
    return ESP_OK;
}








