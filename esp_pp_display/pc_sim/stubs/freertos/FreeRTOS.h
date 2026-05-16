// Stub of freertos/FreeRTOS.h. app_state.c uses xSemaphoreCreateMutex/
// xSemaphoreTake/xSemaphoreGive for its internal lock. We back those with
// SDL_mutex (mock_freertos.c). pdMS_TO_TICKS / portMAX_DELAY are kept
// numerically compatible with FreeRTOS so timing constants don't drift if
// app_state ever picks one up.
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef uint32_t UBaseType_t;

#define pdTRUE         1
#define pdFALSE        0
#define pdPASS         1
#define pdFAIL         0
#define portMAX_DELAY  ((TickType_t)0xFFFFFFFFu)

// 1 tick == 1 ms in the sim — same as the tablet's CONFIG_FREERTOS_HZ=1000.
// sim_compat.h also defines this (force-included into every TU); the
// guard here keeps both paths happy.
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif
