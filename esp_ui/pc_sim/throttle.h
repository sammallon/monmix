// Make the sim respond more like the tablet — single core affinity,
// below-normal priority, frame-rate cap. Toggled by --throttle on the
// command line. None of this is meant to faithfully reproduce ESP32-P4
// performance; the goal is just to widen the timing windows enough that
// race conditions show up here instead of only on hardware.
#ifndef PC_SIM_THROTTLE_H
#define PC_SIM_THROTTLE_H

#include <stdbool.h>
#include <stdint.h>

void throttle_apply(void);

// Frame-period target in milliseconds when throttle is active. 33 ms ≈
// 30 Hz — same ballpark as the tablet's LVGL refresh budget. 0 means
// unthrottled (let LVGL/SDL idle naturally).
uint32_t throttle_frame_ms(void);

bool throttle_active(void);

#endif
