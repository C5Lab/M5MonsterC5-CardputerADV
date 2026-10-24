---
name: janos-sniffer-client-migration
description: Przenieś klienta JanOS na nowe API listy klientów i deauth po BSSID. Używaj przy aktualizacji aplikacji ESP32-S3 w ESP-IDF lub Flipper Zero, zachowując dotychczasowe komendy i tekstowe API JanOS.
---

# Mini-skill: nowe API sniffera JanOS — ESP32-S3 i Flipper Zero

Stan kodu: 2026-10-24, JanOS official z nowymi komendami i poprawką startu konsoli.
Ten plik jest samodzielny: zawiera kontrakt, aktualny workflow ADV, pełny parser C,
przykładową maszynę stanów oraz adaptery transportu. Można przekazać go innemu
agentowi razem z repozytorium aplikacji, którą trzeba poprawić.

## 1. Zadanie i granice zmian

Zaktualizuj ekran klientów sniffera i akcję deauth w aplikacji korzystającej z JanOS.
Dopasowanie celu ma używać **BSSID AP + MAC klienta**, niezależnie od ostatniego
skanu i od SSID. Wyświetlaj rozpoznane SSID; gdy go brak, BSSID i kanał.

Pracuj przede wszystkim w repozytorium aplikacji-klienta. Nie zmieniaj istniejących
komend JanOS, ich argumentów ani odpowiedzi. Nowe funkcje są dostępne wyłącznie
przez dodatkowe komendy. Nie trzeba przepisywać obsługi innych funkcji urządzenia.

Przed edycją znajdź właściciela UART, parser linii, ekran sniffera, ekran klientów,
obsługę deauth, powrót/wyjście, limity list i kod zwalniania callbacków. Dopasuj
integrację do istniejącej architektury; nie uruchamiaj drugiego odbiorcy tego samego
UART. Utrzymuj działające stare funkcje i uwzględnij lokalne instrukcje repozytorium.

Wymagany jest JanOS obsługujący `show_sniffer_clients` i `start_deauth_station`.
Samo `version` zwracające `1.7.5` nie potwierdza ich obecności: taki numer mają też
wcześniejsze binaria. Zdolność potwierdza poprawna odpowiedź `schema_version: 1`
na `show_sniffer_clients`. Brak odpowiedzi/nieznana komenda oznacza potrzebę
aktualizacji JanOS dla tej funkcji. Nie wracaj automatycznie do deauth po SSID.

## 2. Komendy faktycznie używane teraz przez ADV

### A. Menu główne → WiFi Sniff&Karma → Sniffer

| Zdarzenie | TX do JanOS, w kolejności |
|---|---|
| Wejście tylko do podmenu WiFi Sniff&Karma | Brak komendy |
| Pierwsze otwarcie globalnego Sniffera | `start_sniffer` |
| R: otwarcie wyników/klientów | `show_sniffer_clients` — bez wcześniejszego `stop` |
| P: otwarcie probes | `show_probes` — bez wcześniejszego `stop` |
| Powrót z listy klientów lub probes do globalnego Sniffera | `unselect_networks`, następnie `start_sniffer_noscan` |
| ESC/Q/Backspace z ekranu globalnego Sniffera | `stop` |

**Istotny szczegół obecnego kodu ADV:** pierwszy start globalny wysyła samo
`start_sniffer`; nie wysyła wtedy `unselect_networks`. JanOS wybiera tryb na
podstawie istniejącego zaznaczenia sieci. Jeżeli pozostało wcześniejsze zaznaczenie,
sama nazwa ekranu „globalny” nie gwarantuje pracy na wszystkich sieciach.
Jeśli w przenoszonej aplikacji masz gwarantować globalny start, jawnie wykonaj
`unselect_networks` → `start_sniffer`. Jest to świadoma zmiana zachowania klienta,
a nie opis obecnego pierwszego startu ADV. `unselect_networks` zatrzymuje operacje,
czyści wybór sieci i zachowuje wyniki skanu.

`start_sniffer_noscan` przy powrocie wymaga istniejących wyników skanu. Nie zastępuj
nim bezwarunkowo pierwszego `start_sniffer`.

### B. Skan Wi-Fi → wybrana sieć → Sniffer

| Zdarzenie | TX do JanOS, w kolejności |
|---|---|
| Uruchomienie skanu | `scan_networks`; podczas skanu warstwa UART ADV odpytuje `show_scan_results` mniej więcej co sekundę |
| Zatwierdzenie sieci z listy skanu | `select_networks <ID>`; dla wielu: `select_networks <ID1> <ID2> ...` |
| Wybranie Sniffera w menu działań | `start_sniffer` |
| R: otwarcie wyników/klientów | **`stop`, następnie `show_sniffer_clients`** |
| P: otwarcie probes | **`stop`, następnie `show_probes`** |
| Powrót z klientów lub probes do Sniffera wybranej sieci | `start_sniffer` |
| ESC/Q/Backspace z ekranu Sniffera | `stop` |

ID dla `select_networks` jest numerem z wyników skanu, liczonym od 1.
Jest potrzebne do początkowego wyboru sieci; **nie jest identyfikatorem nowej akcji D**.
Gdy sieci są wybrane, `start_sniffer` pracuje w trybie selected, bez nowego skanu.
Gdy nie są wybrane, normalny `start_sniffer` najpierw wykonuje skan.

### C. Wspólna lista klientów — oba konteksty

