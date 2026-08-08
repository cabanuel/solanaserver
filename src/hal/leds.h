/*
  The two WS2812B RGB LEDs on GPIO2, clocked out by the RMT peripheral.

  Not bit-banged: this firmware runs Wi-Fi and BLE, whose high-priority
  interrupts noInterrupts() does not mask, and one of those landing inside a
  60us frame corrupts every bit after it. See the note in begin().

  Also owns the boot animation, which runs as a state machine advanced by
  update() rather than a blocking sequence - the splash screen's backlight fades
  call update() from their step hook, so the lights and the logo animate
  together instead of taking turns.
*/
#pragma once

#include <Arduino.h>

#include "../config.h"

namespace leds {

void begin();

void set(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void setAll(uint8_t r, uint8_t g, uint8_t b);
void off();
void show();

// 0..255, scales everything written to the strip. WS2812s at full brightness
// are genuinely uncomfortable at badge-on-a-lanyard distance.
void setBrightness(uint8_t value);
uint8_t brightness();

// ---------------------------------------------------------------------------
// Animations. Each replaces whatever was playing; update() drives them and
// leaves the LEDs alone once an animation finishes.
// ---------------------------------------------------------------------------

// The boot sequence: a purple->green wipe, a gradient wave travelling across
// both LEDs, three green heartbeats, then a fade to black. Roughly 4.2s.
void playBoot();

// A single coloured pulse that decays over `durationMs` - button feedback,
// notifications, "app installed".
void pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t durationMs);

// The at-rest animation: a slow fade in and out, 6s a breath, with the colour
// walking around the hue wheel over 90s so no two breaths are quite the same.
// Never reaches full black - see IDLE_FLOOR.
void playIdle();

// True while a scripted animation still has frames to render.
bool animating();

// Stops any animation and hands the LEDs back to whoever set them last.
void stopAnimation();

void update();

}  // namespace leds
