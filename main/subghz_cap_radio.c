/**
 * @file subghz_cap_radio.c
 * @brief Glue: Cap CC1101 capture/TX + Monster subghz_ext_* decode/encode.
 */

#include "subghz_cap_radio.h"
#include "cc1101_cap.h"
#include "cc1101_tesla_raw.h"
#include "subghz_ext_client.h"
#include "uart_handler.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SUBGHZ_CAP";

#define CAP_MAX_STORED     24
#define CAP_MIN_EDGES      16
#define CAP_SILENCE_US     8000
#define CAP_RSSI_HZ_TICKS  50

typedef struct {
    int idx;
    bool used;
    bool is_raw;
    bool rolling;
    float freq;
    char type[32];
    char proto[32];
    char serial[32];
    char id[32];
    char mf[32];
    char learn[24];
    int bits;
    int te;
    int btn;
    int cnt;
    uint32_t seed;
    int32_t *timings;
    int timing_count;
} cap_capture_t;

static cap_capture_t s_caps[CAP_MAX_STORED];
static int s_next_idx = 1;
static SemaphoreHandle_t s_cap_mtx;

static TaskHandle_t s_rx_task;
static volatile bool s_rx_stop;
static volatile bool s_rx_running;
static float s_rx_freq = 433.92f;
static bool s_rx_raw;
static int s_rx_min_rssi = -80;

static SemaphoreHandle_t s_op_mtx;

static void ensure_locks(void)
{
    if (!s_cap_mtx) {
        s_cap_mtx = xSemaphoreCreateMutex();
    }
    if (!s_op_mtx) {
        s_op_mtx = xSemaphoreCreateMutex();
    }
}

static void cap_free_slot(cap_capture_t *c)
{
    if (c->timings) {
        free(c->timings);
        c->timings = NULL;
    }
    memset(c, 0, sizeof(*c));
}

static cap_capture_t *cap_find(int idx)
{
    for (int i = 0; i < CAP_MAX_STORED; i++) {
        if (s_caps[i].used && s_caps[i].idx == idx) {
            return &s_caps[i];
        }
    }
    return NULL;
}

static cap_capture_t *cap_alloc_slot(void)
{
    for (int i = 0; i < CAP_MAX_STORED; i++) {
        if (!s_caps[i].used) {
            return &s_caps[i];
        }
    }
    /* Drop oldest (lowest idx). */
    int oldest = 0;
    for (int i = 1; i < CAP_MAX_STORED; i++) {
        if (s_caps[i].idx < s_caps[oldest].idx) {
            oldest = i;
        }
    }
    cap_free_slot(&s_caps[oldest]);
    return &s_caps[oldest];
}

static int store_capture(const int32_t *timings, int count, float freq,
                         const subghz_ext_decode_t *dec, bool raw)
{
    xSemaphoreTake(s_cap_mtx, portMAX_DELAY);
    cap_capture_t *c = cap_alloc_slot();
    cap_free_slot(c);
    c->timings = malloc((size_t)count * sizeof(int32_t));
    if (!c->timings) {
        xSemaphoreGive(s_cap_mtx);
        return -1;
    }
    memcpy(c->timings, timings, (size_t)count * sizeof(int32_t));
    c->timing_count = count;
    c->freq = freq;
    c->used = true;
    c->idx = s_next_idx++;
    c->is_raw = raw;
    if (dec) {
        c->rolling = dec->rolling;
        c->bits = dec->bits;
        c->te = dec->te;
        c->btn = dec->btn;
        c->cnt = dec->cnt;
        c->seed = dec->seed;
        snprintf(c->type, sizeof(c->type), "%s", dec->type);
        snprintf(c->proto, sizeof(c->proto), "%s", dec->proto[0] ? dec->proto : dec->type);
        snprintf(c->serial, sizeof(c->serial), "%s", dec->serial);
        snprintf(c->id, sizeof(c->id), "%s", dec->id);
        snprintf(c->mf, sizeof(c->mf), "%s", dec->mf);
        snprintf(c->learn, sizeof(c->learn), "%s", dec->learn);
    } else {
        snprintf(c->type, sizeof(c->type), "RAW");
    }
    int idx = c->idx;
    xSemaphoreGive(s_cap_mtx);
    return idx;
}