| Zdarzenie | TX / zachowanie ADV |
|---|---|
| Otwarcie listy | `show_sniffer_clients` |
| R na samej liście, po zakończeniu poprzedniego żądania | `show_sniffer_clients` |
| D na wskazanym kliencie | `start_deauth_station <BSSID_AP> <MAC_klienta>` |
| Oczekiwanie po D | Nie wysyłaj drugiego D; czekaj na potwierdzenie tej pary |
| `status: started` tej samej pary | Dopiero teraz otwórz ekran aktywnego deauth |
| Odmowa | Pokaż powód; nie otwieraj aktywnego ekranu |
| Timeout startu / anulowanie oczekiwania | `stop`; ignoruj późne `started`; po timeoutcie wymagaj przeładowania listy |
| ESC z aktywnego deauth | `stop`, powrót do zachowanej listy |
| Powrót z deauth do listy | Sam powrót nie wysyła `show_sniffer_clients` ani `start_sniffer` |
| Wyjście ze zwykłej, gotowej listy | Sama lista nie wysyła `stop`; następnie wykonuje się logika powrotu rodzica z tabel A/B |

D nie używa już sekwencji:

```text
show_scan_results
select_networks <indeks znaleziony po SSID>
select_stations <MAC>
start_deauth
```

Te stare komendy pozostają prawidłowym API JanOS dla innych funkcji/starych klientów.
Nie usuwaj ich i nie zmieniaj ich odpowiedzi. `show_probes` oraz tekstowy licznik
`Sniffer packet count: N` także pozostają bez zmian. W probes są nazwy poszukiwane
przez urządzenia; nie uznawaj samego probe request za potwierdzenie pary AP–klient.

## 3. Kontrakt nowego API: schema_version = 1

### 3.1. Transport i framing

- Zwykłe tekstowe komendy UART, bez JSON w TX. JanOS przyjmuje zakończenie linii;
  ADV używa CRLF, FlipperLight w swoim istniejącym helperze używa LF. Dodaj je raz.
- Bazowy baudrate ADV to 115200, 8N1. Zachowaj mapowanie pinów i wybór UART aplikacji.
- Odbiór to strumień bajtów. Jedno `read` nie oznacza jednej linii ani całej odpowiedzi.
- Odpowiedzi nowych komend to JSON Lines. Dla listy nie ma prefiksu `[WFA1]`.
- W strumieniu pojawiają się także echo komendy, `>`, logi i licznik sniffera.
  Nie karm parsera prefiksem `UART: RX:` z logu ADV — to dekoracja loggera, nie wire format.
- Linie nowego API mieszczą się poniżej 1024 bajtów. Bufor 1024 obejmuje terminator NUL.
  Nie używaj 32/64-bajtowych buforów linii z dawnego ekranu. Błąd FIFO, utratę bajtów
  i przepełnienie linii zgłaszaj jawnie; odrzuć uszkodzoną linię do CR/LF.
- Ignoruj niepowiązane linie; nie interpretuj promptu `>` jako końca snapshotu.

### 3.2. show_sniffer_clients

TX, bez argumentów:

```text
show_sniffer_clients
```

Przykładowe RX, po jednym obiekcie na linię:

```json
{"schema_version":1,"record_type":"sniffer_clients_begin","aps":1,"clients":1}
{"schema_version":1,"record_type":"sniffer_ap","bssid":"02:00:00:00:00:01","channel":6,"ssid_hex":"5061646479735456","ssid_source":"scan"}
{"schema_version":1,"record_type":"sniffer_client","bssid":"02:00:00:00:00:01","station":"04:00:00:00:00:02"}
{"schema_version":1,"record_type":"sniffer_clients_end","aps":1,"clients":1}
```

| Rekord/pole | Znaczenie |
|---|---|
| `sniffer_clients_begin.aps`, `.clients` | Liczby rekordów, które mają nadejść; maks. 100 AP i 5000 klientów |
| `sniffer_ap.bssid` | Tożsamość AP, MAC w zapisie z dwukropkami |
| `sniffer_ap.channel` | Ostatni kanał z RX albo skanu; 0 = nieznany. Nie traktuj zakresu liczbowego jako listy dozwolonych kanałów nadawania |
| `sniffer_ap.ssid_hex` | 0–32 bajty SSID, zakodowane jako 0–64 znaki hex; `""` = brak nazwy |
| `sniffer_ap.ssid_source` | `beacon`, `probe_response`, `association`, `reassociation`, `scan` albo `none` |
| `sniffer_client.bssid` | Jawne wskazanie AP rodzica; nie polegaj wyłącznie na kolejności poprzednich nagłówków |
| `sniffer_client.station` | MAC klienta |
| `sniffer_clients_end` | Jawny koniec; obie liczby muszą zgadzać się z begin i z faktycznym odbiorem |

Snapshot jest spójny. JanOS wysyła AP z klientami w kolejności bazy, a po AP jego
klientów; nie jest to kolejność wg RSSI ani gwarantowane sortowanie wg liczby klientów.
Nie wysyła AP bez klientów i pomija własne/broadcast AP.

**To cała zachowana baza sniffera, również po zatrzymaniu.** Kolejne uruchomienia
sniffera nie czyszczą jej automatycznie. Lista nie musi być równa ostatniemu skanowi
ani ograniczona do obecnego zaznaczenia sieci. Jeżeli UI selected ma filtrować
wyniki, najpierw zweryfikuj cały snapshot, potem filtruj widok po zbiorze BSSID
wybranych AP. Nie odrzucaj rekordów przed kontrolą liczników.

SSID z odebranych kompletnych ramek ma pierwszeństwo przed prawdziwą nazwą
zapamiętaną ze skanu. Etykiety generowane w legacy (`Unknown_…`, `MGMT_…`) nie są
traktowane jako odkryta nazwa. Prawdziwe SSID ze skanu może jednak legalnie mieć
taki tekst. Nie odrzucaj go na podstawie prefiksu po stronie klienta.
Ukryta/pusta ramka nie kasuje uprzednio rozpoznanej nazwy.

