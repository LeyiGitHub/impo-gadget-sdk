#pragma once
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef enum { HTTP_EVENT_ON_DATA = 5 } esp_http_client_event_id_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    void *user_data;
    void *data;
    int data_len;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);
typedef struct {
    const char *url;
    int timeout_ms;
    http_event_handle_cb event_handler;
    void *user_data;
    int (*crt_bundle_attach)(void *conf);
    bool disable_auto_redirect;
    const char *common_name;
} esp_http_client_config_t;
typedef struct fake_http *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
