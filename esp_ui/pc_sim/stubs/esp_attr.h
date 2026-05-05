// Stub of esp_attr.h — IDF placement/section attributes are no-ops on PC.
#pragma once

#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_RODATA_ATTR
#define RTC_NOINIT_ATTR
#define WORD_ALIGNED_ATTR
#define DMA_ATTR
#define DMA_ATTR_ALIGNED(n)
#define _SECTION_ATTR_IMPL(SECTION, COUNTER)
#define COREDUMP_DRAM_ATTR
#define COREDUMP_IRAM_ATTR
#define COREDUMP_RTC_ATTR
#define COREDUMP_RTC_FAST_ATTR
