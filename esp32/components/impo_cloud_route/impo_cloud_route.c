// SPDX-License-Identifier: Apache-2.0
// See impo_cloud_route.h.
#include "impo_cloud_route.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_IMPO_CHINA_RELAY
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "impo.route";

#define RELAY_HOST CONFIG_IMPO_CHINA_RELAY_HOST
#define RELAY_PORT CONFIG_IMPO_CHINA_RELAY_PORT
#define STR2(x) #x
#define STR(x) STR2(x)
#define WHERE_URL "https://" RELAY_HOST ":" STR(CONFIG_IMPO_CHINA_RELAY_PORT) "/where"

#define DECISION_US (12LL * 3600 * 1000000)  // ask again at least twice a day
#define RETRY_US (5LL * 60 * 1000000)        // after a failed /where
#define RELAY_FAILURES 3                     // failed relay connections in a row...
#define RELAY_OFF_US (10LL * 60 * 1000000)   // ...send the gadget direct this long
#define WHERE_TIMEOUT_MS 5000
// The first DNS lookup after Wi-Fi comes up often fails at once (see vm_api.c).
// A /where that fails that fast is asked again; one that timed out is not.
#define WHERE_ATTEMPTS 3
#define WHERE_FAST_FAIL_US (2LL * 1000000)

typedef enum { ROUTE_UNKNOWN, ROUTE_CLOUD, ROUTE_RELAY } route_t;

static SemaphoreHandle_t _Atomic s_lock;
static route_t s_route = ROUTE_UNKNOWN;
static int64_t s_expires_us;
static int64_t s_relay_off_until_us;
static int s_relay_failures;
// Bumped by impo_cloud_route_reset() without the lock; a decision made for an
// older address is made again.
static atomic_uint s_address;
static unsigned s_decided_address;

typedef struct {
    char *buf;
    size_t cap, len;
} body_t;

