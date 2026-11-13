#include "monitor_wifi.h"
#include "monitor_web.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#else
#include "wifi_credentials.example.h"
#endif

#define ANSI_RESET "\033[0m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"

static const char *TAG = "monitor_wifi";

static void connect_to_wifi(void)
{
    esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connection request failed: %s", esp_err_to_name(result));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting to Wi-Fi...");
        connect_to_wifi();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, ANSI_YELLOW "Wi-Fi disconnected (reason %u); reconnecting..." ANSI_RESET,
                 (unsigned)event->reason);
        connect_to_wifi();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, ANSI_GREEN "Wi-Fi connected! IP address: " IPSTR ANSI_RESET,
                 IP2STR(&event->ip_info.ip));
        esp_err_t result = monitor_web_start();
        if (result == ESP_OK) {
            ESP_LOGI(TAG, ANSI_GREEN "Open http://" IPSTR "/" ANSI_RESET,
                     IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGE(TAG, "Web server failed to start: %s", esp_err_to_name(result));
        }
    }
}

void monitor_wifi_start(void)
{
    if (sizeof(MONITOR_WIFI_SSID) <= 1) {
        ESP_LOGW(TAG, ANSI_YELLOW "Wi-Fi disabled: fill in include/wifi_credentials.h and rebuild" ANSI_RESET);
        return;
    }

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL));

    _Static_assert(sizeof(MONITOR_WIFI_SSID) - 1 <= 32, "Wi-Fi SSID exceeds 32 bytes");
    _Static_assert(sizeof(MONITOR_WIFI_PASSWORD) - 1 <= 64, "Wi-Fi password exceeds 64 bytes");
    wifi_config_t config = {
        .sta = {
            .ssid = MONITOR_WIFI_SSID,
            .password = MONITOR_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
}
