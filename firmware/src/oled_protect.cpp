#include "oled_protect.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <lvgl.h>

#define BRIGHTNESS_FULL    200
#define BRIGHTNESS_DIM1    80
#define BRIGHTNESS_DIM2    30
#define BRIGHTNESS_OFF     0

#define IDLE_DIM1_MS       (5UL  * 60UL * 1000UL)
#define IDLE_DIM2_MS       (15UL * 60UL * 1000UL)
#define IDLE_OFF_MS        (60UL * 60UL * 1000UL)

#define PIXEL_SHIFT_PERIOD_MS 30000UL
#define PIXEL_SHIFT_RANGE     2

static uint32_t s_last_activity_ms = 0;
static uint8_t  s_current_brightness = BRIGHTNESS_FULL;
static uint32_t s_last_shift_ms = 0;
static int8_t   s_shift_x = 0;
static int8_t   s_shift_y = 0;

// Per-tick xorshift for the shift jitter so we don't always land on the same
// 4 corners.
static uint32_t xorshift32(void) {
    static uint32_t s = 0xC0DEFEEDu;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void apply_brightness(uint8_t b) {
    if (b == s_current_brightness) return;
    s_current_brightness = b;
#ifndef CLAWDMETER_SIM_BUILD
    if (gfx) gfx->setBrightness(b);
#endif
    Serial.printf("oled: brightness -> %u\n", (unsigned)b);
}

void oled_protect_init(void) {
    s_last_activity_ms = millis();
    s_last_shift_ms = s_last_activity_ms;
    s_current_brightness = BRIGHTNESS_FULL;
}

void oled_protect_note_activity(void) {
    s_last_activity_ms = millis();
    if (s_current_brightness != BRIGHTNESS_FULL) {
        apply_brightness(BRIGHTNESS_FULL);
    }
}

void oled_protect_tick(void) {
    uint32_t now = millis();
    uint32_t idle = now - s_last_activity_ms;

    // Brightness stepping
    uint8_t target;
    if      (idle >= IDLE_OFF_MS)  target = BRIGHTNESS_OFF;
    else if (idle >= IDLE_DIM2_MS) target = BRIGHTNESS_DIM2;
    else if (idle >= IDLE_DIM1_MS) target = BRIGHTNESS_DIM1;
    else                            target = BRIGHTNESS_FULL;
    if (target != s_current_brightness) apply_brightness(target);

    // Pixel shift — applied via LVGL screen-level translate. Cheap and
    // imperceptible at ±2 px, but moves static UI pixels onto neighbours.
    if (now - s_last_shift_ms >= PIXEL_SHIFT_PERIOD_MS) {
        s_last_shift_ms = now;
        uint32_t r = xorshift32();
        s_shift_x = (int8_t)((r        & 0x3) - 1);  // -1..2 -> clamp
        s_shift_y = (int8_t)(((r >> 2) & 0x3) - 1);
        if (s_shift_x > PIXEL_SHIFT_RANGE) s_shift_x = PIXEL_SHIFT_RANGE;
        if (s_shift_y > PIXEL_SHIFT_RANGE) s_shift_y = PIXEL_SHIFT_RANGE;
        if (s_shift_x < -PIXEL_SHIFT_RANGE) s_shift_x = -PIXEL_SHIFT_RANGE;
        if (s_shift_y < -PIXEL_SHIFT_RANGE) s_shift_y = -PIXEL_SHIFT_RANGE;
        lv_obj_t* scr = lv_screen_active();
        if (scr) {
            lv_obj_set_style_translate_x(scr, s_shift_x, 0);
            lv_obj_set_style_translate_y(scr, s_shift_y, 0);
            lv_obj_invalidate(scr);
        }
    }
}
