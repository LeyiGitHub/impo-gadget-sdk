// SPDX-License-Identifier: Apache-2.0
// Runs components/impo_cloud_route/impo_cloud_route.c against a scripted
// relay /where and a fake clock. Built twice: relay on (the default) and,
// with -DROUTE_RELAY_OFF, relay off.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "impo_cloud_route.h"

int64_t fake_now_us;
static int where_calls, locks_held;
static const char *where_body;   // NULL: the request never completes
static int where_status = 200;
static int64_t where_takes_us;   // how long a request takes
static char last_url[128];

struct fake_http { esp_http_client_config_t cfg; };

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    struct fake_http *h = calloc(1, sizeof(*h));
    h->cfg = *config;
    return h;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t h)
{
    where_calls++;
    strncpy(last_url, h->cfg.url, sizeof(last_url) - 1);
    fake_now_us += where_takes_us;
    if (!where_body) {
        return ESP_FAIL;
    }
    esp_http_client_event_t evt = {HTTP_EVENT_ON_DATA, h->cfg.user_data, (void *)where_body, (int)strlen(where_body)};
    h->cfg.event_handler(&evt);
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t h) { (void)h; return where_body ? where_status : 0; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { free(h); return ESP_OK; }

SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int m; return &m; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; assert(locks_held == 0); locks_held++; return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; assert(locks_held == 1); locks_held--; return 1; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
void vTaskDelay(TickType_t ticks) { fake_now_us += (int64_t)ticks * 1000; }

static void endpoint(const char *host, const char *want_host, int want_port)
{
    char out[64];
    int port = 0;
    impo_cloud_route_endpoint(host, out, sizeof(out), &port);
    assert(strcmp(out, want_host) == 0);
    assert(port == want_port);
    assert(locks_held == 0);
}

static void url(const char *in, const char *want)
{
    char out[128];
    impo_cloud_route_url(in, out, sizeof(out));
    assert(strcmp(out, want) == 0);
}

#define MIN (60LL * 1000000)
#define CN "{\"country\":\"CN\",\"ip\":\"203.0.113.10\"}"
#define OTHER "{\"country\":\"other\",\"ip\":\"198.51.100.10\"}"

int main(void)
{
    char header[64];
    impo_cloud_route_host_header("gadgets.impo.ai", 443, header, sizeof(header));
    assert(strcmp(header, "gadgets.impo.ai") == 0);
    impo_cloud_route_host_header("gadget.itoy.cn", 9443, header, sizeof(header));
    assert(strcmp(header, "gadget.itoy.cn:9443") == 0);
    assert(!impo_cloud_route_http_ok(0) && !impo_cloud_route_http_ok(502) && !impo_cloud_route_http_ok(504));
    assert(impo_cloud_route_http_ok(200) && impo_cloud_route_http_ok(401) && impo_cloud_route_http_ok(500));

#ifdef ROUTE_RELAY_OFF
    where_body = CN;
    endpoint("gadgets.impo.ai", "gadgets.impo.ai", 443);
    endpoint("", "gadgets.impo.ai", 443);
    endpoint("dev.example.com", "dev.example.com", 443);
    url("https://gadgets.impo.ai/fetch_vms", "https://gadgets.impo.ai/fetch_vms");
    for (int i = 0; i < 5; i++) impo_cloud_route_report("gadget.itoy.cn", false);
    impo_cloud_route_reset();
    assert(where_calls == 0);
#else
    // A developer's own gateway is used as given, and nobody is asked.
    endpoint("dev.example.com", "dev.example.com", 443);
    url("https://dev.example.com/fetch_vms", "https://dev.example.com/fetch_vms");
    url("https://gadgets.impo.ai.example.com/x", "https://gadgets.impo.ai.example.com/x");
    url("http://gadgets.impo.ai/x", "http://gadgets.impo.ai/x");
    assert(where_calls == 0);

    // In mainland China: the relay, for every path, after one question.
    where_body = CN;
    endpoint("gadgets.impo.ai", "gadget.itoy.cn", 9443);
    assert(where_calls == 1);
    assert(strcmp(last_url, "https://gadget.itoy.cn:9443/where") == 0);
    endpoint("", "gadget.itoy.cn", 9443);
    endpoint(NULL, "gadget.itoy.cn", 9443);
    url("https://gadgets.impo.ai", "https://gadget.itoy.cn:9443");
    url("https://gadgets.impo.ai/fetch_vms", "https://gadget.itoy.cn:9443/fetch_vms");
    url("https://gadgets.impo.ai?x=1", "https://gadget.itoy.cn:9443?x=1");
    assert(where_calls == 1);

    // A new Wi-Fi address: ask again. Outside mainland China: Cloudflare.
    impo_cloud_route_reset();
    where_body = OTHER;
    endpoint("gadgets.impo.ai", "gadgets.impo.ai", 443);
    url("https://gadgets.impo.ai/fetch_vms", "https://gadgets.impo.ai/fetch_vms");
    assert(where_calls == 2);

    // The answer lasts 12 hours.
    where_body = CN;
    fake_now_us += 12 * 60 * MIN - 1;
    endpoint("gadgets.impo.ai", "gadgets.impo.ai", 443);
    assert(where_calls == 2);
    fake_now_us += 1;
    endpoint("gadgets.impo.ai", "gadget.itoy.cn", 9443);
    assert(where_calls == 3);

    // Three failed relay connections in a row: Cloudflare for 10 minutes. A
    // success in between starts the count again; other hosts never count.
    impo_cloud_route_report("gadget.itoy.cn", false);
    impo_cloud_route_report("gadget.itoy.cn", false);
    impo_cloud_route_report("gadget.itoy.cn", true);
    impo_cloud_route_report("gadget.itoy.cn", false);
    impo_cloud_route_report("gadget.itoy.cn", false);
    impo_cloud_route_report("gadgets.impo.ai", false);
    impo_cloud_route_report("https://gadget.itoy.cn.example.com/x", false);
    endpoint("gadgets.impo.ai", "gadget.itoy.cn", 9443);
    impo_cloud_route_report("https://gadget.itoy.cn:9443/fetch_vms", false);
    endpoint("gadgets.impo.ai", "gadgets.impo.ai", 443);
    url("https://gadgets.impo.ai/fetch_vms", "https://gadgets.impo.ai/fetch_vms");
    fake_now_us += 10 * MIN - 1;
    endpoint("gadgets.impo.ai", "gadgets.impo.ai", 443);
    fake_now_us += 1;
    endpoint("gadgets.impo.ai", "gadget.itoy.cn", 9443);
    assert(where_calls == 3);

    // /where fails at once (the first DNS lookup after Wi-Fi): asked three
    // times, then Cloudflare, and asked again five minutes later.
    impo_cloud_route_reset();
    where_body = NULL;
    endpoint("gadgets.impo.ai", "gadgets.impo.ai", 443);
    assert(where_calls == 6);
    where_body = CN;
    fake_now_us += 5 * MIN - 2000;  // the attempts' 1.5 s backoff is already on the clock
    endpoint("gadgets.impo.ai", "gadgets.impo.ai", 443);
    assert(where_calls == 6);
    fake_now_us += 2000;
    endpoint("gadgets.impo.ai", "gadget.itoy.cn", 9443);
    assert(where_calls == 7);

    // /where times out: asked once, not three times over 15 seconds.
    impo_cloud_route_reset();
    where_body = NULL;
    where_takes_us = 5 * 1000000;
    endpoint("gadgets.impo.ai", "gadgets.impo.ai", 443);
    assert(where_calls == 8);
    where_takes_us = 0;

    // The relay answers but not with a country (an error page): Cloudflare.
    impo_cloud_route_reset();
    where_body = "<html>502</html>";
    where_status = 502;
    endpoint("gadgets.impo.ai", "gadgets.impo.ai", 443);
    assert(where_calls == 9);
    impo_cloud_route_reset();
    where_body = CN;
    where_status = 200;
    endpoint("gadgets.impo.ai", "gadget.itoy.cn", 9443);
    assert(where_calls == 10);
#endif
    puts("cloud route harness: ok");
    return 0;
}
