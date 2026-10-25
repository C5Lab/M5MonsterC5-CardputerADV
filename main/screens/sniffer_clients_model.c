#include "sniffer_clients_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int unhex(char c)
{
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
bool sc_parse_mac(const char *text, uint8_t mac[6])
{
    if (strlen(text) != 17) return false;
    for (int i=0; i<6; ++i) {
        int a=unhex(text[i*3]), b=unhex(text[i*3+1]);
        if (a<0 || b<0 || (i<5 && text[i*3+2]!=':')) return false;
        mac[i]=(a<<4)|b;
    }
    static const uint8_t zero[6];
    return !(mac[0]&1) && memcmp(mac,zero,6);
}
void sc_mac_text(char out[18], const uint8_t mac[6])
{
    snprintf(out,18,"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}
/* This API uses flat JSON objects with ASCII identifiers, MACs and hex SSIDs.
 * Validate the entire object, delimiters, duplicate keys, and bounded tokens.
 * Never search for a field inside another value or a console/log prefix. */
typedef struct { char key[24], value[72]; bool string; } field_t;
static const char *space(const char *p) { while (isspace((unsigned char)*p)) ++p; return p; }
static const char *quoted(const char *p, char *out, size_t size)
{
    if (*p++!='"') return NULL;
    size_t n=0;
    while (*p && *p!='"') {
        if ((unsigned char)*p<32 || *p=='\\' || n+1>=size) return NULL;
        out[n++]=*p++;
    }
    if (*p!='"') return NULL;
    out[n]=0;
    return p+1;
}
static int object(const char *line, field_t fields[12])
{
    const char *p=space(line);
    if (*p++!='{') return -1;
    int count=0;
    for (;;) {
        p=space(p);
        if (*p=='}') return !*space(p+1) ? count : -1;
        if (count==12) return -1;
        field_t *f=&fields[count];
        p=quoted(p,f->key,sizeof(f->key));
        if (!p || *(p=space(p))!=':') return -1;
        for (int i=0;i<count;++i) if (!strcmp(fields[i].key,f->key)) return -1;
        p=space(p+1); f->string=*p=='"';
        if (f->string) p=quoted(p,f->value,sizeof(f->value));
        else {
            size_t n=0;
            while (*p && *p!=',' && *p!='}' && !isspace((unsigned char)*p)) {
                if (n+1>=sizeof(f->value) || *p<'0' || *p>'9') return -1;
                f->value[n++]=*p++;
            }
            f->value[n]=0;
            if (!n || (n>1 && f->value[0]=='0')) return -1;
        }
        if (!p) return -1;
        ++count; p=space(p);
        if (*p=='}') return !*space(p+1) ? count : -1;
        if (*p++!=',') return -1;
        if (*space(p)=='}') return -1;
    }
}
static const char *value(field_t *fields,int n,const char *key,bool string)
{
    for(int i=0;i<n;++i) if (!strcmp(fields[i].key,key) && fields[i].string==string) return fields[i].value;
    return NULL;
}
static bool number(field_t *f,int n,const char *key,unsigned max,unsigned *out)
{
    const char *s=value(f,n,key,false);
    if(!s || strlen(s)>5) return false;
    unsigned long x=strtoul(s,NULL,10);
    if(x>max) return false;
    *out=x; return true;
}
void sc_model_free(sc_model_t *m)
{
    free(m->aps); free(m->clients); memset(m,0,sizeof(*m));
}
static sc_event_t fail(sc_model_t *m,const char *message)
{
    m->failed=true; m->complete=false;
    snprintf(m->error,sizeof(m->error),"%s",message);
    return SC_UPDATED;
}
static int ap_index(const sc_model_t *m,const uint8_t bssid[6])
{
    for(unsigned i=0;i<m->ap_count;++i) if(!memcmp(m->aps[i].bssid,bssid,6)) return i;
    return -1;
}
sc_event_t sc_model_line(sc_model_t *m,const char *line,const uint8_t pending_ap[6],const uint8_t pending_sta[6])
{
    bool admission = !strncmp(line, "[WFACTL1] ", 10);
    field_t f[12]; int n=object(admission ? line+10 : line,f);
    if(n<0) return SC_IGNORED;
    unsigned version;
    if(admission) {
        const char *type=value(f,n,"type",true), *code=value(f,n,"code",true);
        const char *command=value(f,n,"command",true);
        if(!number(f,n,"v",1,&version) || version!=1 || !type || strcmp(type,"error") ||
           !code || strcmp(code,"busy") || !command || strcmp(command,"console")) return SC_IGNORED;
        if(pending_ap && pending_sta) {
            snprintf(m->error,sizeof(m->error),"Radio busy"); return SC_REFUSED;
        }
        return fail(m,"Radio busy; R to retry");
    }
    const char *type=value(f,n,"record_type",true);
    if(!type || !number(f,n,"schema_version",1,&version) || version!=1) return SC_IGNORED;
    if(!strcmp(type,"station_deauth")) {
        if(!pending_ap || !pending_sta) return SC_IGNORED;
        const char *status=value(f,n,"status",true);
        if(status && !strcmp(status,"error")) {
            const char *reason=value(f,n,"reason",true);
            snprintf(m->error,sizeof(m->error),"%s",reason ? reason : "Start refused");
            return SC_REFUSED;
        }
        const char *a=value(f,n,"bssid",true), *s=value(f,n,"station",true);
        uint8_t ap[6],sta[6];
        if(status && !strcmp(status,"started") && a && s && sc_parse_mac(a,ap) && sc_parse_mac(s,sta) &&
           !memcmp(ap,pending_ap,6) && !memcmp(sta,pending_sta,6)) return SC_STARTED;
        return SC_IGNORED;
    }
    if(!strcmp(type,"sniffer_clients")) return fail(m,value(f,n,"reason",true) ? value(f,n,"reason",true) : "Snapshot failed");
    if(!strcmp(type,"sniffer_clients_begin")) {
        if(m->begun && !m->complete && !m->failed) return fail(m,"Duplicate snapshot");
        sc_model_free(m);
        if(!number(f,n,"aps",SC_MAX_APS,&m->expected_aps) ||
           !number(f,n,"clients",SC_MAX_CLIENTS,&m->expected_clients)) return fail(m,"Invalid list size");
        m->aps=calloc(m->expected_aps ? m->expected_aps : 1,sizeof(*m->aps));
        m->clients=calloc(m->expected_clients ? m->expected_clients : 1,sizeof(*m->clients));
        if(!m->aps || !m->clients) return fail(m,"Not enough memory");
        m->begun=true; return SC_UPDATED;
    }
    if(!m->begun || m->failed || m->complete) return SC_IGNORED;
    if(!strcmp(type,"sniffer_clients_end")) {
        unsigned aps,clients;
        if(!number(f,n,"aps",SC_MAX_APS,&aps) || !number(f,n,"clients",SC_MAX_CLIENTS,&clients) ||
           aps!=m->ap_count || clients!=m->client_count || aps!=m->expected_aps || clients!=m->expected_clients)
            return fail(m,"Incomplete snapshot");
        m->complete=true; return SC_UPDATED;
    }
    bool is_ap=!strcmp(type,"sniffer_ap"), is_client=!strcmp(type,"sniffer_client");
    if(!is_ap && !is_client) return SC_IGNORED;
    const char *text=value(f,n,"bssid",true); uint8_t bssid[6];
    if(!text || !sc_parse_mac(text,bssid)) return fail(m,"Invalid BSSID");
    int index=ap_index(m,bssid);
    if(is_ap) {
        const char *hex=value(f,n,"ssid_hex",true);
        unsigned channel;
        if(index>=0 || m->ap_count>=m->expected_aps || !hex || strlen(hex)>64 || strlen(hex)%2 ||
           !number(f,n,"channel",177,&channel)) return fail(m,"Invalid AP record");
        sc_ap_t *ap=&m->aps[m->ap_count]; memcpy(ap->bssid,bssid,6); ap->channel=channel;
        ap->ssid_len=strlen(hex)/2;
        for(unsigned i=0;i<ap->ssid_len;++i) {
            int a=unhex(hex[2*i]), b=unhex(hex[2*i+1]);
            if(a<0 || b<0) return fail(m,"Invalid SSID");
            ap->ssid[i]=(a<<4)|b;
        }
        ++m->ap_count;
    } else {
        const char *mac=value(f,n,"station",true); uint8_t station[6];
        if(index<0 || m->client_count>=m->expected_clients || !mac || !sc_parse_mac(mac,station) || !memcmp(station,bssid,6))
            return fail(m,"Invalid client record");
        sc_client_t *client=&m->clients[m->client_count++];
        memcpy(client->mac,station,6); client->ap=index;
    }
    return SC_UPDATED;
}
void sc_ap_label(const sc_ap_t *ap,char *out,size_t size)
{
    if(!size) return;
    if(!ap->ssid_len) {
        char mac[18]; sc_mac_text(mac,ap->bssid);
        snprintf(out,size,"%s CH%u",mac,ap->channel); return;
    }
    size_t len=ap->ssid_len < size-1 ? ap->ssid_len : size-1;
    for(size_t i=0;i<len;++i) out[i]=ap->ssid[i]>=32 && ap->ssid[i]!=127 ? ap->ssid[i] : '?';
    out[len]=0;
}