static void inject_rx(int idx, const subghz_ext_decode_t *dec, float freq, int edges)
{
    char line[256];
    if (!dec) {
        snprintf(line, sizeof(line),
                 "[SUBGHZ_RAW] idx=%d freq=%.2f edges=%d name=cap_%d",
                 idx, freq, edges, idx);
    } else if (dec->rolling) {
        snprintf(line, sizeof(line),
                 "[SUBGHZ_RX] idx=%d type=%s freq=%.2f bits=%d serial=%s btn=%d "
                 "proto=%s learn=%s mf=%s cnt=%d te=%d name=cap_%d",
                 idx,
                 dec->type[0] ? dec->type : "?",
                 dec->freq > 0.1f ? dec->freq : freq,
                 dec->bits,
                 dec->serial[0] ? dec->serial : "0x0",
                 dec->btn,
                 dec->proto[0] ? dec->proto : dec->type,
                 dec->learn[0] ? dec->learn : "-",
                 dec->mf[0] ? dec->mf : "-",
                 dec->cnt,
                 dec->te,
                 idx);
    } else {
        snprintf(line, sizeof(line),
                 "[SUBGHZ_RX] idx=%d type=%s freq=%.2f bits=%d id=%s te=%d "
                 "proto=%s learn=- mf=%s cnt=0 name=cap_%d",
                 idx,
                 dec->type[0] ? dec->type : "?",
                 dec->freq > 0.1f ? dec->freq : freq,
                 dec->bits,
                 dec->id[0] ? dec->id : "0",
                 dec->te,
                 dec->proto[0] ? dec->proto : dec->type,
                 dec->mf[0] ? dec->mf : "-",
                 idx);
    }
    uart_dispatch_line(line);
}

static void inject_rssi(int rssi)
{
    char line[32];
    snprintf(line, sizeof(line), "[SUBGHZ_RSSI] %d", rssi);
    uart_dispatch_line(line);
}

static void process_burst(const int32_t *timings, int count, float freq, bool raw)
{
    if (raw) {
        int idx = store_capture(timings, count, freq, NULL, true);
        if (idx > 0) {
            inject_rx(idx, NULL, freq, count);
        }
        return;
    }

    subghz_ext_decode_t dec;
    memset(&dec, 0, sizeof(dec));
    subghz_ext_status_t st = subghz_ext_decode(timings, count, freq, &dec);
    if (st == SUBGHZ_EXT_ERR_LICENSE) {
        uart_dispatch_line("[SUBGHZ_EXT_ERR] reason=license");
        return;
    }
    if (st != SUBGHZ_EXT_OK) {
        return;
    }
    int idx = store_capture(timings, count, freq, &dec, false);
    if (idx > 0) {
        inject_rx(idx, &dec, freq, count);
    }
}

