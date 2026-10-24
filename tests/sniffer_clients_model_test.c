#include "sniffer_clients_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static sc_event_t feed(sc_model_t *m,const char *s) {return sc_model_line(m,s,NULL,NULL);}
static void begin(sc_model_t *m,unsigned aps,unsigned clients) {
 char line[160];snprintf(line,sizeof(line),"{\"schema_version\":1,\"record_type\":\"sniffer_clients_begin\",\"aps\":%u,\"clients\":%u}",aps,clients);assert(feed(m,line)==SC_UPDATED);
}
static void ap(sc_model_t *m,unsigned i) {
 char line[240];snprintf(line,sizeof(line),"{\"schema_version\":1,\"record_type\":\"sniffer_ap\",\"bssid\":\"02:00:00:00:00:%02X\",\"channel\":48,\"ssid_hex\":\"5061646479735456\",\"ssid_source\":\"scan\"}",i);assert(feed(m,line)==SC_UPDATED);
}
static void client(sc_model_t *m,unsigned a,unsigned i) {
 char line[200];snprintf(line,sizeof(line),"{\"schema_version\":1,\"record_type\":\"sniffer_client\",\"bssid\":\"02:00:00:00:00:%02X\",\"station\":\"04:00:00:00:%02X:%02X\"}",a,a,i);assert(feed(m,line)==SC_UPDATED);
}
int main(void) {
 sc_model_t m={0};begin(&m,2,2);ap(&m,1);client(&m,1,1);ap(&m,2);client(&m,2,2);
 assert(feed(&m,"{\"schema_version\":1,\"record_type\":\"sniffer_clients_end\",\"aps\":2,\"clients\":2}")==SC_UPDATED);
 assert(m.complete && !m.failed && m.clients[0].ap==0 && m.clients[1].ap==1);
 assert(!memcmp(m.aps[0].ssid,"PaddysTV",8));
 uint8_t pending_ap[6]={2,0,0,0,0,2}, pending_sta[6]={4,0,0,0,2,2};
 const char *ack="{\"schema_version\":1,\"record_type\":\"station_deauth\",\"status\":\"started\",\"bssid\":\"02:00:00:00:00:02\",\"station\":\"04:00:00:00:02:02\"}";
 assert(sc_model_line(&m,ack,pending_ap,pending_sta)==SC_STARTED);
 pending_ap[5]=1;assert(sc_model_line(&m,ack,pending_ap,pending_sta)==SC_IGNORED);
 assert(sc_model_line(&m,ack,NULL,NULL)==SC_IGNORED);
 assert(sc_model_line(&m,"{\"schema_version\":1,\"record_type\":\"station_deauth\",\"status\":\"error\",\"reason\":\"channel_unknown\"}",pending_ap,pending_sta)==SC_REFUSED);
 assert(!strcmp(m.error,"channel_unknown"));
 assert(sc_model_line(&m,"[WFACTL1] {\"v\":1,\"type\":\"error\",\"command\":\"console\",\"code\":\"busy\",\"reason\":\"radio_lease\"}",pending_ap,pending_sta)==SC_REFUSED);
 assert(!strcmp(m.error,"Radio busy"));
 sc_model_free(&m);begin(&m,100,5000);
 for(unsigned a=1;a<=100;++a) {ap(&m,a);for(unsigned c=1;c<=50;++c) client(&m,a,c);}
 assert(feed(&m,"{\"schema_version\":1,\"record_type\":\"sniffer_clients_end\",\"aps\":100,\"clients\":5000}")==SC_UPDATED);
 assert(m.complete && m.client_count==5000 && m.clients[4999].ap==99);
 sc_model_free(&m);begin(&m,1,1);ap(&m,1);
 feed(&m,"{\"schema_version\":1,\"record_type\":\"sniffer_clients_end\",\"aps\":1,\"clients\":1}");assert(m.failed && !m.complete);
 sc_model_free(&m);begin(&m,0,0);
 assert(feed(&m,"{\"schema_version\":1,\"record_type\":\"sniffer_clients_end\",\"aps\":0,\"clients\":0}")==SC_UPDATED && m.complete);
 sc_model_free(&m);begin(&m,101,0);assert(m.failed);
 sc_model_free(&m);begin(&m,1,1);
 assert(feed(&m,"{\"schema_version\":1,\"record_type\":\"sniffer_ap\",\"bssid\":\"02:00:00:00:00:01\",\"channel\":6,\"ssid_hex\":\"GG\"}")==SC_UPDATED && m.failed);
 sc_model_free(&m);
 assert(feed(&m,"log {\"schema_version\":1,\"record_type\":\"sniffer_clients_begin\",\"aps\":1,\"clients\":1}")==SC_IGNORED);
 assert(feed(&m,"{\"schema_version\":1,\"schema_version\":1,\"record_type\":\"sniffer_clients_begin\",\"aps\":1,\"clients\":1}")==SC_IGNORED);
 assert(feed(&m,"{\"schema_version\":1,\"record_type\":\"sniffer_clients_begin\",\"aps\":1,\"clients\":1,}")==SC_IGNORED);
 assert(feed(&m,"{\"schema_version\":2,\"record_type\":\"sniffer_clients_begin\",\"aps\":1,\"clients\":1}")==SC_IGNORED);
 sc_ap_t hidden={.bssid={2,0,0,0,0,1},.channel=132};char label[40];sc_ap_label(&hidden,label,sizeof(label));assert(!strcmp(label,"02:00:00:00:00:01 CH132"));
 hidden.ssid_len=6;memcpy(hidden.ssid,"a,\"\n\0z",6);sc_ap_label(&hidden,label,sizeof(label));assert(!strcmp(label,"a,\"??z"));
 sc_model_free(&m);puts("ADV identity model PASS");
}
