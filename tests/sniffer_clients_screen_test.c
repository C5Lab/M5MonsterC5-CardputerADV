/* Compile the real screen with host UI/UART/RTOS doubles. */
#include "screen_manager.h"
#include "uart_handler.h"
#include "station_deauth_screen.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static int64_t now;
static unsigned sent_count,push_count,pop_count,beeps;
static char commands[32][100];
static void (*callback)(const char*,void*);
static void *callback_data;
static bool dispatching;
static station_deauth_params_t accepted;
int64_t esp_timer_get_time(void){return now;}
void uart_register_line_callback(void (*cb)(const char*,void*),void *d){assert(!dispatching);callback=cb;callback_data=d;}
void uart_clear_line_callback(void){assert(!dispatching);callback=NULL;}
int uart_send_command(const char *cmd){assert(sent_count<32);snprintf(commands[sent_count++],100,"%s",cmd);return ESP_OK;}
screen_t *screen_alloc(void){return calloc(1,sizeof(screen_t));}
int screen_manager_push(screen_t *(*fn)(void*),void *p){assert(!dispatching);++push_count;accepted=*(station_deauth_params_t*)p;free(p);return ESP_OK;}
int screen_manager_pop(void){++pop_count;return ESP_OK;}
screen_t *station_deauth_screen_create(void *p){return NULL;}
void buzzer_beep_attack(void){++beeps;}
static void rx(const char *line){assert(callback);dispatching=true;callback(line,callback_data);dispatching=false;}
#include "../main/screens/sniffer_results_screen.c"
static void list(void){
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_clients_begin\",\"aps\":1,\"clients\":1}");
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_ap\",\"bssid\":\"02:00:00:00:00:01\",\"channel\":6,\"ssid_hex\":\"5061646479735456\"}");
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_client\",\"bssid\":\"02:00:00:00:00:01\",\"station\":\"04:00:00:00:00:02\"}");
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_clients_end\",\"aps\":1,\"clients\":1}");
}
static void list_grouped(void){
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_clients_begin\",\"aps\":2,\"clients\":3}");
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_ap\",\"bssid\":\"02:00:00:00:00:01\",\"channel\":6,\"ssid_hex\":\"5061646479735456\"}");
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_client\",\"bssid\":\"02:00:00:00:00:01\",\"station\":\"04:00:00:00:00:02\"}");
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_client\",\"bssid\":\"02:00:00:00:00:01\",\"station\":\"04:00:00:00:00:03\"}");
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_ap\",\"bssid\":\"02:00:00:00:00:02\",\"channel\":11,\"ssid_hex\":\"\"}");
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_client\",\"bssid\":\"02:00:00:00:00:02\",\"station\":\"04:00:00:00:00:04\"}");
 rx("{\"schema_version\":1,\"record_type\":\"sniffer_clients_end\",\"aps\":2,\"clients\":3}");
}
static const char *ack="{\"schema_version\":1,\"record_type\":\"station_deauth\",\"status\":\"started\",\"bssid\":\"02:00:00:00:00:01\",\"station\":\"04:00:00:00:00:02\"}";
static const char *refused="{\"schema_version\":1,\"record_type\":\"station_deauth\",\"status\":\"error\",\"reason\":\"channel_unknown\"}";
int main(void){
 screen_t *g=sniffer_results_screen_create(NULL);assert(g && sent_count==1 && !strcmp(commands[0],"show_sniffer_clients"));
 list_grouped();
 g->on_key(g,KEY_D);
 assert(sent_count==2 && !strcmp(commands[1],"start_deauth_station 02:00:00:00:00:01 04:00:00:00:00:02"));
 rx(refused);g->on_tick(g);
 g->on_key(g,KEY_DOWN);g->on_key(g,KEY_D);
 assert(sent_count==3 && !strcmp(commands[2],"start_deauth_station 02:00:00:00:00:01 04:00:00:00:00:03"));
 g->on_key(g,KEY_ESC);g->on_destroy(g);free(g);
 sent_count=0;push_count=0;pop_count=0;beeps=0;now=0;callback=NULL;
 screen_t *s=sniffer_results_screen_create(NULL);assert(s && sent_count==1 && !strcmp(commands[0],"show_sniffer_clients"));
 s->on_key(s,KEY_D);assert(sent_count==1);list();
 s->on_key(s,KEY_D);assert(sent_count==2 && !strcmp(commands[1],"start_deauth_station 02:00:00:00:00:01 04:00:00:00:00:02"));
 s->on_key(s,KEY_D);assert(sent_count==2 && push_count==0);
 rx(ack);assert(push_count==0);s->on_tick(s);assert(push_count==1 && beeps==1);
 assert(!strcmp(accepted.ssid,"PaddysTV") && !strcmp(accepted.bssid,"02:00:00:00:00:01"));
 s->on_resume(s);s->on_key(s,KEY_D);
 unsigned before=sent_count;now+=START_TIMEOUT_US+1;s->on_tick(s);
 assert(sent_count==before+1 && !strcmp(commands[sent_count-1],"stop"));
 rx(ack);s->on_tick(s);assert(push_count==1);
 s->on_key(s,KEY_D);assert(sent_count==before+1); /* Must reload after timeout: late ack cannot restart. */
 s->on_key(s,KEY_R);assert(!strcmp(commands[sent_count-1],"show_sniffer_clients"));rx(ack);list();
 s->on_key(s,KEY_D);rx("{\"schema_version\":1,\"record_type\":\"station_deauth\",\"status\":\"error\",\"reason\":\"channel_unknown\"}");
 s->on_tick(s);assert(push_count==1);
 s->on_key(s,KEY_D);before=sent_count;s->on_key(s,KEY_ESC);
 assert(pop_count==1 && sent_count==before+1 && !strcmp(commands[sent_count-1],"stop"));
 s->on_destroy(s);free(s);puts("ADV screen lifecycle PASS");
}
