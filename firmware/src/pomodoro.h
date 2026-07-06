#pragma once
#include <stdint.h>

// Lightweight Pomodoro timer state machine. Runs irrespective of which
// screen is active, so the countdown keeps ticking while the user navigates
// elsewhere. Defaults: 25 min work, 5 min short break, 15 min long break
// after every four completed work sessions.
enum pomo_state_t {
    POMO_IDLE,
    POMO_WORKING,
    POMO_BREAK,
};

void     pomodoro_init(void);
void     pomodoro_tick(void);          // call from main loop

void     pomodoro_start_work(void);    // (re)start the work phase
void     pomodoro_toggle_pause(void);  // pause / resume current phase
void     pomodoro_reset(void);         // back to IDLE

pomo_state_t pomodoro_state(void);
bool         pomodoro_paused(void);
uint32_t     pomodoro_remaining_ms(void);
uint32_t     pomodoro_phase_total_ms(void);  // length of the current phase
uint16_t     pomodoro_completed_work(void);  // # of finished work cycles