Nie używaj SSID jako klucza. Zachowuj bajty SSID z długością: mogą zawierać NUL,
cudzysłowy, przecinki, znaki sterujące i niepoprawny UTF-8. Dla UI przygotuj osobną,
bezpieczną etykietę; brak SSID wyświetl jako BSSID + kanał.

Błąd listy:

```json
{"schema_version":1,"record_type":"sniffer_clients","status":"error","reason":"no_memory"}
```

Obecne powody błędu listy to `invalid_arguments` i `no_memory`.
Brak `end` nie jest poprawną pustą listą.

### 3.3. start_deauth_station

TX, dokładnie BSSID i MAC klienta; bez kanału, SSID i indeksu skanu:

```text
start_deauth_station 02:00:00:00:00:01 04:00:00:00:00:02
```

JanOS weryfikuje parę w bazie sniffera, pobiera swój kanał i dopiero po walidacji
zatrzymuje aktywne operacje oraz uruchamia jeden cel. Nie wymaga wpisu w ostatnim
skanie, nie tworzy sztucznych wyników skanu i nie nadpisuje wyboru sieci/stacji legacy.
Nie trzeba wysyłać osobnego `stop` przed tą komendą. W jej odpowiedzi mogą wystąpić
zwykłe tekstowe logi wewnętrznego zatrzymywania — nie są odmową ani potwierdzeniem.

Potwierdzenie:

```json
{"schema_version":1,"record_type":"station_deauth","status":"started","bssid":"02:00:00:00:00:01","station":"04:00:00:00:00:02"}
```

Odmowa:

```json
{"schema_version":1,"record_type":"station_deauth","status":"error","reason":"client_not_found"}
```

| `reason` | Obsługa klienta |
|---|---|
| `invalid_arguments`, `invalid_target` | Nie uruchamiaj; sprawdź MAC/parę i serializację komendy |
| `not_ready` | Pokaż brak gotowości JanOS |
| `client_not_found` | Odśwież bazę/listę; nie szukaj zamiennika po SSID |
| `channel_unknown`, `channel_unavailable` | Pokaż problem kanału; potrzebne aktualne dane/radio dopuszczające kanał |
| `stop_failed`, `radio_error` | Pokaż błąd; nie otwieraj aktywnego ekranu |
| `no_memory` | Pokaż błąd pamięci; nie uruchamiaj retry w pętli |

Przed wywołaniem handlera konsola może odmówić zajętego radia:

```text
[WFACTL1] {"v":1,"type":"error","command":"console","code":"busy","reason":"radio_lease"}
```

To inny format niż `schema_version`. Parser niżej rozpoznaje właśnie taki prefiks
oraz `type=error`, `command=console`, `code=busy`. Nie traktuj każdego logu z napisem
„busy” jako odpowiedzi tej komendy.

`started` musi dotyczyć oczekiwanej pary i aktywnego stanu STARTING. Potwierdza
uruchomienie workera, nie skuteczne rozłączenie klienta; PMF może temu zapobiec.
`stop` jest nadal starą komendą, bez nowego JSON-owego ACK. Status udanego TX stopu
nie stanowi dowodu zakończenia pracy radia.

Nowy deauth ma własny cel i używa istniejącego mechanizmu nadawania. Nie przechodzi
na broadcast/CSA po niepowodzeniu. Co ok. pięć minut pasywnie szuka tego samego
BSSID na obsługiwanych kanałach. Brak odbioru zachowuje ostatni znany kanał;
ten tryb nie nadpisuje wyników skanu legacy.

## 4. Maszyna stanów i limity czasu

| Stan / zdarzenie | Działanie |
|---|---|
| Otwarcie listy | Zarejestruj odbiorcę przed TX, wyzeruj model, wyślij `show_sniffer_clients`, przejdź do LOADING |
| LOADING + rekordy | Waliduj wersję, MAC, hex, pojemności, AP rodzica i liczniki; nie udostępniaj D dla niepełnego snapshotu |
| LOADING + poprawny end | READY; udostępnij listę i wybór klienta |
| READY + D/akcja Flippera | Skopiuj BSSID i MAC z modelu, zapamiętaj je, wyślij start, STARTING |
| STARTING + ponowne D | Ignoruj; nie duplikuj komendy |
| STARTING + started zgodnej pary | ACTIVE; zleć zmianę widoku w wątku GUI |
| STARTING + odmowa | Pokaż błąd; bez otwierania aktywnego widoku |
| STARTING + timeout/anulowanie | Wyślij `stop`; unieważnij oczekiwanie i późne ACK. Po timeoutcie wymuś nową listę przed następnym startem |
| ACTIVE + Back/ESC | `stop`; powrót do listy bez automatycznego wznowienia sniffera |
| Powrót z listy do ekranu sniffera | Wykonaj odpowiednią sekwencję rodzica z sekcji 2 |
| Zamknięcie aplikacji / błąd UI | Gdy oczekiwano na start lub akcja trwa, zażądaj stopu; odłącz i dołącz/join odbiorcę, dopiero potem zwolnij stan |

Aktualne stałe ADV: **15 s na ACK startu**, **5 s bez rozpoznanego rekordu listy**,
**300 s łącznie na listę**. Nie resetuj timeoutu przypadkowymi logami licznika.
Pełne 5000 klientów przy 115200 baud może przesyłać się ponad minutę.

API nie ma `request_id`. Utrzymuj jedną transakcję na UART i nie ponawiaj startu
samoczynnie. Po anulowaniu/timeoutcie kolejność to `stop`, potem jawne przeładowanie
listy; późne `started` ignoruj podczas innych stanów. Samo dopasowanie MAC bez
kontroli stanu nie chroni przed późnym ACK.

