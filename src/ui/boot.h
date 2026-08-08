/*
  The boot sequence: Solana logo, then the SKYRIZZ credit, with the RGB LEDs
  running the brand gradient underneath.

  The two are deliberately interleaved rather than sequential. The backlight
  fades block for a few hundred milliseconds each, so every wait in here calls
  leds::update() from its step hook - the lights animate through the logo
  instead of waiting their turn.
*/
#pragma once

#include <Arduino.h>

namespace boot {

// Runs the whole sequence. Blocking, and only ever called from setup().
void run();

// A progress card for the slower init steps, drawn between the splashes.
void progress(const char *step, const char *detail, uint8_t percent);

}  // namespace boot
