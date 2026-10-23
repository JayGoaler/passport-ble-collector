#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define pdTRUE 1
#define pdPASS 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define MALLOC_CAP_8BIT 1
#define ESP_LOGE(...) ((void)0)
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef struct fake_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned n,unsigned size);
int xQueueSend(QueueHandle_t q,const void *p,unsigned ticks);
int xQueueReceive(QueueHandle_t q,void *p,unsigned ticks);
int xQueueOverwrite(QueueHandle_t q,const void *p);
unsigned uxQueueMessagesWaiting(QueueHandle_t q);
int xTaskCreate(void (*fn)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *);
void vTaskDelay(unsigned ticks);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t);
unsigned esp_get_free_heap_size(void);
unsigned esp_get_minimum_free_heap_size(void);
unsigned heap_caps_get_largest_free_block(unsigned flags);
uint32_t esp_random(void);
void esp_fill_random(void *p,size_t n);
esp_err_t bsp_battery_init(void);
int bsp_battery_soc(void);
enum { BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK };
enum { BSP_BTN_PRESS, BSP_BTN_CLICK, BSP_BTN_DOUBLE, BSP_BTN_LONG };