## 5. Pełny parser C używany przez ADV

Poniższe dwa bloki są kopią produkcyjnych `main/screens/sniffer_clients_model.h`
i `.c` ze stanu opisanego na początku pliku. Są niezależne od ESP-IDF i Furi.
Skopiuj je pod podanymi nazwami do repozytorium klienta. Nie zastępuj ich
`strstr(SSID)` lub dzieleniem po przecinkach. Jest to celowo ograniczony parser
płaskich rekordów tego API, nie ogólna biblioteka dowolnego JSON.

Model przechowuje AP oddzielnie od klientów. Pole `client.ap` jest indeksem do
`model.aps`; `station` i `bssid` pozostają niezależnymi adresami. Po `SC_STARTED`
wyświetlaj aktywny ekran dopiero w wątku GUI. `SC_REFUSED` oznacza odmowę.

Maksymalna pamięć rekordów to
`100 * sizeof(sc_ap_t) + 5000 * sizeof(sc_client_t)` — w obecnym układzie około
39 kB, plus stan, bufor UART i pamięć GUI. Sprawdź wynik alokacji. Indeksy/liczniki
klientów nie mogą być `uint8_t`; użyj co najmniej `uint16_t` lub `unsigned`.

### Plik `sniffer_clients_model.h`

```c
#ifndef SNIFFER_CLIENTS_MODEL_H
#define SNIFFER_CLIENTS_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SC_MAX_APS 100
#define SC_MAX_CLIENTS 5000

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    uint8_t ssid_len;
    uint8_t ssid[32];
} sc_ap_t;
typedef struct { uint8_t mac[6]; uint8_t ap; } sc_client_t;
typedef struct {
    sc_ap_t *aps;
    sc_client_t *clients;
    unsigned ap_count, client_count, expected_aps, expected_clients;
    bool begun, complete, failed;
    char error[48];
} sc_model_t;
typedef enum { SC_IGNORED, SC_UPDATED, SC_STARTED, SC_REFUSED } sc_event_t;
void sc_model_free(sc_model_t *model);
sc_event_t sc_model_line(sc_model_t *model, const char *line,
                         const uint8_t pending_bssid[6], const uint8_t pending_station[6]);
bool sc_parse_mac(const char *text, uint8_t mac[6]);
void sc_mac_text(char out[18], const uint8_t mac[6]);
void sc_ap_label(const sc_ap_t *ap, char *out, size_t size);
#endif
```

### Plik `sniffer_clients_model.c`

```c
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
```

## 6. Przenośna sesja i framing do adaptacji

Ten blok jest **referencyjną integracją dla portowanych aplikacji**, nie kopią
całego ekranu ADV. Używa dokładnie parsera z sekcji 5 i tych samych timeoutów.
`JsSession` i `JsLine` umieść w stanie aplikacji na stercie. Wszystkie operacje
`js_*` wykonuje jeden worker. GUI przekazuje do niego żądania przez kolejkę.
Do GUI publikuj kopię małego modelu widoku lub chroń odczyt mutexem; nie zostawiaj
wskaźnika do zwolnionych tablic po `js_load`/`js_free`.

Funkcja TX dostaje komendę bez końca linii i dodaje go w adapterze. To status
transportu; tylko odpowiedź JanOS uruchamia ACTIVE. Gdy TX zawiedzie częściowo,
napraw/ponownie zsynchronizuj transport przed kolejną komendą.

### Plik `janos_sniffer_session.h`

