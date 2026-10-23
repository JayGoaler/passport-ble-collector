#pragma once
#include <stdint.h>
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
void fake_critical_enter(portMUX_TYPE *lock);
void fake_critical_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) fake_critical_enter(lock)
#define portEXIT_CRITICAL(lock) fake_critical_exit(lock)
int xTaskCreate(void (*entry)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *handle);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t handle);
