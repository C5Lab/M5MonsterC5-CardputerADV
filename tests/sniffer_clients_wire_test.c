/* Consume the actual JanOS command fixture, including binary SSID and hidden AP. */
#include "sniffer_clients_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    sc_model_t model = {0};
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) sc_model_line(&model, line, NULL, NULL);
    assert(model.complete && !model.failed && model.ap_count == 2 && model.client_count == 2);
    assert(model.aps[0].ssid_len == 6 && !memcmp(model.aps[0].ssid, "A,\"\n\0Z", 6));
    assert(model.aps[1].ssid_len == 0 && model.aps[1].channel == 48);
    assert(model.clients[0].ap == 0 && model.clients[1].ap == 1);
    sc_model_free(&model);
    puts("JanOS to ADV wire contract PASS");
}
