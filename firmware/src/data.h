#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

struct SpotifyData {
    char track[96];
    char artist[64];
    bool playing;
    bool valid;
    uint32_t bg_color;   // 0xRRGGBB dominant colour from album art, 0 = no tint
    bool has_color;
};

struct CalendarData {
    char title[96];
    int mins_until;          // <0 = none / unknown
    bool valid;
};

#define HISTORY_MAX_DAYS 30
struct HistoryData {
    // Daily token usage, oldest -> newest, in units of 100k tokens.
    uint16_t vals[HISTORY_MAX_DAYS];
    uint32_t cost_cents[HISTORY_MAX_DAYS];  // equivalent USD cost per day, in cents
    uint8_t  count;        // number of valid token entries (<= HISTORY_MAX_DAYS)
    bool     valid;
    bool     cost_valid;   // true once a cost ("ck") payload has arrived
};

struct VacationData {
    char destination[64];   // "Tokyo, Japan"
    char date_label[40];    // preformatted, e.g. "1 Sep 2026"
    int  days;              // days until trip start; 0 = today, <0 = under way
    bool active;            // false when no trip is configured
    bool valid;
};
