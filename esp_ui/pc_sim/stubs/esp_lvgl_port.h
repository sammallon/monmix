// Stub of esp_lvgl_port.h. Tablet uses the espressif/esp_lvgl_port managed
// component to lock the LVGL task; here we use a single OS mutex backed
// by SDL (mock_esp_lvgl_port.c). Same rule applies: any non-LVGL caller
// that touches widgets, the timer list, or lv_async_call MUST take this
// lock. The PC sim's only "non-LVGL" task today is the SDL event pump,
// which we serialize with the LVGL handler in pc_main.c, so practical
// contention is rare — but the lock is there if mocks ever spawn a thread.
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);
