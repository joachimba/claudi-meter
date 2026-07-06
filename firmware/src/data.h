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

struct PhotoData {
    char caption[96];        // photographer / place text from daemon
    bool valid;
};

#define BT_ACC_MAX 6
struct BTAccessory {
    char name[40];
    int  battery_pct;    // 0..100; -1 if unknown
    bool charging;
};
struct BTAccessoriesData {
    BTAccessory items[BT_ACC_MAX];
    uint8_t count;
    bool valid;
};