```c
#ifndef JANOS_SNIFFER_SESSION_H
#define JANOS_SNIFFER_SESSION_H
#include "sniffer_clients_model.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    JS_EMPTY, JS_LOADING, JS_READY, JS_STARTING, JS_ACTIVE, JS_ERROR, JS_RELOAD_REQUIRED
} JsPhase;
typedef bool (*JsSendCommand)(void* context, const char* command);
typedef struct {
    sc_model_t model;
    JsPhase phase;
    uint8_t pending_ap[6], pending_sta[6];
    uint32_t list_started_ms, last_record_ms, start_sent_ms;
    char error[48];
    JsSendCommand send;
    void* send_context;
} JsSession;

/* Zero-initialize once. Call all functions from the same worker/task. */
static inline void js_init(JsSession* s, JsSendCommand send, void* context) {
    memset(s, 0, sizeof(*s));
    s->send = send;
    s->send_context = context;
}
static inline void js_set_error(JsSession* s, JsPhase phase, const char* message) {
    s->phase = phase;
    snprintf(s->error, sizeof(s->error), "%s", message);
}
static inline bool js_send(JsSession* s, const char* command) {
    return s->send && s->send(s->send_context, command);
}
static inline bool js_load(JsSession* s, uint32_t now_ms) {
    if(s->phase == JS_LOADING || s->phase == JS_STARTING || s->phase == JS_ACTIVE) return false;
    sc_model_free(&s->model);
    s->error[0] = 0;
    s->phase = JS_LOADING;
    s->list_started_ms = s->last_record_ms = now_ms;
    if(js_send(s, "show_sniffer_clients")) return true;
    js_set_error(s, JS_ERROR, "UART TX failed");
    return false;
}
static inline bool js_start(JsSession* s, unsigned selected_client, uint32_t now_ms) {
    if(s->phase != JS_READY || !s->model.complete || s->model.failed ||
       selected_client >= s->model.client_count) return false;
    const sc_client_t* client = &s->model.clients[selected_client];
    const sc_ap_t* ap = &s->model.aps[client->ap];
    char bssid[18], station[18], command[80];
    memcpy(s->pending_ap, ap->bssid, 6);
    memcpy(s->pending_sta, client->mac, 6);
    sc_mac_text(bssid, s->pending_ap);
    sc_mac_text(station, s->pending_sta);
    snprintf(command, sizeof(command), "start_deauth_station %s %s", bssid, station);
    s->phase = JS_STARTING;
    s->start_sent_ms = now_ms;
    s->error[0] = 0;
    if(js_send(s, command)) return true;
    js_set_error(s, JS_RELOAD_REQUIRED, "UART TX failed; check link");
    return false;
}
static inline sc_event_t js_line(JsSession* s, const char* line, uint32_t now_ms) {
    bool starting = s->phase == JS_STARTING;
    if(!starting && s->phase != JS_LOADING) return SC_IGNORED;
    sc_event_t event = sc_model_line(
        &s->model, line, starting ? s->pending_ap : NULL, starting ? s->pending_sta : NULL);
    if(!starting && event == SC_UPDATED) {
        s->last_record_ms = now_ms;
        if(s->model.failed) js_set_error(s, JS_ERROR, s->model.error);
        else if(s->model.complete) s->phase = JS_READY;
    } else if(starting && event == SC_STARTED) {
        s->phase = JS_ACTIVE;
    } else if(starting && event == SC_REFUSED) {
        js_set_error(s, JS_READY, s->model.error);
    }
    return event;
}
/* Poll even if no bytes arrive. Unsigned subtraction handles millisecond wrap. */
static inline void js_tick(JsSession* s, uint32_t now_ms) {
    if(s->phase == JS_STARTING && (uint32_t)(now_ms - s->start_sent_ms) >= 15000U) {
        bool sent = js_send(s, "stop");
        js_set_error(s, JS_RELOAD_REQUIRED, sent ? "Timeout; stop requested; reload" : "Timeout; stop TX failed");
    } else if(s->phase == JS_LOADING &&
              ((uint32_t)(now_ms - s->last_record_ms) >= 5000U ||
               (uint32_t)(now_ms - s->list_started_ms) >= 300000U)) {
        js_set_error(s, JS_ERROR, "Incomplete list; update JanOS / reload");
    }
}
static inline bool js_cancel(JsSession* s) {
    bool starting = s->phase == JS_STARTING;
    bool active = s->phase == JS_ACTIVE;
    bool sent = (!starting && !active) || js_send(s, "stop");
    if(starting || !sent) s->phase = JS_RELOAD_REQUIRED;
    else if(s->model.complete && !s->model.failed) s->phase = JS_READY;
    else s->phase = JS_EMPTY;
    if(!sent) snprintf(s->error, sizeof(s->error), "Stop TX failed; check link");
    return sent; /* This is TX status, not proof of radio shutdown. */
}
static inline void js_rx_loss(JsSession* s) {
    bool need_stop = s->phase == JS_STARTING || s->phase == JS_ACTIVE;
    bool sent = !need_stop || js_send(s, "stop");
    js_set_error(s, JS_RELOAD_REQUIRED, sent ? "RX lost; reload required" : "RX lost; stop TX failed");
}
static inline void js_free(JsSession* s) {
    /* Stop/cancel and join UART callbacks before freeing the session. */
    sc_model_free(&s->model);
}

/* Optional byte framer, useful when the app has no bounded line reader.
 * Use existing complete-line callbacks OR this framer, not both in parallel. */
typedef struct { char text[1024]; size_t used; bool discard; } JsLine;
static inline void js_feed(JsSession* s, JsLine* line,
                           const uint8_t* bytes, size_t count, uint32_t now_ms) {
    for(size_t i = 0; i < count; ++i) {
        uint8_t byte = bytes[i];
        if(byte == '\r' || byte == '\n') {
            if(!line->discard && line->used) {
                line->text[line->used] = 0;
                js_line(s, line->text, now_ms);
            }
            line->used = 0;
            line->discard = false;
        } else if(!line->discard) {
            if(byte == 0 || line->used + 1 >= sizeof(line->text)) {
                line->used = 0;
                line->discard = true;
                js_rx_loss(s);
            } else line->text[line->used++] = (char)byte;
        }
    }
}
#endif
```

## 7. Port na ESP32-S3 / ESP-IDF

1. Dodaj `sniffer_clients_model.c` do `idf_component_register(SRCS ...)` i jego
   katalog do `INCLUDE_DIRS`. `janos_sniffer_session.h` jest header-only. Nie trzeba
   dodawać cJSON ani pobierać dodatkowej biblioteki.
2. Pozostaw istniejący sterownik UART, piny i jego właściciela. Jeżeli masz już
   callback kompletnych linii, wywołuj `js_line` zamiast budować drugiego czytnika.
   Jeżeli odbierasz bajty, użyj `JsLine`/`js_feed`.
3. Stan inicjalizuj przed komendą. Dla wejścia na listę worker wykonuje
   `js_load(&link->session, esp_sniffer_now_ms())`.
4. Akcja GUI D/menu przesyła indeks klienta do workera, który wywołuje
   `js_start(&link->session, index, esp_sniffer_now_ms())`. Klient jest z pełnego
   modelu; jeżeli GUI filtruje listę, przetłumacz indeks widoku na indeks modelu.
5. Przetwarzaj GUI-request queue i RX w jednym tasku lub synchronizuj dostęp do
   sesji. Publikuj `JS_READY`, `JS_ACTIVE` i błędy przez istniejącą kolejkę zdarzeń.
   Nie otwieraj widoku, nie zwalniaj ekranu i nie odłączaj callbacka w samym callbacku
   UART. Może to zakleszczyć funkcję czekającą na zakończenie dispatchu.
6. Przy Back/ESC w workerze wykonaj `js_cancel`. Po zamknięciu ekranu zsynchronizuj
   odłączenie callbacka/koniec workera; dopiero wtedy `js_free` i `free(link)`.
