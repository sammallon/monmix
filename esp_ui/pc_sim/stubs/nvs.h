// Stub of nvs.h. app_config.c and app_prefs.c open NVS handles for
// persistent storage on the tablet. The sim's mocks bypass NVS entirely
// (in-memory state is fine for the bug we're chasing), but app_state.c
// pulls in nothing here, and app_config.c does — we provide a mock
// app_config.c so the real one isn't compiled.
//
// This stub stays minimal since no compiled TU should actually call into
// it; if a future PC-build TU does, the linker will catch it.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef int      esp_err_t;
typedef uint32_t nvs_handle_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102

typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
