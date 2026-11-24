// Host stand-ins for the ESP-IDF/FreeRTOS declarations the C6 radio firmware uses (fuzzing build only).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define MALLOC_CAP_8BIT 4
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGV(tag, ...) ((void)(tag))
TaskHandle_t xTaskGetCurrentTaskHandle(void);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
esp_err_t esp_event_loop_create_default(void);
int esp_reset_reason(void);
esp_err_t uart_wait_tx_done(int port, TickType_t ticks);
size_t heap_caps_get_free_size(uint32_t caps);
int64_t esp_timer_get_time(void);