7. Odbiornik musi wykrywać błędy z kolejki zdarzeń UART. `uart_read_bytes` samo nie
   zastępuje obsługi `UART_FIFO_OVF`/`UART_BUFFER_FULL`. Nie ukrywaj utraty rekordów.

Poniżej adapter do **już skonfigurowanego** UART; `*_pump` i `*_uart_event` podłącz
do istniejącego workera/event loop. Nie jest to drugi samodzielny sterownik.
Zachowaj RX ring buffer co najmniej 4096 bajtów jak ADV; dobierz go do obciążenia
aplikacji i nie blokuj czytnika renderowaniem GUI ani długim logowaniem.

### Plik `esp_adapter.c`

```c
#include "janos_sniffer_session.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

typedef struct {
    uart_port_t uart; /* Existing configured JanOS UART; preserve app pin mapping. */
    JsSession session;
    JsLine line;
} EspSnifferPort; /* Allocate on heap, not a small task's stack. */

static bool esp_sniffer_send(void* context, const char* command) {
    EspSnifferPort* link = context;
    size_t len = strlen(command);
    if(uart_write_bytes(link->uart, command, len) != (int)len) return false;
    return uart_write_bytes(link->uart, "\r\n", 2) == 2;
}
static uint32_t esp_sniffer_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}
void esp_sniffer_init(EspSnifferPort* link, uart_port_t existing_uart) {
    memset(link, 0, sizeof(*link));
    link->uart = existing_uart;
    js_init(&link->session, esp_sniffer_send, link);
}
/* Call in the existing UART worker, after dequeuing UI actions. */
void esp_sniffer_pump(EspSnifferPort* link) {
    uint8_t bytes[128];
    int n = uart_read_bytes(link->uart, bytes, sizeof(bytes), pdMS_TO_TICKS(100));
    uint32_t now_ms = esp_sniffer_now_ms();
    if(n > 0) js_feed(&link->session, &link->line, bytes, (size_t)n, now_ms);
    else if(n < 0) js_rx_loss(&link->session);
    js_tick(&link->session, now_ms);
    /* Publish a UI event/small copied view model if phase/data changed. */
}
/* Wire this into the app's existing UART event queue handler. */
void esp_sniffer_uart_event(EspSnifferPort* link, uart_event_type_t type) {
    if(type == UART_FIFO_OVF || type == UART_BUFFER_FULL ||
       type == UART_PARITY_ERR || type == UART_FRAME_ERR) {
        js_rx_loss(&link->session);
        uart_flush_input(link->uart);
        link->line.used = 0;
        link->line.discard = true; /* Resync on the next CR/LF. */
    }
}
```

## 8. Port na Flipper Zero / Furi

Nie ma obowiązku zmieniania całego projektu na nowy transport. Użyj tego samego
modelu i parsera C z sekcji 5; MCU klienta nie zmienia formatu JanOS.

Dla lokalnego FlipperLight miejsca do sprawdzenia to `src/uart_comm.c`,
`src/uart_comm.h`, `src/screen_sniff_karma.c`, `src/screen_sniffer.c` oraz `src/app.h`.
W innych FAP znajdź ich odpowiedniki, nie zakładaj identycznych nazw funkcji.

1. W `BrowseClients` zastąp `show_sniffer_results` nowym `show_sniffer_clients`.
   Nie przerabiaj globalnie parsera wszystkich starych komend na JSON.
2. Zamiast tablic tekstowych `char clients[][64]` i heurystyki „linia SSID, potem
   MAC” przechowuj AP i klientów z jawnym `bssid`. Zmień `client_count`, `selected`
   i `scroll_offset`, jeśli były `uint8_t` — 5000 klientów przekracza ten zakres.
3. Podłącz akcję UI (np. menu klienta lub długie OK) do `js_start`. Nie przypisuj
   nowego działania do przycisku, który aplikacja już wykorzystuje, bez uwzględnienia
   jej nawigacji. Potwierdzenie oraz odmowa mają ten sam workflow co w ADV.
4. Callback `furi_hal_serial_async_rx_start` pracuje w IRQ. Kopiuje bajty do
   `FuriStreamBuffer`, sygnalizuje przepełnienia i wraca. **Nie parsuj tam JSON,
   nie alokuj modelu, nie nadaj komendy i nie odświeżaj GUI w IRQ.**
5. Worker odbiera fragmenty, składa linie i wywołuje parser. Istniejący helper
   `uart_read_line` we FlipperLight korzysta ze wspólnego bufora; nie uruchamiaj go
   równocześnie z drugim konsumentem `FuriStreamBuffer`. Wybierz jeden dispatch
   i rozdzielaj gotowe linie między funkcje aplikacji.
6. Dotychczasowy `uart_send_command(WiFiApp*, const char*)` we FlipperLight już
   dopisuje LF. Jeżeli go używasz, nie dodawaj kolejnego zakończenia. W przykładzie
   niżej HAL sam wysyła LF, więc tego helpera już nie wołaj.
7. GUI otrzymuje zdarzenia przez `view_dispatcher_send_custom_event` lub istniejącą
   kolejkę aplikacji. Przyjmij event ACTIVE dopiero po poprawnym ACK. Przekazywane
   numery zdarzeń i model widoku dopasuj do istniejącej aplikacji.
8. Przy zamykaniu zatrzymaj RX (`furi_hal_serial_async_rx_stop`), zatrzymaj i join
   worker, potem zwolnij model/bufory i dopiero oddaj posiadany uchwyt portu.
   Nie zwalniaj serial handle należącego do innego podsystemu.

