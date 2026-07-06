// Minimal Arduino.h shim for the desktop sim. Firmware sources include this
// header transitively; on the ESP it's the real Arduino core. On the desktop
// we only need a handful of types and a Serial-like sink for printf.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <chrono>

inline uint32_t millis(void) {
    static auto t0 = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
}

inline void delay(uint32_t ms) {
    (void)ms;  // sim runs the main loop at its own pace
}

struct SerialSink {
    void begin(unsigned long) {}
    int available() { return 0; }
    int read() { return -1; }
    size_t write(const uint8_t*, size_t) { return 0; }
    void flush() { ::fflush(stdout); }
    int println() { puts(""); return 1; }
    int println(const char* s) { puts(s); return 1; }
    int print(const char* s) { fputs(s, stdout); return 0; }
    int printf(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int r = vprintf(fmt, ap);
        va_end(ap);
        return r;
    }
};
extern SerialSink Serial;

#ifndef __APPLE__
inline size_t strlcpy(char* dst, const char* src, size_t size) {
    size_t srclen = ::strlen(src);
    if (size > 0) {
        size_t n = (srclen >= size) ? size - 1 : srclen;
        ::memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return srclen;
}
#endif
