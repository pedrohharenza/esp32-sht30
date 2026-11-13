#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "monitor_wifi.h"
#include "monitor_web.h"

static const char *TAG = "monitor";

// Reset after each field so ANSI colors do not leak into later output.
#define ANSI_RESET   "\033[0m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN    "\033[36m"

// ADC1 remains available when Wi-Fi is enabled on the classic ESP32.
#define TEMPERATURE_CHANNEL ADC_CHANNEL_6  // GPIO34
#define HUMIDITY_CHANNEL    ADC_CHANNEL_7  // GPIO35
#define SAMPLE_COUNT 64
// Replace with the measured sensor VDD in millivolts for better accuracy.
#define SENSOR_SUPPLY_MV 3300.0f

static int read_voltage_mv(adc_oneshot_unit_handle_t adc,
                           adc_cali_handle_t calibration,
                           adc_channel_t channel)
{
    int total_mv = 0;
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        int raw;
        int voltage_mv;
        ESP_ERROR_CHECK(adc_oneshot_read(adc, channel, &raw));
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(calibration, raw, &voltage_mv));
        total_mv += voltage_mv;
    }
    return total_mv / SAMPLE_COUNT;
}

void app_main(void)
{
    monitor_wifi_start();

    adc_oneshot_unit_handle_t adc;
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &adc));

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, TEMPERATURE_CHANNEL, &channel_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, HUMIDITY_CHANNEL, &channel_config));

    adc_cali_line_fitting_efuse_val_t calibration_source;
    ESP_ERROR_CHECK(adc_cali_scheme_line_fitting_check_efuse(&calibration_source));
    if (calibration_source == ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF) {
        ESP_LOGW(TAG, ANSI_YELLOW "No factory ADC calibration; voltages use an approximate 1100 mV reference" ANSI_RESET);
    }

    adc_cali_handle_t calibration;
    adc_cali_line_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .default_vref = 1100,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&calibration_config, &calibration));

    // Allow the sensor to produce its first measurement after power-up.
    vTaskDelay(pdMS_TO_TICKS(1000));
    while (1) {
        int temperature_mv = read_voltage_mv(adc, calibration, TEMPERATURE_CHANNEL);
        int humidity_mv = read_voltage_mv(adc, calibration, HUMIDITY_CHANNEL);
        // SHT3x-ARP datasheet: outputs are proportional to the sensor supply.
        float temperature_c = -66.875f + 218.75f * temperature_mv / SENSOR_SUPPLY_MV;
        float humidity_pct = -12.5f + 125.0f * humidity_mv / SENSOR_SUPPLY_MV;
        monitor_web_update(temperature_c, humidity_pct);

        ESP_LOGI(TAG, ANSI_MAGENTA "T (GPIO34): %d mV" ANSI_RESET
                 " | " ANSI_CYAN "RH (GPIO35): %d mV" ANSI_RESET,
                 temperature_mv, humidity_mv);
        ESP_LOGI(TAG, ANSI_MAGENTA "Temperature: %.2f C" ANSI_RESET
                 " | " ANSI_CYAN "Humidity: %.2f %%" ANSI_RESET,
                 temperature_c, humidity_pct);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
