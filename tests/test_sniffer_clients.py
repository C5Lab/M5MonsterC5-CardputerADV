"""Host tests for ADV's bounded UART identity model. No ESP-IDF required."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class ModelTests(unittest.TestCase):
 def test_real_model(self):
  with tempfile.TemporaryDirectory() as tmp:
   executable=str(Path(tmp)/'clients')
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main/screens'),str(ROOT/'main/screens/sniffer_clients_model.c'),str(ROOT/'tests/sniffer_clients_model_test.c'),'-o',executable],check=True)
   self.assertIn('ADV identity model PASS',subprocess.check_output([executable],text=True))

STUB_HEADERS={
 'screen_manager.h':'''#pragma once
#include <stdint.h>
#include <stdbool.h>
#define ESP_OK 0
#define ESP_FAIL -1
typedef int key_code_t;
enum {KEY_UP,KEY_DOWN,KEY_D,KEY_R,KEY_ESC,KEY_Q,KEY_BACKSPACE};
typedef struct screen screen_t;
struct screen {void *user_data; void (*on_draw)(screen_t*),(*on_tick)(screen_t*),(*on_destroy)(screen_t*),(*on_resume)(screen_t*);void (*on_key)(screen_t*,key_code_t);};
screen_t *screen_alloc(void);
int screen_manager_push(screen_t *(*fn)(void*),void *p);
int screen_manager_pop(void);
''',
 'uart_handler.h':'''#pragma once
void uart_register_line_callback(void (*cb)(const char*,void*),void *d);
void uart_clear_line_callback(void);
int uart_send_command(const char *cmd);
''',
 'text_ui.h':'''#pragma once
#define UI_COLOR_DIMMED 0
#define UI_COLOR_TEXT 1
#define UI_COLOR_HIGHLIGHT 2
#define UI_COLOR_SELECTED 3
static void ui_clear(void){}
static void ui_draw_title(const char *s){}
static void ui_print_center(int r,const char *s,int c){}
static void ui_print(int x,int y,const char *s,int c){}
static void ui_draw_status(const char *s){}
''',
 'buzzer.h':'#pragma once\nvoid buzzer_beep_attack(void);\n',
 'esp_timer.h':'#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
 'freertos/FreeRTOS.h':'#pragma once\n#define portMAX_DELAY 1\n',
 'freertos/semphr.h':'''#pragma once
#include <stdlib.h>
typedef void *SemaphoreHandle_t;
static void *xSemaphoreCreateMutex(void){return malloc(1);}
static int xSemaphoreTake(void *m,int wait){return 1;}
static void xSemaphoreGive(void *m){}
static void vSemaphoreDelete(void *m){free(m);}
''',
}
class ScreenTests(unittest.TestCase):
 def test_ack_timeout_cancel_and_resume(self):
  with tempfile.TemporaryDirectory() as tmp:
   directory=Path(tmp)
   for name,content in STUB_HEADERS.items():
    p=directory/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content)
   executable=str(directory/'screen')
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-I',str(directory),'-I',str(ROOT/'main/screens'),str(ROOT/'main/screens/sniffer_clients_model.c'),str(ROOT/'tests/sniffer_clients_screen_test.c'),'-o',executable],check=True)
   self.assertIn('ADV screen lifecycle PASS',subprocess.check_output([executable],text=True))

class WireTests(unittest.TestCase):
 def test_janos_to_adv(self):
  import os
  import sys
  import importlib.util
  janos=os.environ.get('JANOS_SOURCE_ROOT')
  if not janos:self.skipTest('Set JANOS_SOURCE_ROOT to ESP32C5 for cross-repository wire test')
  test_file=Path(janos)/'tests/test_sniffer_clients.py'
  sys.path.insert(0,str(test_file.parent))
  spec=importlib.util.spec_from_file_location('janos_identity_fixture',test_file)
  module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
  fixture=module.IdentityTests
  fixture.setUpClass()
  try:
   wire=subprocess.check_output([str(fixture.exe),'list'])
   with tempfile.TemporaryDirectory() as tmp:
    executable=str(Path(tmp)/'wire')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main/screens'),str(ROOT/'main/screens/sniffer_clients_model.c'),str(ROOT/'tests/sniffer_clients_wire_test.c'),'-o',executable],check=True)
    self.assertIn(b'wire contract PASS',subprocess.check_output([executable],input=wire))
  finally:
   fixture.tearDownClass();sys.path.pop(0)
