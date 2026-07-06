#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_HISTORY,
    SCREEN_SPOTIFY,
    SCREEN_CALENDAR,
    SCREEN_POMODORO,
    SCREEN_PHOTO,
    SCREEN_BTACC,
    SCREEN_SETTINGS,
    SCREEN_BLUETOOTH,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_update_spotify(const SpotifyData* data);
void ui_update_spotify_art(const uint8_t* rgb565, int w, int h);
void ui_update_calendar(const CalendarData* data);
void ui_update_photo_caption(const PhotoData* data);
void ui_update_photo_image(const uint8_t* rgb565);
void ui_update_bt_accessories(const BTAccessoriesData* data);
void ui_update_history(const HistoryData* data);
void ui_set_afk(bool on);
void ui_set_attention(bool on);   // bounce the top-left Clawd logo when Claude Code needs you
void ui_refresh_settings(void);   // sync Settings screen widgets to settings.cpp state
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_cycle_screen_auto(void);   // auto-rotate cycle: Usage→Spotify→Calendar (skips Settings/BT)
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
