/**
 * @file cc1101_cap.c
 * @brief Slim CC1101 ASK/OOK driver for Cardputer Cap (SPI3 + GDO0).
 *
 * Ported from janos_subghz subghz_cc1101.c: SPI, Flipper OOK 650 kHz async
 * preset, GDO0 ANYEDGE ISR, PATABLE software OOK TX, jammer. No decode.
 */

#include "cc1101_cap.h"
#include "screenshot.h"

#include <stdint.h>
#include <math.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "CC1101_CAP";

#define CC1101_IOCFG0       0x02
#define CC1101_FIFOTHR      0x03
#define CC1101_PKTLEN       0x06
#define CC1101_PKTCTRL1     0x07
#define CC1101_PKTCTRL0     0x08
#define CC1101_FSCTRL1      0x0B
#define CC1101_FSCTRL0      0x0C
#define CC1101_FREQ2        0x0D
#define CC1101_FREQ1        0x0E
#define CC1101_FREQ0        0x0F
#define CC1101_MDMCFG4      0x10
#define CC1101_MDMCFG3      0x11
#define CC1101_MDMCFG2      0x12
#define CC1101_MDMCFG1      0x13
#define CC1101_MDMCFG0      0x14
#define CC1101_DEVIATN      0x15
#define CC1101_MCSM0        0x18
#define CC1101_FOCCFG       0x19
#define CC1101_AGCCTRL2     0x1B
#define CC1101_AGCCTRL1     0x1C
#define CC1101_AGCCTRL0     0x1D
#define CC1101_WORCTRL      0x20
#define CC1101_FREND1       0x21
#define CC1101_FREND0       0x22
#define CC1101_FSCAL3       0x23
#define CC1101_FSCAL2       0x24
#define CC1101_FSCAL1       0x25
#define CC1101_FSCAL0       0x26
#define CC1101_FSTEST       0x29
#define CC1101_TEST0        0x2E
#define CC1101_PARTNUM      0x30
#define CC1101_VERSION      0x31
#define CC1101_RSSI_REG     0x34
#define CC1101_MARCSTATE    0x35
#define CC1101_SRES         0x30
#define CC1101_SRX          0x34
#define CC1101_STX          0x35
#define CC1101_SIDLE        0x36
#define CC1101_SFRX         0x3A
#define CC1101_PATABLE      0x3E
#define CC1101_WRITE_SINGLE 0x00
#define CC1101_WRITE_BURST  0x40
#define CC1101_READ_SINGLE  0x80
#define CC1101_READ_BURST   0xC0

#define RAW_GLITCH_MIN_US   110
#define SUBGHZ_RAW_TX_REPS  3
#define SUBGHZ_RAW_TX_GAP_US 4000
#define SUBGHZ_RAW_TX_MIN_FRAME_EDGES 8

#define NVS_NS              "cc1101cap"
#define NVS_KEY_FCORR       "freq_corr"
#define FCORR_MAX_ABS_MHZ   5.0f

static spi_device_handle_t s_spi;
static bool s_initialized;
static bool s_jamming;
static float s_frequency = CC1101_CAP_DEFAULT_FREQ;
static float s_freq_correction;
static bool s_freq_correction_loaded;

static volatile int32_t *s_rc_timings;
static volatile int s_rc_change_count;
static volatile int64_t s_last_edge_us;
static volatile uint32_t s_raw_glitch_pending_us;
static volatile bool s_capture_armed;

static void spi_lock(void)
{
    screenshot_spi_acquire();
}

static void spi_unlock(void)
{
    screenshot_spi_release();
}

static esp_err_t cc1101_spi_polling(spi_transaction_t *t)
{
    spi_lock();
    esp_err_t err = spi_device_polling_transmit(s_spi, t);
    spi_unlock();
    return err;
}

static esp_err_t cc1101_strobe(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_data = {cmd},
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
    };
    return cc1101_spi_polling(&t);
}

static esp_err_t cc1101_write_reg(uint8_t addr, uint8_t val)
{
    uint8_t tx[2] = {(uint8_t)(addr | CC1101_WRITE_SINGLE), val};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
    };
    return cc1101_spi_polling(&t);
}

static uint8_t cc1101_read_reg(uint8_t addr)
{
    uint8_t tx[2] = {(uint8_t)(addr | CC1101_READ_SINGLE), 0};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    cc1101_spi_polling(&t);
    return rx[1];
}

