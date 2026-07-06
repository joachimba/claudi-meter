#pragma once
#include <stdint.h>

// User-tweakable settings, persisted in NVS via the Arduino Preferences
// wrapper. `settings_init()` must be called once after Serial + Wire are up.
void settings_init(void);

bool     settings_auto_rotate(void);
void     settings_set_auto_rotate(bool on);

uint16_t settings_rotate_interval_sec(void);   // default 60s
void     settings_set_rotate_interval_sec(uint16_t s);
