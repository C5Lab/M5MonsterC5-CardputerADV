"""Host tests for JanOS radio-busy handling in the Cardputer client."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EMPTY_HEADERS = (
    "esp_err.h", "esp_log.h", "esp_system.h", "esp_heap_caps.h", "esp_timer.h",
    "driver/uart.h", "freertos/FreeRTOS.h", "freertos/task.h", "freertos/semphr.h",
)
SCREEN_HEADERS = {
    "screen_manager.h": """#pragma once
#include <stdbool.h>
#define ESP_OK 0
typedef int esp_err_t;
typedef int key_code_t;
enum {KEY_ESC=1, KEY_Q};
typedef struct screen screen_t;
struct screen { void *user_data; void (*on_key)(screen_t *, key_code_t);
    void (*on_destroy)(screen_t *), (*on_draw)(screen_t *); };
screen_t *screen_alloc(void);
int screen_manager_replace(screen_t *(*create)(void *), void *params);
int screen_manager_pop(void);
""",
    "uart_handler.h": """#pragma once
#include <stdbool.h>
typedef struct { char ssid[33]; } wifi_network_t;
typedef void (*uart_scan_complete_callback_t)(wifi_network_t *, int, void *);
int uart_start_wifi_scan(uart_scan_complete_callback_t cb, void *data);
bool uart_is_scanning(void);
const char *uart_get_scan_status(void);
""",
    "text_ui.h": """#pragma once
#define UI_COLOR_BG 0
#define UI_COLOR_TEXT 1
#define DISPLAY_WIDTH 240
void ui_clear(void);
void ui_draw_title(const char *title);
void ui_print_center(int row, const char *text, int color);
void ui_draw_status(const char *text);
void display_fill_rect(int x, int y, int w, int h, int color);
""",
    "esp_timer.h": """#pragma once
typedef void *esp_timer_handle_t;
typedef struct {void (*callback)(void *); void *arg; const char *name;} esp_timer_create_args_t;
int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *handle);
int esp_timer_start_periodic(esp_timer_handle_t handle, int us);
int esp_timer_stop(esp_timer_handle_t handle);
int esp_timer_delete(esp_timer_handle_t handle);
""",
    "esp_log.h": "#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n",
}
SNIFFER_HEADERS = {
    "screen_manager.h": """#pragma once
#include <stdbool.h>
#define ESP_OK 0
#define ESP_FAIL -1
typedef int key_code_t;
enum {KEY_R=1, KEY_P, KEY_ESC, KEY_Q, KEY_BACKSPACE, KEY_S};
typedef struct screen screen_t;
struct screen {void *user_data; void (*on_key)(screen_t *, key_code_t);
    void (*on_destroy)(screen_t *), (*on_draw)(screen_t *),
         (*on_tick)(screen_t *), (*on_resume)(screen_t *);};
screen_t *screen_alloc(void);
int screen_manager_push(screen_t *(*create)(void *), void *params);
int screen_manager_pop(void);
""",
    "uart_handler.h": """#pragma once
typedef struct {char ssid[33];} wifi_network_t;
int uart_send_command(const char *cmd);
void uart_register_line_callback(void (*cb)(const char *, void *), void *data);
void uart_clear_line_callback(void);
""",
    "text_ui.h": """#pragma once
#define UI_COLOR_HIGHLIGHT 1
#define UI_COLOR_TEXT 2
void ui_clear(void);
void ui_draw_title(const char *title);
void ui_print_center(int row, const char *text, int color);
void ui_draw_status(const char *text);
""",
    "buzzer.h": "#pragma once\nvoid buzzer_beep_attack(void);\n",
    "esp_log.h": "#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
}


def wsl_path(path):
    path = Path(path).resolve()
    return f"/mnt/{path.drive[0].lower()}/{path.relative_to(path.anchor).as_posix()}"


class BusyRadioTests(unittest.TestCase):
    def test_scan_busy_does_not_complete_from_cached_results(self):
        with tempfile.TemporaryDirectory(prefix="cardputer-busy-") as tmp:
            directory = Path(tmp)
            for name in EMPTY_HEADERS:
                header = directory / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text("#pragma once\n", encoding="utf-8")
            source = ROOT / "tests/uart_busy_scan_test.c"
            output = directory / "uart_busy_scan"
            if shutil.which("cc"):
                compile_cmd = ["cc", "-std=gnu11", "-I", str(directory), str(source), "-o", str(output)]
                run_cmd = [str(output)]
            else:
                self.assertIsNotNone(shutil.which("wsl.exe"), "C compiler required")
                compile_cmd = ["wsl.exe", "cc", "-std=gnu11", "-I", wsl_path(directory),
                               wsl_path(source), "-o", wsl_path(output)]
                run_cmd = ["wsl.exe", wsl_path(output)]
            built = subprocess.run(compile_cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run(run_cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("UART busy scan PASS", result.stdout)

    def test_scan_screen_displays_radio_busy(self):
        with tempfile.TemporaryDirectory(prefix="cardputer-screen-") as tmp:
            directory = Path(tmp)
            for name, content in SCREEN_HEADERS.items():
                header = directory / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(content, encoding="utf-8")
            source = ROOT / "tests/wifi_scan_busy_screen_test.c"
            output = directory / "wifi_scan_busy"
            if shutil.which("cc"):
                compile_cmd = ["cc", "-std=gnu11", "-I", str(directory), str(source), "-o", str(output)]
                run_cmd = [str(output)]
            else:
                self.assertIsNotNone(shutil.which("wsl.exe"), "C compiler required")
                compile_cmd = ["wsl.exe", "cc", "-std=gnu11", "-I", wsl_path(directory),
                               wsl_path(source), "-o", wsl_path(output)]
                run_cmd = ["wsl.exe", wsl_path(output)]
            built = subprocess.run(compile_cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run(run_cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("WiFi scan busy screen PASS", result.stdout)

    def test_sniffer_screens_report_refused_start_without_stopping_wps(self):
        for variant in ("global", "selected"):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory(prefix="sniffer-busy-") as tmp:
                directory = Path(tmp)
                for name, content in SNIFFER_HEADERS.items():
                    header = directory / name
                    header.parent.mkdir(parents=True, exist_ok=True)
                    header.write_text(content, encoding="utf-8")
                source = ROOT / "tests/sniffer_busy_screen_test.c"
                output = directory / "sniffer_busy"
                variant_flag = ["-DTEST_GLOBAL"] if variant == "global" else []
                if shutil.which("cc"):
                    compile_cmd = ["cc", "-std=gnu11", *variant_flag, "-I", str(directory),
                                   str(source), "-o", str(output)]
                    run_cmd = [str(output)]
                else:
                    self.assertIsNotNone(shutil.which("wsl.exe"), "C compiler required")
                    compile_cmd = ["wsl.exe", "cc", "-std=gnu11", *variant_flag,
                                   "-I", wsl_path(directory), wsl_path(source), "-o", wsl_path(output)]
                    run_cmd = ["wsl.exe", wsl_path(output)]
                built = subprocess.run(compile_cmd, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                result = subprocess.run(run_cmd, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Sniffer busy screen PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