static uint8_t cc1101_read_status(uint8_t addr)
{
    uint8_t tx[2] = {(uint8_t)(addr | CC1101_READ_BURST), 0};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    cc1101_spi_polling(&t);
    return rx[1];
}

static void cc1101_write_pa_table(uint8_t val)
{
    uint8_t pa[8] = {0x00, val, 0, 0, 0, 0, 0, 0};
    uint8_t tx[9];
    tx[0] = CC1101_PATABLE | CC1101_WRITE_BURST;
    memcpy(&tx[1], pa, 8);
    spi_transaction_t t = {
        .length = 9 * 8,
        .tx_buffer = tx,
    };
    cc1101_spi_polling(&t);
}

static void cc1101_reset(void)
{
    cc1101_strobe(CC1101_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void cc1101_set_idle(void)
{
    cc1101_strobe(CC1101_SIDLE);
    int retries = 50;
    while (retries-- > 0) {
        uint8_t st = cc1101_read_status(CC1101_MARCSTATE) & 0x1F;
        if (st == 0x01) {
            break;
        }
        esp_rom_delay_us(100);
    }
}

static void freq_correction_load(void)
{
    if (s_freq_correction_loaded) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t cc = 0;
        if (nvs_get_i32(h, NVS_KEY_FCORR, &cc) == ESP_OK) {
            s_freq_correction = (float)cc / 100.0f;
            ESP_LOGI(TAG, "Freq correction %.2f MHz", s_freq_correction);
        }
        nvs_close(h);
    }
    s_freq_correction_loaded = true;
}

static void cc1101_set_frequency_regs(float mhz)
{
    uint32_t freq_word = (uint32_t)(((mhz + s_freq_correction) * 65536.0f) / 26.0f);
    cc1101_set_idle();
    cc1101_write_reg(CC1101_FREQ2, (freq_word >> 16) & 0xFF);
    cc1101_write_reg(CC1101_FREQ1, (freq_word >> 8) & 0xFF);
    cc1101_write_reg(CC1101_FREQ0, (freq_word >> 0) & 0xFF);
}

static void cc1101_set_rx_bw(float bw_khz)
{
    uint8_t reg = cc1101_read_reg(CC1101_MDMCFG4) & 0x0F;
    uint8_t bw_bits;
    if (bw_khz >= 812) {
        bw_bits = 0x00;
    } else if (bw_khz >= 650) {
        bw_bits = 0x10;
    } else if (bw_khz >= 541) {
        bw_bits = 0x20;
    } else if (bw_khz >= 406) {
        bw_bits = 0x30;
    } else if (bw_khz >= 325) {
        bw_bits = 0x40;
    } else if (bw_khz >= 270) {
        bw_bits = 0x50;
    } else if (bw_khz >= 203) {
        bw_bits = 0x60;
    } else {
        bw_bits = 0xF0;
    }
    cc1101_write_reg(CC1101_MDMCFG4, reg | bw_bits);
}

static void cc1101_set_modulation(uint8_t mod)
{
    uint8_t reg = cc1101_read_reg(CC1101_MDMCFG2);
    reg = (uint8_t)((reg & 0x8F) | ((mod & 0x07) << 4));
    cc1101_write_reg(CC1101_MDMCFG2, reg);
}

static void cc1101_set_pa_level(int dbm)
{
    uint8_t pa;
    if (dbm >= 12) {
        pa = 0xC0;
    } else if (dbm >= 10) {
        pa = 0xC5;
    } else if (dbm >= 7) {
        pa = 0xCD;
    } else if (dbm >= 0) {
        pa = 0x50;
    } else {
        pa = 0x26;
    }
    cc1101_write_pa_table(pa);
}

static void cc1101_enter_rx_locked(void)
{
    cc1101_set_idle();
    cc1101_strobe(CC1101_SFRX);
    cc1101_strobe(CC1101_SRX);
    for (int i = 0; i < 200; i++) {
        uint8_t st = cc1101_read_status(CC1101_MARCSTATE) & 0x1F;
        if (st == 0x0D) {
            return;
        }
        if (st == 0x11) {
            cc1101_strobe(CC1101_SFRX);
            cc1101_strobe(CC1101_SRX);
        }
        esp_rom_delay_us(100);
    }
}

static void cc1101_enter_tx(void)
{
    cc1101_set_idle();
    cc1101_strobe(CC1101_STX);
}

static void cc1101_calibrate(float mhz)
{
    if (mhz >= 300.0f && mhz <= 348.0f) {
        uint8_t fsctrl0 = (uint8_t)(24 + (mhz - 300.0f) / (348.0f - 300.0f) * 4.0f);
        cc1101_write_reg(CC1101_FSCTRL0, fsctrl0);
        if (mhz < 322.88f) {
            cc1101_write_reg(CC1101_TEST0, 0x0B);
        } else {
            cc1101_write_reg(CC1101_TEST0, 0x09);
            uint8_t s = cc1101_read_status(CC1101_FSCAL2);
            if (s < 32) {
                cc1101_write_reg(CC1101_FSCAL2, (uint8_t)(s + 32));
            }
        }
    } else if (mhz >= 378.0f && mhz <= 464.0f) {
        uint8_t fsctrl0 = (uint8_t)(31 + (mhz - 378.0f) / (464.0f - 378.0f) * 7.0f);
        cc1101_write_reg(CC1101_FSCTRL0, fsctrl0);
        if (mhz < 430.5f) {
            cc1101_write_reg(CC1101_TEST0, 0x0B);
        } else {
            cc1101_write_reg(CC1101_TEST0, 0x09);
            uint8_t s = cc1101_read_status(CC1101_FSCAL2);
            if (s < 32) {
                cc1101_write_reg(CC1101_FSCAL2, (uint8_t)(s + 32));
            }
        }
    } else if (mhz >= 779.0f && mhz <= 928.0f) {
        uint8_t fsctrl0 = (mhz < 900.0f)
            ? (uint8_t)(65 + (mhz - 779.0f) / (899.0f - 779.0f) * 11.0f)
            : (uint8_t)(77 + (mhz - 900.0f) / (928.0f - 900.0f) * 2.0f);
        cc1101_write_reg(CC1101_FSCTRL0, fsctrl0);
        cc1101_write_reg(CC1101_TEST0, mhz < 861.0f ? 0x0B : 0x09);
        if (mhz >= 861.0f) {
            uint8_t s = cc1101_read_status(CC1101_FSCAL2);
            if (s < 32) {
                cc1101_write_reg(CC1101_FSCAL2, (uint8_t)(s + 32));
            }
        }
    }
}

static void cc1101_gdo0_input(void)
{
    gpio_set_direction(CC1101_CAP_GDO0_PIN, GPIO_MODE_INPUT);
    gpio_set_intr_type(CC1101_CAP_GDO0_PIN, GPIO_INTR_ANYEDGE);
}

static void cc1101_full_init(void)
{
    cc1101_reset();
    cc1101_write_reg(CC1101_IOCFG0, 0x0D);
    cc1101_write_reg(CC1101_FIFOTHR, 0x07);
    cc1101_write_reg(CC1101_PKTCTRL0, 0x32);
    cc1101_write_reg(CC1101_FSCTRL1, 0x06);
    cc1101_set_frequency_regs(s_frequency);
    cc1101_calibrate(s_frequency);
    cc1101_write_reg(CC1101_MDMCFG0, 0x00);
    cc1101_write_reg(CC1101_MDMCFG1, 0x00);
    cc1101_write_reg(CC1101_MDMCFG2, 0x30);
    cc1101_write_reg(CC1101_MDMCFG3, 0x32);
    cc1101_write_reg(CC1101_MDMCFG4, 0x17);
    cc1101_write_reg(CC1101_DEVIATN, 0x00);
    cc1101_write_reg(CC1101_MCSM0, 0x18);
    cc1101_write_reg(CC1101_FOCCFG, 0x18);
    cc1101_write_reg(CC1101_AGCCTRL0, 0x91);
    cc1101_write_reg(CC1101_AGCCTRL1, 0x00);
    cc1101_write_reg(CC1101_AGCCTRL2, 0x07);
    cc1101_write_reg(CC1101_WORCTRL, 0xFB);
    cc1101_write_reg(CC1101_FREND0, 0x11);
    cc1101_write_reg(CC1101_FREND1, 0xB6);
    cc1101_write_reg(CC1101_FSCAL3, 0xE9);
    cc1101_write_reg(CC1101_FSCAL2, 0x2A);
    cc1101_write_reg(CC1101_FSCAL1, 0x00);
    cc1101_write_reg(CC1101_FSCAL0, 0x1F);
    cc1101_write_reg(CC1101_FSTEST, 0x59);
    cc1101_write_reg(CC1101_PKTCTRL1, 0x04);
    cc1101_write_reg(CC1101_PKTLEN, 0x00);
    cc1101_set_pa_level(12);
    cc1101_gdo0_input();
    cc1101_enter_rx_locked();
}

static void IRAM_ATTR gdo0_isr_handler(void *arg)
{
    (void)arg;
    if (!s_capture_armed || !s_rc_timings) {
        return;
    }

    int64_t now = esp_timer_get_time();
    uint32_t duration = (uint32_t)(now - s_last_edge_us);
    s_last_edge_us = now;

    int level = gpio_get_level(CC1101_CAP_GDO0_PIN);

    if (duration < RAW_GLITCH_MIN_US) {
        s_raw_glitch_pending_us += duration;
        return;
    }

    uint32_t total_dur = duration + s_raw_glitch_pending_us;
    s_raw_glitch_pending_us = 0;
    int32_t adjusted = (level == 0) ? (int32_t)total_dur : -(int32_t)total_dur;

    if (s_rc_change_count > 0) {
        int32_t prev = s_rc_timings[s_rc_change_count - 1];
        bool same_sign = (prev > 0) == (adjusted > 0);
        if (same_sign) {
            s_rc_timings[s_rc_change_count - 1] = prev + adjusted;
            return;
        }
    }

    if (s_rc_change_count < CC1101_CAP_RAW_BUF_SIZE) {
        s_rc_timings[s_rc_change_count++] = adjusted;
    }
}

static inline void tx_spin_until(int64_t target_us, int *spin_acc_us)
{
    int64_t remain = target_us - esp_timer_get_time();
    if (remain > 4000) {
        int64_t delay_us = remain - 1500;
        vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));
        *spin_acc_us = 0;
    } else if (remain > 0 && *spin_acc_us > 200000) {
        vTaskDelay(1);
        *spin_acc_us = 0;
    }
    if (remain > 0) {
        *spin_acc_us += (int)remain;
    }
    while (esp_timer_get_time() < target_us) {
    }
}