static esp_err_t on_where(esp_http_client_event_t *evt)
{
    body_t *b = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b->len + evt->data_len < b->cap) {
        memcpy(b->buf + b->len, evt->data, evt->data_len);
        b->len += evt->data_len;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

// GET /where on the relay. Returns the HTTP status, or 0 if it never completed.
static int where(char *buf, size_t cap)
{
    body_t b = {buf, cap, 0};
    buf[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = WHERE_URL,
        .timeout_ms = WHERE_TIMEOUT_MS,
        .event_handler = on_where,
        .user_data = &b,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return 0;
    }
    int status = esp_http_client_perform(c) == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    esp_http_client_cleanup(c);
    return status;
}

// Link, the chat session and the HTTP clients can ask at once; the first to
// arrive creates the lock.
static SemaphoreHandle_t lock(void)
{
    SemaphoreHandle_t l = atomic_load(&s_lock);
    if (!l) {
        SemaphoreHandle_t mine = xSemaphoreCreateMutex();
        if (!mine) {
            return NULL;
        }
        SemaphoreHandle_t none = NULL;
        if (atomic_compare_exchange_strong(&s_lock, &none, mine)) {
            l = mine;
        } else {
            vSemaphoreDelete(mine);
            l = none;
        }
    }
    xSemaphoreTake(l, portMAX_DELAY);
    return l;
}

static route_t decide(void)
{
    SemaphoreHandle_t l = lock();
    if (!l) {
        return ROUTE_CLOUD;
    }
    int64_t now = esp_timer_get_time();
    unsigned address = atomic_load(&s_address);
    if (s_route == ROUTE_UNKNOWN || now >= s_expires_us || s_decided_address != address) {
        char body[160];
        int status = 0;
        for (int attempt = 0; attempt < WHERE_ATTEMPTS; attempt++) {
            if (attempt) {
                vTaskDelay(pdMS_TO_TICKS(500 << (attempt - 1)));
            }
            int64_t start = esp_timer_get_time();
            status = where(body, sizeof(body));
            now = esp_timer_get_time();
            if (status != 0 || now - start >= WHERE_FAST_FAIL_US) {
                break;
            }
        }
        if (status == 200 && strstr(body, "\"country\":\"CN\"")) {
            s_route = ROUTE_RELAY;
            s_expires_us = now + DECISION_US;
            ESP_LOGI(TAG, "in mainland China: Impo through %s:%d", RELAY_HOST, RELAY_PORT);
        } else if (status == 200 && strstr(body, "\"country\":")) {
            s_route = ROUTE_CLOUD;
            s_expires_us = now + DECISION_US;
            ESP_LOGI(TAG, "outside mainland China: Impo at " IMPO_CLOUD_HOST);
        } else {
            s_route = ROUTE_CLOUD;
            s_expires_us = now + RETRY_US;
            ESP_LOGW(TAG, "%s answered %d: " IMPO_CLOUD_HOST " for now, asking again in 5 min", WHERE_URL, status);
        }
        s_decided_address = address;
        s_relay_failures = 0;
        s_relay_off_until_us = 0;
    }
    route_t route = s_route;
    if (route == ROUTE_RELAY && now < s_relay_off_until_us) {
        route = ROUTE_CLOUD;
    }
    xSemaphoreGive(l);
    return route;
}

static bool is_relay(const char *host)
{
    if (!host) {
        return false;
    }
    if (strncmp(host, "https://", 8) == 0) {
        host += 8;
    }
    size_t n = strlen(RELAY_HOST);
    return strncmp(host, RELAY_HOST, n) == 0
           && (host[n] == '\0' || host[n] == ':' || host[n] == '/');
}

void impo_cloud_route_report(const char *host, bool ok)
{
    if (!is_relay(host)) {
        return;
    }
    SemaphoreHandle_t l = lock();
    if (!l) {
        return;
    }
    if (ok) {
        s_relay_failures = 0;
    } else if (++s_relay_failures >= RELAY_FAILURES) {
        s_relay_failures = 0;
        s_relay_off_until_us = esp_timer_get_time() + RELAY_OFF_US;
        ESP_LOGW(TAG, "%s failed %d times in a row: " IMPO_CLOUD_HOST " for 10 min", RELAY_HOST, RELAY_FAILURES);
    }
    xSemaphoreGive(l);
}

void impo_cloud_route_reset(void)
{
    atomic_fetch_add(&s_address, 1);
}
#else
void impo_cloud_route_report(const char *host, bool ok)
{
    (void)host;
    (void)ok;
}

void impo_cloud_route_reset(void) {}
#endif

bool impo_cloud_route_http_ok(int status)
{
    return status > 0 && status != 502 && status != 504;
}

static bool is_cloud(const char *host)
{
    return !host || !*host || strcmp(host, IMPO_CLOUD_HOST) == 0;
}

void impo_cloud_route_endpoint(const char *host, char *out_host, size_t cap, int *out_port)
{
#if CONFIG_IMPO_CHINA_RELAY
    if (is_cloud(host) && decide() == ROUTE_RELAY) {
        snprintf(out_host, cap, "%s", RELAY_HOST);
        *out_port = RELAY_PORT;
        return;
    }
#endif
    snprintf(out_host, cap, "%s", is_cloud(host) ? IMPO_CLOUD_HOST : host);
    *out_port = 443;
}

void impo_cloud_route_url(const char *url, char *out, size_t cap)
{
    const size_t n = sizeof(IMPO_CLOUD_BASE_URL) - 1;
    bool cloud = url && strncmp(url, IMPO_CLOUD_BASE_URL, n) == 0
                 && (url[n] == '\0' || url[n] == '/' || url[n] == '?');
#if CONFIG_IMPO_CHINA_RELAY
    if (cloud && decide() == ROUTE_RELAY) {
        snprintf(out, cap, "https://%s:%d%s", RELAY_HOST, RELAY_PORT, url + n);
        return;
    }
#else
    (void)cloud;
#endif
    snprintf(out, cap, "%s", url ? url : "");
}

void impo_cloud_route_host_header(const char *host, int port, char *out, size_t cap)
{
    if (port == 443) {
        snprintf(out, cap, "%s", host);
    } else {
        snprintf(out, cap, "%s:%d", host, port);
    }
}
