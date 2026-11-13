# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

set(app_sources "main.c" "monitor_wifi.c" "monitor_web.c")

idf_component_register(SRCS ${app_sources}
    INCLUDE_DIRS "../include"
    PRIV_REQUIRES esp_adc esp_common log freertos esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_timer
    EMBED_TXTFILES "web/index.html")
