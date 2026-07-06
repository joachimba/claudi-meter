#include "pomodoro.h"
#include <Arduino.h>

#define WORK_MS         (25UL * 60UL * 1000UL)
#define SHORT_BREAK_MS  ( 5UL * 60UL * 1000UL)
#define LONG_BREAK_MS   (15UL * 60UL * 1000UL)
#define LONG_BREAK_EVERY 4

static pomo_state_t s_state    = POMO_IDLE;
static bool         s_paused   = false;
static uint32_t     s_phase_total_ms = 0;
static uint32_t     s_end_ms   = 0;       // millis() at which the phase ends
static uint32_t     s_pause_remaining_ms = 0;
static uint16_t     s_completed_work = 0;

static void start_phase(pomo_state_t state, uint32_t length_ms) {
    s_state = state;
    s_paused = false;
    s_phase_total_ms = length_ms;
    s_end_ms = millis() + length_ms;
    s_pause_remaining_ms = 0;
    Serial.printf("pomo: -> state=%d len=%lums\n", (int)state, (unsigned long)length_ms);
}

void pomodoro_init(void) {
    s_state = POMO_IDLE;
    s_paused = false;
    s_phase_total_ms = 0;
    s_end_ms = 0;
    s_pause_remaining_ms = 0;
    s_completed_work = 0;
}

void pomodoro_start_work(void) {
    start_phase(POMO_WORKING, WORK_MS);
}

void pomodoro_reset(void) {
    pomodoro_init();
    Serial.println("pomo: reset");
}

void pomodoro_toggle_pause(void) {
    if (s_state == POMO_IDLE) {
        pomodoro_start_work();
        return;
    }
    if (s_paused) {
        s_end_ms = millis() + s_pause_remaining_ms;
        s_paused = false;
        Serial.println("pomo: resume");
    } else {
        uint32_t now = millis();
        s_pause_remaining_ms = (s_end_ms > now) ? (s_end_ms - now) : 0;
        s_paused = true;
        Serial.printf("pomo: pause (rem=%lums)\n", (unsigned long)s_pause_remaining_ms);
    }
}

void pomodoro_tick(void) {
    if (s_state == POMO_IDLE || s_paused) return;
    uint32_t now = millis();
    if ((int32_t)(now - s_end_ms) >= 0) {
        // Phase complete — advance state machine.
        if (s_state == POMO_WORKING) {
            s_completed_work++;
            bool long_break = (s_completed_work % LONG_BREAK_EVERY) == 0;
            start_phase(POMO_BREAK, long_break ? LONG_BREAK_MS : SHORT_BREAK_MS);
        } else {  // POMO_BREAK -> back to work
            start_phase(POMO_WORKING, WORK_MS);
        }
    }
}

pomo_state_t pomodoro_state(void)          { return s_state; }
bool         pomodoro_paused(void)         { return s_paused; }
uint32_t     pomodoro_phase_total_ms(void) { return s_phase_total_ms; }
uint16_t     pomodoro_completed_work(void) { return s_completed_work; }

uint32_t pomodoro_remaining_ms(void) {
    if (s_state == POMO_IDLE) return 0;
    if (s_paused) return s_pause_remaining_ms;
    uint32_t now = millis();
    return (s_end_ms > now) ? (s_end_ms - now) : 0;
}
