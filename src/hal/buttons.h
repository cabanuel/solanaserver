/*
  The six push buttons behind the TCA9534 I2C expander.

  update() runs once per main-loop tick and computes the edge masks for that
  tick. Both the shell and the Lua runtime read those masks, and only one of the
  two is ever the foreground consumer, so there is no need for a queue.

  Logical key indices (BTN_UP, BTN_A, ...) come from config.h.
*/
#pragma once

#include <Arduino.h>

#include "../config.h"

namespace buttons {

bool begin();
bool present();

void update();

// State for the current tick.
bool down(uint8_t key);      // held right now
bool pressed(uint8_t key);   // went down during this tick
bool released(uint8_t key);  // came up during this tick

// pressed() OR an auto-repeat tick while held. Menus want this; games usually
// want down() or pressed().
bool repeated(uint8_t key);

uint8_t downMask();
uint8_t pressedMask();

// How long `key` has been held, in ms; 0 when it is not down.
uint32_t heldMs(uint8_t key);

// "PB5 (A)" - for the on-screen key legend and Lua's input.name().
const char *name(uint8_t key);
const char *shortName(uint8_t key);

// Re-runs the expander init. update() already calls this by itself every few
// seconds while the expander is not answering, so the badge recovers its own
// input without help; this is for Settings > Device info, which asks for one
// immediately.
bool retry();

}  // namespace buttons
