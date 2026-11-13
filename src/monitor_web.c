#include "monitor_web.h"

#include <stdbool.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_http_server.h"
#include "esp_timer.h"

extern const char page_start[] asm("_binary_index_html_start");
extern const char page_end[] asm("_binary_index_html_end");

static httpd_handle_t server;
static portMUX_TYPE sample_lock = portMUX_INITIALIZER_UNLOCKED;
static float latest_temperature;
static float latest_humidity;
static int64_t sample_time_us;
static bool sample_available;

void monitor_web_update(float temperature_c, float humidity_pct)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&sample_lock);
    latest_temperature = temperature_c;
    latest_humidity = humidity_pct;
    sample_time_us = now;
    sample_available = true;
    portEXIT_CRITICAL(&sample_lock);
}

static esp_err_t page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, page_start, page_end - page_start - 1);
}

static esp_err_t readings_handler(httpd_req_t *request)
{
    portENTER_CRITICAL(&sample_lock);
    float temperature = latest_temperature;
    float humidity = latest_humidity;
    int64_t timestamp = sample_time_us;
    bool available = sample_available;
    portEXIT_CRITICAL(&sample_lock);

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (!available) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "{\"error\":\"Waiting for sensor readings\"}");
    }

    char response[160];
    int length = snprintf(response, sizeof(response),
        "{\"temperature_c\":%.2f,\"humidity_pct\":%.2f,\"age_ms\":%lld}",
        (double)temperature, (double)humidity,
        (long long)((esp_timer_get_time() - timestamp) / 1000));
    if (length < 0 || length >= (int)sizeof(response)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot format readings");
    }
    return httpd_resp_send(request, response, length);
}

esp_err_t monitor_web_start(void)
{
    // The Wi-Fi event loop calls this again after reconnection.
    if (server != NULL) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK) {
        server = NULL;
        return result;
    }
    const httpd_uri_t page = {
        .uri = "/", .method = HTTP_GET, .handler = page_handler,
    };
    const httpd_uri_t readings = {
        .uri = "/api/readings", .method = HTTP_GET, .handler = readings_handler,
    };
    result = httpd_register_uri_handler(server, &page);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &readings);
    }
    if (result != ESP_OK) {
        httpd_stop(server);
        server = NULL;
    }
    return result;
}
