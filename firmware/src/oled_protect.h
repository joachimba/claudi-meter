#pragma once
#include <stdint.h>

// OLED burn-in safety. The CO5300 panel is AMOLED — static high-contrast
// elements (the corner logo, fixed status text, etc.) will leave permanent
// ghosting after enough hours. This module:
//
//   1. Tracks an "activity" timestamp; bumped on every user action, BLE
//      data write, or screen change.
//   2. Steps the panel brightness down after configurable idle windows
//      (200 → 80 → 30 → off).
//   3. Periodically nudges the logo + battery icon ±1 px so their
//      individual pixels age more evenly.
//
// `oled_protect_init()` must be called after `gfx` + UI exist.

void oled_protect_init(void);
void oled_protect_tick(void);
void oled_protect_note_activity(void);    // call on touch / btn / BLE / screen change
bool oled_protect_is_dark(void);          // true when panel is fully blanked
