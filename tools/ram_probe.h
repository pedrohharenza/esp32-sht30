/* Optional diagnostic helper; not included by the production firmware.
 * Add #include "../tools/ram_probe.h" in a source file, then call
 * monitor_ram_probe("stage name") outside critical sections.
 * Intended stages: before/after Wi-Fi init/start, after HTTP start,
 * immediately after history snapshot malloc, after sending/freeing it.
 * Do not log inside the sample_lock critical section.
 */
#pragma once

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline void monitor_ram_probe(const char *stage)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI("ram_probe",
             "%s: allocated=%u free=%u minimum_free=%u largest_block=%u "
             "task_stack_free_min=%u bytes",
             stage, (unsigned)info.total_allocated_bytes,
             (unsigned)info.total_free_bytes,
             (unsigned)info.minimum_free_bytes,
             (unsigned)info.largest_free_block,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