`FlipperSnifferPort`, model i bufor linii trzymaj na stercie. Około 39 kB rekordów
plus bufor RX i pozostałe dane aplikacji muszą zmieścić się w dostępnej pamięci.
Przy braku pamięci pokaż błąd. Nie obcinaj listy po cichu do starego limitu.
Jeżeli projekt potrzebuje paginacji/strumieniowania do pliku zamiast pełnego modelu,
zaprojektuj to po stronie klienta, nadal odbierając i weryfikując cały snapshot;
nie dopisuj nieistniejącego argumentu `limit`/`offset` do JanOS.

Sprawdź stos workera: parser ma lokalną tablicę pól ponad 1 kB, a wywołania
biblioteczne też używają stosu. 4 kB to punkt wyjścia jak w istniejących workerach,
nie dowód wystarczającego zapasu; zmierz watermark na docelowym firmware.

Przykład poniżej używa obecnych lokalnych API Furi: `furi_get_tick()` jest w ms,
`furi_stream_buffer_send(..., 0)` może działać w IRQ. Zweryfikuj sygnatury w SDK
portowanej aplikacji. Rejestrując poniższy callback, użyj raportowania błędów RX
(`report_errors=true`). Przy istniejącym IRQ włącz jego logikę do niego, zamiast
rejestrować drugi callback i gubić obsługę reszty aplikacji.

### Plik `flipper_adapter.c`

```c
#include "janos_sniffer_session.h"
#include <furi.h>
#include <furi_hal.h>
#include <stdatomic.h>
#include <string.h>

typedef struct {
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx;
    atomic_bool rx_lost;
    JsSession session;
    JsLine line;
} FlipperSnifferPort; /* Heap allocation; check allocation failures. */

static bool flipper_sniffer_send(void* context, const char* command) {
    FlipperSnifferPort* link = context;
    if(!link->serial) return false;
    furi_hal_serial_tx(link->serial, (const uint8_t*)command, strlen(command));
    furi_hal_serial_tx(link->serial, (const uint8_t*)"\n", 1);
    furi_hal_serial_tx_wait_complete(link->serial);
    return true; /* HAL TX has no status return; wait for the JanOS ACK. */
}
void flipper_sniffer_init(FlipperSnifferPort* link,
                          FuriHalSerialHandle* existing_serial, FuriStreamBuffer* existing_rx) {
    memset(link, 0, sizeof(*link));
    link->serial = existing_serial;
    link->rx = existing_rx;
    atomic_init(&link->rx_lost, false);
    js_init(&link->session, flipper_sniffer_send, link);
}
/* Replace/merge into the existing RX IRQ, never create a second UART consumer. */
void flipper_sniffer_irq(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    FlipperSnifferPort* link = context;
    if(event & (FuriHalSerialRxEventOverrunError | FuriHalSerialRxEventFrameError |
                FuriHalSerialRxEventParityError | FuriHalSerialRxEventNoiseError))
        atomic_store(&link->rx_lost, true);
    if(!(event & FuriHalSerialRxEventData)) return;
    while(furi_hal_serial_async_rx_available(handle)) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        if(furi_stream_buffer_send(link->rx, &byte, 1, 0) != 1)
            atomic_store(&link->rx_lost, true);
    }
}
/* Call in the serial worker; furi_get_tick() returns milliseconds in this SDK. */
void flipper_sniffer_pump(FlipperSnifferPort* link) {
    uint8_t bytes[128];
    size_t n = furi_stream_buffer_receive(link->rx, bytes, sizeof(bytes), furi_ms_to_ticks(100));
    uint32_t now_ms = furi_get_tick();
    if(atomic_exchange(&link->rx_lost, false)) {
        js_rx_loss(&link->session);
        link->line.used = 0;
        link->line.discard = true;
    }
    if(n) js_feed(&link->session, &link->line, bytes, n, now_ms);
    js_tick(&link->session, now_ms);
    /* Post a custom event / copied view model to the GUI thread. */
}
```

## 9. Kolejność wdrożenia w aplikacji

1. Potwierdź, że posiadany JanOS obsługuje nowe komendy. Zrób to na gotowym,
   bezczynnie obsługiwanym łączu; nie wysyłaj równoległych żądań z dwóch ekranów.
2. Zapisz obecne sekwencje startu/powrotu, potem wybierz politykę global/selected
   z sekcji 2. Nie usuwaj początkowego `select_networks`, jeśli użytkownik wybrał
   sieć ze skanu. Usuń jedynie wyszukiwanie indeksu na potrzeby nowej akcji klienta.
3. Wprowadź model, parser i obsługę begin/end; najpierw przetestuj sam odczyt listy.
4. Dodaj identyfikację pary BSSID–station i czekanie na ACK. Nazwa służy tylko do UI.
5. Dodaj timeout, stop/cancel, retry przez nową listę i bezpieczny teardown.
6. Przetestuj powrót do listy, następnie do obu rodzajów ekranu sniffera. Nie
   uruchamiaj wznowienia sniffera przy samym powrocie z deauth do listy.
7. Zbuduj klienta. Przetestuj sprzętowo UART, oba pasma obsługiwane przez JanOS,
   reakcję Back/ESC i współpracę pozostałych starszych aplikacji z tym samym JanOS.

Przy przenoszeniu tych plików do istniejącego klienta można użyć jego własnej
maszyny stanów i JSON parsera, jeśli zachowuje kontrakt. Kod tutaj służy też jako
punkt odniesienia dla testów, nie narzuca wymiany całej architektury aplikacji.

## 10. Testy i kryteria odbioru

Sprawdź co najmniej:

- Dwa różne BSSID mające identyczne SSID: D wybiera dokładnie właściwy AP i klienta.
- `PaddysTV` jest w bazie sniffera, ale nie ma go w ostatnim skanie: start nie
  wykonuje `show_scan_results` i nadal działa dla znanej pary/kanału.
