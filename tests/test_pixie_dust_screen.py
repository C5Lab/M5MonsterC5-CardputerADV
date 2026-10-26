"""Host exercise of the real Pixie screen's UART and navigation boundary."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    "screen_manager.h": """#pragma once
#include <stdint.h>
#include <stdbool.h>
#define ESP_OK 0
#define ESP_FAIL -1
typedef int key_code_t;
enum {KEY_ESC=1, KEY_Q, KEY_BACKSPACE, KEY_ENTER};
typedef struct screen screen_t;
struct screen { void *user_data; void (*on_key)(screen_t*,key_code_t);
 void (*on_destroy)(screen_t*); void (*on_resume)(screen_t*);
 void (*on_draw)(screen_t*); void (*on_tick)(screen_t*); };
screen_t *screen_alloc(void);
int screen_manager_pop(void);
""",
    "uart_handler.h": """#pragma once
#include <stdbool.h>
#include "screen_manager.h"
typedef struct {int id; char ssid[33],bssid[18]; int channel;
 char security[24]; int rssi; char band[8]; bool selected;} wifi_network_t;
void uart_register_line_callback(void (*cb)(const char*,void*),void *data);
void uart_clear_line_callback(void);
int uart_send_command(const char *cmd);
bool uart_janos_version_at_least(int major, int minor, int patch);
""",
    "text_ui.h": """#pragma once
#define UI_COLOR_TEXT 1
#define UI_COLOR_HIGHLIGHT 2
void ui_clear(void);
void ui_draw_title(const char *s);
void ui_print(int x,int y,const char *s,int color);
void ui_draw_status(const char *s);
""",
    "buzzer.h": "#pragma once\nvoid buzzer_beep_attack(void);\n",
    "esp_log.h": "#pragma once\n#define ESP_LOGE(tag, ...) ((void)(tag))\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "freertos/FreeRTOS.h": "#pragma once\n#define portMAX_DELAY 1\n",
    "freertos/semphr.h": """#pragma once
#include <stdlib.h>
typedef void *SemaphoreHandle_t;
static inline void *xSemaphoreCreateMutex(void){return malloc(1);}
static inline int xSemaphoreTake(void *m,int wait){(void)m;(void)wait;return 1;}
static inline void xSemaphoreGive(void *m){(void)m;}
static inline void vSemaphoreDelete(void *m){free(m);}
""",
}


def wsl_path(path):
    path = Path(path).resolve()
    return f"/mnt/{path.drive[0].lower()}/{path.relative_to(path.anchor).as_posix()}"


class PixieScreenTests(unittest.TestCase):
    def test_stop_waits_for_remote_completion(self):
        with tempfile.TemporaryDirectory(prefix="pixie-host-") as temporary:
            directory = Path(temporary)
            for name, contents in STUBS.items():
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents, encoding="utf-8")
            executable = directory / "pixie_screen_test"
            source = ROOT / "tests/pixie_dust_screen_test.c"
            compiler = shutil.which("cc")
            if compiler:
                command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                           "-Wno-unused-parameter", "-I", str(directory),
                           "-I", str(ROOT / "main/screens"), str(source),
                           "-o", str(executable)]
                run = [str(executable)]
            else:
                self.assertIsNotNone(shutil.which("wsl.exe"), "host C compiler required")
                command = ["wsl.exe", "cc", "-std=c11", "-Wall", "-Wextra",
                           "-Werror", "-Wno-unused-parameter", "-I", wsl_path(directory),
                           "-I", wsl_path(ROOT / "main/screens"), wsl_path(source),
                           "-o", wsl_path(executable)]
                run = ["wsl.exe", wsl_path(executable)]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run(run, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Pixie screen lifecycle PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
