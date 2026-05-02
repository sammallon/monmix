#pragma once

#include <stdbool.h>

// One-shot fetch of `/console/information` over HTTP. Tells us, for the
// currently-connected console, how many channels of each type exist
// and where they sit in the dotted-path namespace (`ch.<offset+i>.*`).
//
// Schema (per OpenAPI):
//   { totalChannels, channelTypes:[{name, count, offset, stereo, ...}], ... }
//
// We only care about the named ranges below — anything else in the
// response is ignored.
typedef struct {
    int total;          // totalChannels
    int input_offset;   // ch range for `Input` type (mono input strips)
    int input_count;
    int aux_offset;     // ch range for `Aux` type
    int aux_count;
    int mix_offset;     // ch range for `Mix` type — drives the mix bus selector
    int mix_count;
    int matrix_offset;  // ch range for `Matrix` type
    int matrix_count;
} app_ms_info_t;

// Blocking HTTP GET on http://<host>:<port>/console/information. Call
// from a non-LVGL worker task — ~50 ms typical, 5 s timeout. Returns
// true on success and fills *out; logs and returns false on error.
bool app_ms_info_fetch(const char *host, int port, app_ms_info_t *out);