- Ukryta nazwa: `ssid_hex=""`, UI pokazuje BSSID + kanał; późniejsza nazwa jest
  aktualizowana po ponownym pobraniu listy, nie przez zgadywanie po MAC/vendorze.
- SSID ma 32 bajty, NUL, przecinki, cudzysłowy lub CR/LF: hex dekoduje się bez
  uszkodzenia rekordów, a UI nie interpretuje znaków sterujących.
- Fragmentacja po jednym bajcie, kilka linii w jednym RX, CRLF i LF, echo komendy,
  `Sniffer packet count`, zwykłe logi oraz prompt pomiędzy rekordami.
- Brak end, złe liczniki, zły hex, błędne MAC, klient bez znanego AP, zła wersja,
  przepełnienie linii i utrata UART: nie pokazuj kompletnej listy ani aktywnej akcji.
- Poprawny pusty snapshot begin/end z zerami i pełny snapshot 100 AP / 5000 klientów.
- D podczas LOADING nie wysyła startu, powtórne D podczas STARTING jest ignorowane.
- started innej pary, started bez oczekiwania i started po timeoutcie są ignorowane.
- Wszystkie wymienione błędy oraz prefiksowana odmowa busy nie otwierają aktywnego UI.
- Timeout/anulowanie żąda stopu, a kolejny start po timeoutcie wymaga odświeżenia.
- Po Back/ESC nie zostaje wiszący worker ani callback odwołujący się do zwolnionego modelu.
- Wejście na probes nadal używa tekstowego `show_probes`; pozostałe stare funkcje
  aplikacji i formaty JanOS nadal działają.

W repozytorium ADV są gotowe testy parsera, pełnej listy, cyklu ekranu i kontraktu
wire pomiędzy kodem JanOS i kodem ADV:

```sh
python3 -m unittest discover -s tests -p 'test_sniffer_clients.py'
JANOS_SOURCE_ROOT=/path/to/projectZero/ESP32C5 python3 -m unittest discover -s tests -p 'test_sniffer_clients.py'
```

W repozytorium JanOS (`ESP32C5`):

```sh
python3 -m unittest discover -s tests -p 'test_sniffer*.py'
JANOS_TEST_SANITIZERS=1 python3 -m unittest discover -s tests -p 'test_janos_console_startup.py'
```

Jeżeli oprócz klienta modyfikujesz JanOS, zachowaj testy porównujące stary output
bajt po bajcie i nie wprowadzaj JSON do istniejących komend. Skontroluj pełną
rejestrację przy starcie: po dodaniu dwóch komend jest 130 kontekstów, a
`JANOS_CONSOLE_MAX_COMMANDS` jest poprawione z 128 na 256. Pozostaw test, który
wykonuje całą listę rejestracji. Samo pomyślne `idf.py build` nie wykryło tego
wcześniejszego błędu startu. Nie „naprawiaj” go przez pomijanie komend lub
ignorowanie `ESP_ERROR_CHECK`.

Po portowaniu raportuj: zmienione funkcje klienta, testy/build, wymagane firmware
JanOS oraz oddzielnie wyniki testów na sprzęcie. Nie nazywaj samego ACK dowodem
rozłączenia i nie twierdź, że test na hoście potwierdza działanie fizycznego UART.

## 11. Źródła do sprawdzenia przy kolejnej aktualizacji

W repozytorium ADV:

- `main/screens/global_sniffer_screen.c` — globalny start i resume.
- `main/screens/network_list_screen.c`, `attack_select_screen.c`, `sniffer_screen.c`
  — wybór sieci, start selected, stop przed klientami/probes i resume.
- `main/screens/sniffer_results_screen.c` — list/ACK/timeout/GUI lifecycle.
- `main/screens/sniffer_clients_model.h` i `.c` — kod skopiowany w sekcji 5.
- `main/screens/station_deauth_screen.c`, `main/uart_handler.c` — stop i transport.

W JanOS official:

- `ESP32C5/main/sniffer_clients.inc` — jedyne źródło prawdy dla nowych odpowiedzi.
- `ESP32C5/main/main.c` — rejestracja, wybór trybu sniffera, metadane i stop.
- `ESP32C5/main/sniffer_extended.c` — rozpoznawanie nazw z ramek.
- `ESP32C5/docs/sniffer_clients.md` — opis kontraktu.

Lokalne repozytorium JanOS będące podstawą tej wersji:
`janos_public/official/projectZero`, nie starsza kopia `kod/projectZero`.
Po zmianie implementacji odśwież ten skill z kodu, zwłaszcza sekwencje resume,
stałe czasu, limity oraz parser. Nie dopisuj do dokumentacji komend lub parametrów,
których aktualny JanOS nie obsługuje.

## 12. Weryfikacja tej wersji pliku

Parser w sekcji 5 jest identyczny z aktualnym kodem ADV. Kod wyodrębniony z bloków
przeszedł test pełnej listy/modelu oraz test sesji i framingu z AddressSanitizer
i UndefinedBehaviorSanitizer (m.in. pojedyncze bajty, timeout, późne ACK, anulowanie,
przepełnienie i zawinięcie zegara). Adapter ESP skompilowano z rzeczywistymi
nagłówkami i kompilatorem ESP-IDF 6.0 dla ESP32-S3. Adapter Flippera sprawdzono
składniowo z deklaracjami odpowiadającymi odczytanym lokalnym nagłówkom Furi;
to nie zastępuje kompilacji FAP ani testu na Flipperze. Żaden z tych przykładów
nie oznacza wykonania testu na fizycznym UART lub urządzeniu.