static void cap_rx_task(void *arg)
{
    (void)arg;
    int32_t *scratch = malloc(CC1101_CAP_RAW_BUF_SIZE * sizeof(int32_t));
    if (!scratch) {
        ESP_LOGE(TAG, "RX scratch alloc failed");
        s_rx_running = false;
        s_rx_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    cc1101_cap_set_frequency(s_rx_freq);
    cc1101_cap_enter_rx();
    cc1101_cap_capture_start();

    int rssi_ticks = 0;
    int peak_rssi = -127;

    while (!s_rx_stop) {
        int rssi = cc1101_cap_get_rssi();
        if (rssi > peak_rssi) {
            peak_rssi = rssi;
        }
        if (++rssi_ticks >= CAP_RSSI_HZ_TICKS) {
            rssi_ticks = 0;
            inject_rssi(rssi);
        }

        int count = cc1101_cap_capture_count();
        if (count >= CAP_MIN_EDGES) {
            int64_t silence = esp_timer_get_time() - cc1101_cap_last_edge_us();
            if (silence >= CAP_SILENCE_US) {
                cc1101_cap_capture_stop();
                int n = cc1101_cap_copy_timings(scratch, CC1101_CAP_RAW_BUF_SIZE);
                int burst_peak = peak_rssi;
                peak_rssi = -127;

                if (n >= CAP_MIN_EDGES && burst_peak >= s_rx_min_rssi) {
                    process_burst(scratch, n, s_rx_freq, s_rx_raw);
                }

                if (s_rx_stop) {
                    break;
                }
                cc1101_cap_enter_rx();
                cc1101_cap_capture_start();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    cc1101_cap_capture_stop();
    cc1101_cap_idle();
    free(scratch);
    s_rx_running = false;
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t subghz_cap_radio_enable(void)
{
    ensure_locks();
    return cc1101_cap_init();
}

void subghz_cap_radio_disable(void)
{
    subghz_cap_listen_stop();
    subghz_cap_jam_stop();
    cc1101_cap_idle();
}

bool subghz_cap_radio_ready(void)
{
    return cc1101_cap_is_initialized();
}

esp_err_t subghz_cap_listen_start(float freq_mhz, bool raw_mode, int min_rssi_dbm)
{
    ensure_locks();
    if (!cc1101_cap_is_initialized()) {
        esp_err_t err = cc1101_cap_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_rx_running) {
        return ESP_OK;
    }

    s_rx_freq = freq_mhz;
    s_rx_raw = raw_mode;
    s_rx_min_rssi = min_rssi_dbm;
    s_rx_stop = false;
    s_rx_running = true;
    BaseType_t ok = xTaskCreate(cap_rx_task, "cap_rx", 6144, NULL, 5, &s_rx_task);
    if (ok != pdPASS) {
        s_rx_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void subghz_cap_listen_stop(void)
{
    if (!s_rx_running) {
        return;
    }
    s_rx_stop = true;
    for (int i = 0; i < 50 && s_rx_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_rx_task) {
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
        cc1101_cap_capture_stop();
        s_rx_running = false;
    }
}

bool subghz_cap_listen_running(void)
{
    return s_rx_running;
}

static void build_encode_cmd(const cap_capture_t *c, int cnt, char *cmd, size_t len)
{
    const char *proto = c->proto[0] ? c->proto : c->type;
    if (c->rolling) {
        snprintf(cmd, len,
                 "subghz_ext_encode %s serial=%s cnt=%d btn=%d mf=%s learn=%s seed=0x%lX bits=%d",
                 proto,
                 c->serial[0] ? c->serial : "0x0",
                 cnt,
                 c->btn,
                 c->mf[0] ? c->mf : "-",
                 c->learn[0] ? c->learn : "UNKNOWN",
                 (unsigned long)c->seed,
                 c->bits);
    } else {
        snprintf(cmd, len,
                 "subghz_ext_encode %s key=0x%s bits=%d te=%d",
                 proto,
                 c->id[0] ? c->id : "0",
                 c->bits > 0 ? c->bits : 24,
                 c->te);
    }
}

esp_err_t subghz_cap_save(int local_idx)
{
    ensure_locks();
    xSemaphoreTake(s_op_mtx, portMAX_DELAY);
    xSemaphoreTake(s_cap_mtx, portMAX_DELAY);
    cap_capture_t *c = cap_find(local_idx);
    if (!c || !c->timings) {
        xSemaphoreGive(s_cap_mtx);
        xSemaphoreGive(s_op_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    int32_t *copy = malloc((size_t)c->timing_count * sizeof(int32_t));
    int n = c->timing_count;
    float freq = c->freq;
    if (copy) {
        memcpy(copy, c->timings, (size_t)n * sizeof(int32_t));
    }
    xSemaphoreGive(s_cap_mtx);
    if (!copy) {
        xSemaphoreGive(s_op_mtx);
        return ESP_ERR_NO_MEM;
    }

    subghz_ext_decode_t dec;
    subghz_ext_status_t st = subghz_ext_decode(copy, n, freq, &dec);
    free(copy);
    if (st != SUBGHZ_EXT_OK) {
        xSemaphoreGive(s_op_mtx);
        return ESP_FAIL;
    }
    st = subghz_ext_save(NULL);
    xSemaphoreGive(s_op_mtx);
    return (st == SUBGHZ_EXT_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t subghz_cap_tx_capture(int local_idx)
{
    ensure_locks();
    xSemaphoreTake(s_op_mtx, portMAX_DELAY);
    xSemaphoreTake(s_cap_mtx, portMAX_DELAY);
    cap_capture_t *c = cap_find(local_idx);
    if (!c) {
        xSemaphoreGive(s_cap_mtx);
        xSemaphoreGive(s_op_mtx);
        return ESP_ERR_NOT_FOUND;
    }

    bool raw = c->is_raw;
    float freq = c->freq;
    int cnt_tx = c->cnt + (c->rolling ? 1 : 0);
    char cmd[256];
    build_encode_cmd(c, cnt_tx, cmd, sizeof(cmd));

    int32_t *raw_copy = NULL;
    int raw_n = 0;
    if (raw && c->timings) {
        raw_n = c->timing_count;
        raw_copy = malloc((size_t)raw_n * sizeof(int32_t));
        if (raw_copy) {
            memcpy(raw_copy, c->timings, (size_t)raw_n * sizeof(int32_t));
        }
    }
    xSemaphoreGive(s_cap_mtx);

    bool was_rx = s_rx_running;
    if (was_rx) {
        cc1101_cap_capture_stop();
    }

    esp_err_t err = ESP_FAIL;
    if (raw && raw_copy) {
        err = cc1101_cap_tx_timings(freq, raw_copy, raw_n);
    } else {
        int32_t *enc = malloc(SUBGHZ_EXT_MAX_TIMINGS * sizeof(int32_t));
        int enc_n = 0;
        if (enc && subghz_ext_encode(cmd, enc, &enc_n) == SUBGHZ_EXT_OK) {
            err = cc1101_cap_tx_timings(freq, enc, enc_n);
            if (err == ESP_OK && cnt_tx > 0) {
                xSemaphoreTake(s_cap_mtx, portMAX_DELAY);
                cap_capture_t *again = cap_find(local_idx);
                if (again && again->rolling) {
                    again->cnt = cnt_tx;
                }
                xSemaphoreGive(s_cap_mtx);
            }
        }
        free(enc);
    }
    free(raw_copy);

    if (was_rx && s_rx_running) {
        cc1101_cap_enter_rx();
        cc1101_cap_capture_start();
    }
    xSemaphoreGive(s_op_mtx);
    return err;
}

esp_err_t subghz_cap_tx_sd(int sd_idx, const char *name)
{
    ensure_locks();
    xSemaphoreTake(s_op_mtx, portMAX_DELAY);

    int32_t *enc = malloc(SUBGHZ_EXT_MAX_TIMINGS * sizeof(int32_t));
    int enc_n = 0;
    float freq = 433.92f;
    int reps = 3;
    subghz_ext_status_t st = SUBGHZ_EXT_ERR_ENCODE;
    if (enc) {
        st = subghz_ext_tx_sd(sd_idx, name, enc, &enc_n, &freq, &reps);
    }

    esp_err_t err = ESP_FAIL;
    if (st == SUBGHZ_EXT_OK && enc && enc_n >= 2) {
        if (freq < 0.1f) {
            freq = 433.92f;
        }
        ESP_LOGI(TAG, "ext_tx sd idx=%d name=%s edges=%d freq=%.2f reps=%d",
                 sd_idx, name && name[0] ? name : "-", enc_n, freq, reps);
        err = cc1101_cap_tx_timings_reps(freq, enc, enc_n, reps);
    }
    free(enc);

    xSemaphoreGive(s_op_mtx);
    return err;
}

esp_err_t subghz_cap_jam_start(float freq_mhz)
{
    ensure_locks();
    subghz_cap_listen_stop();
    return cc1101_cap_jam_start(freq_mhz);
}

void subghz_cap_jam_stop(void)
{
    cc1101_cap_jam_stop();
}

esp_err_t subghz_cap_tx_tesla(void)
{
    ensure_locks();
    xSemaphoreTake(s_op_mtx, portMAX_DELAY);
    if (!cc1101_cap_is_initialized()) {
        esp_err_t err = cc1101_cap_init();
        if (err != ESP_OK) {
            xSemaphoreGive(s_op_mtx);
            return err;
        }
    }
    bool was_rx = s_rx_running;
    if (was_rx) {
        cc1101_cap_capture_stop();
    }
    esp_err_t err = cc1101_cap_tx_timings(315.00f, subghz_tesla_charge_edges,
                                          subghz_tesla_charge_edge_count);
    if (was_rx && s_rx_running) {
        cc1101_cap_enter_rx();
        cc1101_cap_capture_start();
    }
    xSemaphoreGive(s_op_mtx);
    return err;
}
