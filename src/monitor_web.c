#include "monitor_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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

#define HISTORY_INTERVAL_SECONDS 20
#define HISTORY_RETENTION_SECONDS (24 * 60 * 60)
#define HISTORY_CAPACITY (HISTORY_RETENTION_SECONDS / HISTORY_INTERVAL_SECONDS)

typedef struct {
    uint32_t uptime_seconds;
    float temperature_c;
    float humidity_pct;
} history_sample_t;

// About 51 KiB in RAM; populated by the sensor task even without HTTP clients.
static history_sample_t history[HISTORY_CAPACITY];
static size_t history_next;
static size_t history_count;
static uint32_t last_history_seconds;

void monitor_web_update(float temperature_c, float humidity_pct)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&sample_lock);
    latest_temperature = temperature_c;
    latest_humidity = humidity_pct;
    sample_time_us = now;
    sample_available = true;
    uint32_t seconds = (uint32_t)(now / 1000000);
    if (history_count == 0 || seconds - last_history_seconds >= HISTORY_INTERVAL_SECONDS) {
        history[history_next] = (history_sample_t) {
            .uptime_seconds = seconds,
            .temperature_c = temperature_c,
            .humidity_pct = humidity_pct,
        };
        history_next = (history_next + 1) % HISTORY_CAPACITY;
        if (history_count < HISTORY_CAPACITY) ++history_count;
        last_history_seconds = seconds;
    }
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

static esp_err_t history_handler(httpd_req_t *request)
{
    // Snapshot before sending: never hold the sensor lock during network I/O.
    history_sample_t *snapshot = malloc(sizeof(history));
    if (snapshot == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "History temporarily unavailable");
    }
    portENTER_CRITICAL(&sample_lock);
    size_t count = history_count;
    size_t oldest = (history_next + HISTORY_CAPACITY - count) % HISTORY_CAPACITY;
    for (size_t i = 0; i < count; ++i) {
        snapshot[i] = history[(oldest + i) % HISTORY_CAPACITY];
    }
    portEXIT_CRITICAL(&sample_lock);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr_chunk(request,
        "{\"interval_seconds\":20,\"samples\":[");
    // Batch records to avoid one TCP write per sample on a full day's history.
    char chunk[1024];
    size_t used = 0;
    bool first = true;
    for (size_t i = 0; result == ESP_OK && i < count; ++i) {
        uint32_t age = now - snapshot[i].uptime_seconds;
        if (age >= HISTORY_RETENTION_SECONDS) continue;
        char record[96];
        int length = snprintf(record, sizeof(record), "%s[%lu,%.2f,%.2f]",
            first ? "" : ",", (unsigned long)age,
            (double)snapshot[i].temperature_c, (double)snapshot[i].humidity_pct);
        if (length < 0 || length >= (int)sizeof(record)) {
            result = ESP_FAIL;
            break;
        }
        if (used + (size_t)length > sizeof(chunk)) {
            result = httpd_resp_send_chunk(request, chunk, used);
            used = 0;
        }
        if (result != ESP_OK) break;
        for (int j = 0; j < length; ++j) chunk[used++] = record[j];
        first = false;
    }
    free(snapshot);
    if (result == ESP_OK && used > 0) result = httpd_resp_send_chunk(request, chunk, used);
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(request, "]}");
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, NULL, 0);
    return result;
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
    const httpd_uri_t stored_history = {
        .uri = "/api/history", .method = HTTP_GET, .handler = history_handler,
    };
    result = httpd_register_uri_handler(server, &page);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &readings);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &stored_history);
    }
    if (result != ESP_OK) {
        httpd_stop(server);
        server = NULL;
    }
    return result;
}
