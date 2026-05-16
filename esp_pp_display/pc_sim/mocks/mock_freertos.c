// SDL_mutex-backed semphr.h surface. app_state.c is the only consumer.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <SDL.h>

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)SDL_CreateMutex();
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout) {
    (void)timeout;
    return SDL_LockMutex((SDL_mutex *)s) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    return SDL_UnlockMutex((SDL_mutex *)s) == 0 ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t s) {
    SDL_DestroyMutex((SDL_mutex *)s);
}
