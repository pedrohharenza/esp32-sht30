#pragma once

#include "esp_err.h"

esp_err_t monitor_web_start(void);
void monitor_web_update(float temperature_c, float humidity_pct);
