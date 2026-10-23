#pragma once
#include <stdint.h>
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle);
