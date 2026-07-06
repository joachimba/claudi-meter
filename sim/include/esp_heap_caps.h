// esp_heap_caps.h shim for desktop sim — maps PSRAM allocations to regular malloc.
#pragma once

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM    0x01
#define MALLOC_CAP_INTERNAL  0x02
#define MALLOC_CAP_DMA       0x04
#define MALLOC_CAP_DEFAULT   0x08

inline void* heap_caps_malloc(size_t size, unsigned int /*caps*/) { return malloc(size); }
inline void  heap_caps_free(void* p) { free(p); }