static bool raw_tx_is_gap(int32_t t)
{
    return abs(t) >= SUBGHZ_RAW_TX_GAP_US;
}

static void raw_tx_frame_bounds(const int32_t *timings, int count,
                                int *out_start, int *out_len)
{
    *out_start = 0;
    *out_len = count;
    if (count < SUBGHZ_RAW_TX_MIN_FRAME_EDGES) {
        return;
    }
    for (int i = SUBGHZ_RAW_TX_MIN_FRAME_EDGES; i < count; i++) {
        if (raw_tx_is_gap(timings[i])) {
            *out_len = i;
            return;
        }
    }
}

static int raw_tx_compute_bias(const int32_t *timings, int count)
{
    int64_t pos_sum = 0, neg_sum = 0;
    int pos_count = 0, neg_count = 0;
    uint32_t min_pulse = UINT32_MAX;

    for (int i = 0; i < count; i++) {
        uint32_t a = (uint32_t)abs(timings[i]);
        if (a < 80 || a > 2000) {
            continue;
        }
        if (a < min_pulse) {
            min_pulse = a;
        }
        if (timings[i] >= 0) {
            pos_sum += a;
            pos_count++;
        } else {
            neg_sum += a;
            neg_count++;
        }
    }

    int bias = 0;
    if (pos_count > 5 && neg_count > 5) {
        int avg_pos = (int)(pos_sum / pos_count);
        int avg_neg = (int)(neg_sum / neg_count);
        bias = (avg_neg - avg_pos) / 2;
        if (bias < 0) {
            bias = 0;
        }
        if (bias > 150) {
            bias = 150;
        }
    }
    if (min_pulse < 380) {
        bias = 0;
    }
    return bias;
}

