// SDL_mutex-backed lvgl_port_lock. The sim doesn't have multiple LVGL
// callers today (we serialise SDL events with the LVGL handler in
// pc_main.c), but the lock has to actually exist so app_ui.c's
// "non-LVGL task" call sites all succeed. Initialised lazily on first
// take() so we don't depend on the SDL_Init order.
#include "esp_lvgl_port.h"

#include <SDL.h>
#include <stdbool.h>

static SDL_mutex *s_mutex;

bool lvgl_port_lock(uint32_t timeout_ms) {
    (void)timeout_ms;  // SDL_LockMutex blocks indefinitely; good enough.
    if (!s_mutex) s_mutex = SDL_CreateMutex();
    return SDL_LockMutex(s_mutex) == 0;
}

void lvgl_port_unlock(void) {
    if (s_mutex) SDL_UnlockMutex(s_mutex);
}
