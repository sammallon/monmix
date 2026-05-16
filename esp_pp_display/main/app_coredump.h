#pragma once

#include <stdbool.h>

// Copy any pending coredump from the `coredump` flash partition to the SD
// card as `/sdcard/coredump-NNNN.elf` and erase the partition.
//
// Call once early in app_main, after app_storage_init(). Safe in all states:
//   - clean boot (no dump):     logs, returns false
//   - dump present, SD missing: logs, leaves dump in place, returns false
//   - dump present, SD ok:      writes file, erases partition, returns true
//   - dump corrupted:           logs, leaves dump in place, returns false
//
// Decode host-side with:  espcoredump.py info_corefile -c <file> build/esp_ui.elf
bool app_coredump_flush_to_sd(void);