esp_err_t cc1101_cap_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = screenshot_ensure_spi_bus();
    if (ret != ESP_OK) {
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CC1101_CAP_CS_PIN,
        .queue_size = 4,
    };
    ret = spi_bus_add_device(SPI3_HOST, &dev_cfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    cc1101_strobe(CC1101_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t version = cc1101_read_status(CC1101_VERSION);
    uint8_t partnum = cc1101_read_status(CC1101_PARTNUM);
    ESP_LOGI(TAG, "CC1101 PARTNUM=0x%02X VERSION=0x%02X", partnum, version);
    if (version == 0x00 || version == 0xFF) {
        ESP_LOGE(TAG, "CC1101 not detected (CS=%d GDO0=%d)",
                 CC1101_CAP_CS_PIN, CC1101_CAP_GDO0_PIN);
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    s_rc_timings = heap_caps_malloc(CC1101_CAP_RAW_BUF_SIZE * sizeof(int32_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rc_timings) {
        s_rc_timings = malloc(CC1101_CAP_RAW_BUF_SIZE * sizeof(int32_t));
    }
    if (!s_rc_timings) {
        ESP_LOGE(TAG, "capture buffer alloc failed");
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        return ESP_ERR_NO_MEM;
    }

    gpio_reset_pin(CC1101_CAP_GDO0_PIN);
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << CC1101_CAP_GDO0_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_cfg);
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service: %s", esp_err_to_name(ret));
    }
    ret = gpio_isr_handler_add(CC1101_CAP_GDO0_PIN, gdo0_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ISR add: %s", esp_err_to_name(ret));
    }
    gpio_intr_disable(CC1101_CAP_GDO0_PIN);

    freq_correction_load();
    cc1101_full_init();

    s_initialized = true;
    ESP_LOGI(TAG, "CC1101 ready on SPI3 CS=%d GDO0=%d @ %.2f MHz",
             CC1101_CAP_CS_PIN, CC1101_CAP_GDO0_PIN, s_frequency);
    return ESP_OK;
}

bool cc1101_cap_is_initialized(void)
{
    return s_initialized;
}

esp_err_t cc1101_cap_set_frequency(float mhz)
{
    if (mhz < 300.0f || mhz > 1000.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    s_frequency = mhz;
    if (s_initialized && !s_jamming) {
        cc1101_set_idle();
        cc1101_set_frequency_regs(mhz);
        cc1101_calibrate(mhz);
        cc1101_enter_rx_locked();
    }
    return ESP_OK;
}

float cc1101_cap_get_frequency(void)
{
    return s_frequency;
}

float cc1101_cap_get_freq_correction(void)
{
    freq_correction_load();
    return s_freq_correction;
}

esp_err_t cc1101_cap_set_freq_correction(float mhz)
{
    if (isnan(mhz) || isinf(mhz)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fabsf(mhz) > FCORR_MAX_ABS_MHZ) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t cc = (int32_t)lroundf(mhz * 100.0f);
    float new_corr = (float)cc / 100.0f;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(h, NVS_KEY_FCORR, cc);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    s_freq_correction = new_corr;
    s_freq_correction_loaded = true;
    if (s_initialized && !s_jamming) {
        cc1101_set_idle();
        cc1101_set_frequency_regs(s_frequency);
        cc1101_enter_rx_locked();
    }
    return ESP_OK;
}

int cc1101_cap_get_rssi(void)
{
    if (!s_initialized) {
        return -127;
    }
    uint8_t raw_u = cc1101_read_status(CC1101_RSSI_REG);
    if (raw_u >= 128) {
        return ((int)raw_u - 256) / 2 - 74;
    }
    return (int)raw_u / 2 - 74;
}

void cc1101_cap_enter_rx(void)
{
    if (!s_initialized) {
        return;
    }
    cc1101_gdo0_input();
    cc1101_full_init();
}

void cc1101_cap_idle(void)
{
    if (!s_initialized) {
        return;
    }
    cc1101_cap_capture_stop();
    cc1101_set_idle();
}

void cc1101_cap_capture_start(void)
{
    if (!s_initialized || !s_rc_timings) {
        return;
    }
    s_rc_change_count = 0;
    s_raw_glitch_pending_us = 0;
    s_last_edge_us = esp_timer_get_time();
    s_capture_armed = true;
    gpio_intr_enable(CC1101_CAP_GDO0_PIN);
}

void cc1101_cap_capture_stop(void)
{
    s_capture_armed = false;
    if (s_initialized) {
        gpio_intr_disable(CC1101_CAP_GDO0_PIN);
    }
}

int cc1101_cap_capture_count(void)
{
    return s_rc_change_count;
}

int64_t cc1101_cap_last_edge_us(void)
{
    return s_last_edge_us;
}

int cc1101_cap_copy_timings(int32_t *dst, int max)
{
    if (!dst || max <= 0 || !s_rc_timings) {
        return 0;
    }
    int n = s_rc_change_count;
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        dst[i] = s_rc_timings[i];
    }
    return n;
}

esp_err_t cc1101_cap_tx_timings(float mhz, const int32_t *timings, int count)
{
    return cc1101_cap_tx_timings_reps(mhz, timings, count, SUBGHZ_RAW_TX_REPS);
}

esp_err_t cc1101_cap_tx_timings_reps(float mhz, const int32_t *timings, int count,
                                     int reps)
{
    if (!timings || count < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reps < 1) {
        reps = 1;
    }
    if (reps > 20) {
        reps = 20;
    }
    if (!s_initialized) {
        esp_err_t err = cc1101_cap_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    cc1101_cap_capture_stop();

    /* Hold SPI for the whole burst so SD cannot sneak between PATABLE writes. */
    screenshot_spi_acquire();

    cc1101_set_idle();
    cc1101_set_modulation(3);
    cc1101_write_reg(CC1101_FREND0, 0x10);
    cc1101_set_frequency_regs(mhz);
    cc1101_set_rx_bw(270.0f);
    cc1101_set_pa_level(12);

    gpio_set_direction(CC1101_CAP_GDO0_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CC1101_CAP_GDO0_PIN, 0);

    cc1101_enter_tx();
    for (int i = 0; i < 200; i++) {
        uint8_t st = cc1101_read_status(CC1101_MARCSTATE) & 0x1F;
        if (st == 0x13) {
            break;
        }
        esp_rom_delay_us(100);
    }

    const uint8_t pa_on = 0xC0;
    int frame_start = 0, frame_len = count;
    raw_tx_frame_bounds(timings, count, &frame_start, &frame_len);
    const int32_t *frame = timings + frame_start;
    int bias = raw_tx_compute_bias(frame, frame_len);

    for (int rep = 0; rep < reps; rep++) {
        int64_t target = esp_timer_get_time();
        int spin_acc_us = 0;
        for (int i = 0; i < frame_len; i++) {
            int32_t t = frame[i];
            int32_t dur = (t >= 0) ? (t + bias) : -(abs(t) - bias);
            if (abs(dur) < 10) {
                dur = (t >= 0) ? 10 : -10;
            }
            target += (int64_t)abs(dur);
            cc1101_write_reg(CC1101_PATABLE, (t >= 0) ? pa_on : 0x00);
            tx_spin_until(target, &spin_acc_us);
        }
        cc1101_write_reg(CC1101_PATABLE, 0x00);
        if (rep + 1 < reps) {
            int64_t gap = esp_timer_get_time() + SUBGHZ_RAW_TX_GAP_US;
            tx_spin_until(gap, &spin_acc_us);
        }
    }

    screenshot_spi_release();
    cc1101_cap_enter_rx();
    return ESP_OK;
}

esp_err_t cc1101_cap_jam_start(float mhz)
{
    if (!s_initialized) {
        esp_err_t err = cc1101_cap_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_jamming) {
        return ESP_OK;
    }

    cc1101_cap_capture_stop();
    s_frequency = mhz;

    cc1101_set_idle();
    cc1101_set_modulation(0);
    cc1101_set_frequency_regs(mhz);
    cc1101_set_pa_level(12);
    cc1101_write_reg(CC1101_DEVIATN, 0x00);
    cc1101_set_rx_bw(270.0f);
    cc1101_enter_tx();
    cc1101_write_reg(CC1101_PATABLE, 0xFF);

    s_jamming = true;
    ESP_LOGI(TAG, "Jammer on %.2f MHz", mhz);
    return ESP_OK;
}

void cc1101_cap_jam_stop(void)
{
    if (!s_jamming) {
        return;
    }
    s_jamming = false;
    cc1101_set_idle();
    cc1101_set_modulation(3);
    cc1101_cap_enter_rx();
    ESP_LOGI(TAG, "Jammer stopped");
}

bool cc1101_cap_is_jamming(void)
{
    return s_jamming;
}
