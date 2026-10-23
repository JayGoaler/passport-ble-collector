#pragma once
#include <stdint.h>
typedef void *QueueHandle_t;
int xQueueSend(QueueHandle_t queue, const void *item, uint32_t wait);
int xQueueReceive(QueueHandle_t queue, void *item, uint32_t wait);
