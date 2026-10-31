"""Compile and exercise the real ADV detector screen with host UI/UART stubs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    "screen_manager.h": """#pragma once
#include <stdbool.h>
typedef enum { KEY_ESC, KEY_Q, KEY_S, KEY_A, KEY_C, KEY_W, KEY_R,
 KEY_UP, KEY_DOWN, KEY_SPACE, KEY_ENTER } key_code_t;
typedef struct screen screen_t;
struct screen { void *user_data; void (*on_key)(screen_t*, key_code_t);
 void (*on_destroy)(screen_t*); void (*on_draw)(screen_t*); void (*on_tick)(screen_t*); };
screen_t *screen_alloc(void);
void screen_manager_pop(void);
""",
    "uart_handler.h": """#pragma once
int uart_send_command(const char *command);
void uart_register_line_callback(void (*callback)(const char*, void*), void *data);
void uart_clear_line_callback(void);
""",
    "text_ui.h": """#pragma once
#define UI_COLOR_TEXT 1
#define UI_COLOR_HIGHLIGHT 2
#define UI_COLOR_DIMMED 3
void ui_clear(void);
void ui_draw_title(const char *s);
void ui_print(int x, int y, const char *s, int color);
void ui_print_center(int y, const char *s, int color);
void ui_draw_status(const char *s);
void ui_draw_progress(int row, int progress, const char *text);
""",
    "esp_log.h": "#pragma once\n#define ESP_LOGI(tag, ...) ((void)(tag))\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
}


class DeauthDetectorTests(unittest.TestCase):
    def test_uart_records(self):
        with tempfile.TemporaryDirectory(prefix="deauth-host-") as temporary:
            directory = Path(temporary)
            for name, contents in STUBS.items():
                (directory / name).write_text(contents, encoding="utf-8")
            compiler = shutil.which("cc")
            self.assertIsNotNone(compiler, "Run this host test under WSL when Windows has no cc")
            executable = directory / "deauth_detector_test"
            subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter",
                "-I", str(directory), "-I", str(ROOT / "main/screens"),
                str(ROOT / "tests/deauth_detector_screen_test.c"),
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
