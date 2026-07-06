#include "settings.h"
#include <Arduino.h>

#ifndef CLAWDMETER_SIM_BUILD
#include <Preferences.h>
static Preferences prefs;
static constexpr const char* NS = "clawd";
static constexpr const char* KEY_AUTO_ROTATE   = "auto_rot";
static constexpr const char* KEY_ROTATE_SEC    = "rot_sec";
#endif

static bool     s_auto_rotate = false;
static uint16_t s_rotate_sec  = 60;

void settings_init(void) {
#ifndef CLAWDMETER_SIM_BUILD
    if (!prefs.begin(NS, /*readOnly=*/false)) {
        Serial.println("settings: NVS open failed");
        return;
    }
    s_auto_rotate = prefs.getBool(KEY_AUTO_ROTATE, false);
    s_rotate_sec  = prefs.getUShort(KEY_ROTATE_SEC, 60);
#endif
    Serial.printf("settings: auto_rotate=%d rotate_sec=%u\n",
                  s_auto_rotate ? 1 : 0, (unsigned)s_rotate_sec);
}

bool settings_auto_rotate(void) { return s_auto_rotate; }

void settings_set_auto_rotate(bool on) {
    if (on == s_auto_rotate) return;
    s_auto_rotate = on;
#ifndef CLAWDMETER_SIM_BUILD
    prefs.putBool(KEY_AUTO_ROTATE, on);
#endif
    Serial.printf("settings: auto_rotate -> %d\n", on ? 1 : 0);
}

uint16_t settings_rotate_interval_sec(void) { return s_rotate_sec; }

void settings_set_rotate_interval_sec(uint16_t s) {
    if (s < 5)   s = 5;
    if (s > 3600) s = 3600;
    if (s == s_rotate_sec) return;
    s_rotate_sec = s;
#ifndef CLAWDMETER_SIM_BUILD
    prefs.putUShort(KEY_ROTATE_SEC, s);
#endif
}
