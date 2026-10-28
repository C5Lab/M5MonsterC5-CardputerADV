/* Exercise the real UART scan handler with external ESP-IDF calls stubbed. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define UART_NUM_1 1
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdPASS 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)

typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef int BaseType_t;
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;

static int64_t fake_now;
static char last_tx[80];
static const char *next_response;
static bool fail_next_scan_send;
static void process_line(const char *line);
int settings_get_uart_tx_pin(void) { return 2; }
int settings_get_uart_rx_pin(void) { return 1; }
static inline size_t heap_caps_get_free_size(int cap) { (void)cap; return 65536; }
static inline size_t esp_get_free_heap_size(void) { return 65536; }
static inline const char *esp_err_to_name(int err) { (void)err; return "error"; }
static inline int64_t esp_timer_get_time(void) { return fake_now; }
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, int delay) { (void)s; (void)delay; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)2; }
static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                                     void *arg, unsigned priority, TaskHandle_t *handle)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; *handle = (void *)3; return pdPASS; }
static inline void vTaskDelay(int ticks)
{ (void)ticks; if (next_response &&
    (!strcmp(last_tx, "version") || !strcmp(last_tx, "wps status"))) {
      const char *line = next_response;
      next_response = NULL;
      process_line(line);
  } }
static inline int uart_param_config(int port, const uart_config_t *config)
{ (void)port; (void)config; return ESP_OK; }
static inline int uart_set_pin(int port, int tx, int rx, int rts, int cts)
{ (void)port; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK; }
static inline int uart_driver_install(int port, int rx, int tx, int queue, void *event, int flags)
{ (void)port; (void)rx; (void)tx; (void)queue; (void)event; (void)flags; return ESP_OK; }
static inline int uart_write_bytes(int port, const char *bytes, size_t size)
{ (void)port; if (size != 2 || memcmp(bytes, "\r\n", 2) != 0) {
      if (size < sizeof(last_tx)) { memcpy(last_tx, bytes, size); last_tx[size] = '\0'; }
      if (fail_next_scan_send && size == strlen("scan_networks") &&
          memcmp(bytes, "scan_networks", size) == 0) {
          fail_next_scan_send = false;
          return -1;
      }
  } return (int)size; }
static inline int uart_read_bytes(int port, void *bytes, size_t size, int delay)
{ (void)port; (void)bytes; (void)size; (void)delay; return 0; }
static inline int uart_get_buffered_data_len(int port, size_t *size)
{ (void)port; *size = 0; return ESP_OK; }

#include "../main/uart_handler.c"

static unsigned completions;
static int completed_count;
static wifi_network_t saved[8];
static int saved_count;
static void scan_done(wifi_network_t *result, int count, void *user_data)
{
    (void)user_data;
    ++completions;
    completed_count = count;
    saved_count = 0;
    if (result && count > 0) {
        int n = count < (int)(sizeof(saved) / sizeof(saved[0])) ? count
                : (int)(sizeof(saved) / sizeof(saved[0]));
        memcpy(saved, result, (size_t)n * sizeof(saved[0]));
        saved_count = n;
    }
}

int main(void)
{
    uart_mutex = (void *)1;
    assert(uart_start_wifi_scan(scan_done, NULL) == ESP_OK);
    assert(strcmp(last_tx, "scan_networks") == 0);
    // A delayed refusal must win even if old show_scan_results output arrives.
    process_line("\"1\",\"Old AP\",\"\",\"02:00:00:00:00:01\",\"6\",\"WPA2\",\"-45\",\"2.4GHz\"");
    process_line("Scan results printed.");
    assert(uart_is_scanning());
    assert(completions == 0);
    process_line("[WFACTL1] {\"v\":1,\"type\":\"error\",\"command\":\"console\",\"code\":\"busy\",\"reason\":\"radio_lease\"}");
    assert(!uart_is_scanning());
    assert(completions == 1 && completed_count == 0);
    assert(strstr(uart_get_scan_status(), "busy") != NULL);
    process_line("\"1\",\"Old AP\",\"\",\"02:00:00:00:00:01\",\"6\",\"WPA2\",\"-45\",\"2.4GHz\"");
    process_line("Scan results printed.");
    assert(completions == 1);

    assert(uart_start_wifi_scan(scan_done, NULL) == ESP_OK);
    process_line("Background scan started (min: 30 ms, max: 60 ms per channel)");
    fake_now += 6000000;
    scan_poll(fake_now);
    assert(uart_is_scanning());
    process_line("\"1\",\"Fresh AP\",\"\",\"02:00:00:00:00:02\",\"6\",\"WPA2\",\"-45\",\"2.4GHz\"");
    process_line("Scan results printed.");
    assert(!uart_is_scanning());
    assert(completions == 2 && completed_count == 1);
    assert(strcmp(uart_get_scan_status(), "Found 1 networks") == 0);

    fail_next_scan_send = true;
    assert(uart_start_wifi_scan(scan_done, NULL) != ESP_OK);
    assert(!uart_is_scanning());
    assert(completions == 3 && completed_count == 0);
    assert(strstr(uart_get_scan_status(), "unknown") != NULL);

    assert(uart_start_wifi_scan(scan_done, NULL) == ESP_OK);
    fake_now += 6000000;
    scan_poll(fake_now);
    assert(!uart_is_scanning());
    assert(completions == 4 && completed_count == 0);
    assert(strstr(uart_get_scan_status(), "unknown") != NULL);

    next_response = "JanOS version: 1.7.7";
    assert(uart_check_janos_version(100));
    assert(uart_janos_version_at_least(1, 7, 7));
    assert(!uart_janos_version_at_least(1, 7, 8));
    next_response = "JanOS version: 1.7.6";
    assert(uart_check_janos_version(100));
    assert(!uart_janos_version_at_least(1, 7, 7));
    assert(!uart_check_janos_version(20));
    assert(!uart_janos_version_at_least(1, 0, 0));

    next_response = "{\"schema_version\":1,\"record_type\":\"wps_status\",\"phase\":\"running\",\"run_id\":7}";
    assert(uart_probe_wps_busy(100) == 1);
    next_response = "{\"schema_version\":1,\"record_type\":\"wps_status\",\"phase\":\"idle\",\"run_id\":7}";
    assert(uart_probe_wps_busy(100) == 0);
    assert(uart_probe_wps_busy(20) == -1);

    assert(uart_start_wifi_scan(scan_done, NULL) == ESP_OK);
    process_line("No networks found in last scan.");
    assert(uart_is_scanning() && completions == 4); // old zero-result output
    process_line("Background scan started (min: 30 ms, max: 60 ms per channel)");
    process_line("No networks found in last scan.");
    assert(!uart_is_scanning());
    assert(completions == 5 && completed_count == 0);
    assert(strcmp(uart_get_scan_status(), "Found 0 networks") == 0);

    /* RFC 4180 quotes and commas in SSID/vendor, plus a plain line. */
    assert(uart_start_wifi_scan(scan_done, NULL) == ESP_OK);
    process_line("Background scan started (min: 30 ms, max: 60 ms per channel)");
    process_line("\"1\",\"moja \"\"specjalna\"\" siec\",\"Acme \"\"X\"\" Inc\",\"AA:BB:CC:DD:EE:FF\",\"6\",\"WPA2\",\"-50\",\"2.4GHz\"");
    process_line("\"2\",\"Plain AP\",\"\",\"02:00:00:00:00:02\",\"11\",\"WPA/WPA2 Mixed\",\"-72\",\"5GHz\"");
    process_line("\"3\",\"a,\"\"b\"\"\",\"Acme, \"\"X\"\"\",\"AA:BB:CC:DD:EE:03\",\"1\",\"Open\",\"-90\",\"2.4GHz\"");
    /* Legacy double-wrapped fields from the pre-RFC encoder. */
    process_line("\"4\",\"\"foo\"\"bar\"\",\"\"Acme \"\"X\"\" Inc\"\",\"AA:BB:CC:DD:EE:04\",\"6\",\"WPA2\",\"-40\",\"2.4GHz\"");
    /* Quote immediately before a comma in the legacy form must not crash or shift later rows. */
    process_line("\"8\",\"\"foo\"\",bar\"\",\"vendor\",\"AA:BB:CC:DD:EE:08\",\"6\",\"WPA2\",\"-40\",\"2.4GHz\"");

    char long_line[512];
    char ssid40[41];
    char vendor80[81];
    memset(ssid40, 'A', 40);
    ssid40[40] = '\0';
    memset(vendor80, 'V', 80);
    vendor80[80] = '\0';
    snprintf(long_line, sizeof(long_line),
             "\"5\",\"%s\",\"%s\",\"AA:BB:CC:DD:EE:05\",\"36\",\"WPA3\",\"-61\",\"5GHz\"",
             ssid40, vendor80);
    process_line(long_line);

    char quotes[33];
    char quoted_field[80];
    memset(quotes, '"', 32);
    quotes[32] = '\0';
    /* 32 quote bytes -> 64 quote characters inside one pair of enclosing quotes. */
    quoted_field[0] = '"';
    memcpy(quoted_field + 1, quotes, 32);
    memcpy(quoted_field + 33, quotes, 32);
    quoted_field[65] = '"';
    quoted_field[66] = '\0';
    snprintf(long_line, sizeof(long_line),
             "\"6\",%s,\"v\",\"AA:BB:CC:DD:EE:06\",\"6\",\"WPA2\",\"-55\",\"2.4GHz\"",
             quoted_field);
    process_line(long_line);

    process_line("Scan results printed.");
    assert(!uart_is_scanning());
    assert(completions == 6 && completed_count == 6 && saved_count == 6);

    assert(saved[0].id == 1);
    assert(strcmp(saved[0].ssid, "moja \"specjalna\" siec") == 0);
    assert(strcmp(saved[0].vendor, "Acme \"X\" Inc") == 0);
    assert(strcmp(saved[0].bssid, "AA:BB:CC:DD:EE:FF") == 0);
    assert(saved[0].channel == 6);
    assert(strcmp(saved[0].security, "WPA2") == 0);
    assert(saved[0].rssi == -50);
    assert(strcmp(saved[0].band, "2.4GHz") == 0);
    assert(!saved[0].selected);

    assert(strcmp(saved[1].ssid, "Plain AP") == 0);
    assert(saved[1].vendor[0] == '\0');
    assert(strcmp(saved[1].bssid, "02:00:00:00:00:02") == 0);
    assert(saved[1].channel == 11);
    assert(strcmp(saved[1].security, "WPA/WPA2 Mixed") == 0);
    assert(saved[1].rssi == -72);
    assert(strcmp(saved[1].band, "5GHz") == 0);

    assert(strcmp(saved[2].ssid, "a,\"b\"") == 0);
    assert(strcmp(saved[2].vendor, "Acme, \"X\"") == 0);
    assert(strcmp(saved[2].bssid, "AA:BB:CC:DD:EE:03") == 0);
    assert(saved[2].channel == 1);
    assert(strcmp(saved[2].security, "Open") == 0);
    assert(saved[2].rssi == -90);

    assert(strcmp(saved[3].ssid, "foo\"bar") == 0);
    assert(strcmp(saved[3].vendor, "Acme \"X\" Inc") == 0);
    assert(strcmp(saved[3].bssid, "AA:BB:CC:DD:EE:04") == 0);
    assert(saved[3].rssi == -40);

    assert(strlen(saved[4].ssid) == 32);
    assert(strspn(saved[4].ssid, "A") == 32);
    assert(saved[4].ssid[32] == '\0');
    assert(strlen(saved[4].vendor) == 60);
    assert(strspn(saved[4].vendor, "V") == 60);
    assert(saved[4].vendor[60] == '\0');
    assert(strcmp(saved[4].bssid, "AA:BB:CC:DD:EE:05") == 0);
    assert(saved[4].channel == 36);
    assert(strcmp(saved[4].security, "WPA3") == 0);
    assert(saved[4].rssi == -61);
    assert(strcmp(saved[4].band, "5GHz") == 0);

    assert(strlen(saved[5].ssid) == 32);
    assert(strspn(saved[5].ssid, "\"") == 32);
    assert(saved[5].ssid[32] == '\0');
    assert(strcmp(saved[5].vendor, "v") == 0);
    assert(strcmp(saved[5].bssid, "AA:BB:CC:DD:EE:06") == 0);
    assert(saved[5].channel == 6);
    assert(saved[5].rssi == -55);

    puts("UART busy scan PASS");
    return 0;
}
