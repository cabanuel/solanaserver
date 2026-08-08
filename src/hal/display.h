/*
  Display.

  Everything in Solana OS - the shell and every Lua app alike - draws into one
  320x240 RGB565 sprite held in PSRAM, and flush() blits it to the panel in a
  single transfer. Nothing paints the glass directly.

  That single rule is what makes Lua apps flicker-free without asking app
  authors to think about it: a half-finished frame never reaches the panel, and
  a frame that did not change costs no SPI traffic at all.
*/
#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

namespace display {

bool begin();

// The framebuffer every drawing call should target.
LGFX_Sprite &canvas();

// The panel itself. Only the splash path and the backlight use this.
lgfx::LGFX_Device &panel();

int16_t width();
int16_t height();

// Marks the canvas as changed. Drawing helpers call this for you; raw canvas()
// users must call it themselves or their frame may not be pushed.
void touch();

// Blits the canvas to the panel if anything marked it dirty. Returns true if a
// transfer actually happened.
bool flush();

// Pushes on the next flush() whether or not anything changed.
void invalidate();

void setBrightness(uint8_t value);
uint8_t brightness();

// Ramps the backlight. `stepHook`, when given, runs on every step so the LED
// animation keeps advancing during a blocking fade.
void fadeTo(uint8_t target, uint16_t durationMs, void (*stepHook)() = nullptr);

// ---------------------------------------------------------------------------
// Drawing helpers shared by the shell. Lua gets its own, richer set in
// lib_gfx.cpp; these are the handful the C++ UI actually needs.
// ---------------------------------------------------------------------------
void text(const char *string, int x, int y, uint16_t color, uint8_t size = 1);
void textCentered(const char *string, int centerX, int y, uint16_t color, uint8_t size = 1);
void textRight(const char *string, int rightX, int y, uint16_t color, uint8_t size = 1);
void card(int x, int y, int w, int h, uint16_t fill, uint16_t border);

// Standard chrome: title bar with the app/screen name plus radio and battery
// status on the right.
void statusBar(const char *title);
constexpr int STATUS_BAR_HEIGHT = 22;

// The Solana brand gradient (purple -> green), precomputed into a table so the
// per-column brand rules, progress bars and offer banners can index it instead
// of recomputing theme::gradient565() - which does float math and a colour
// conversion - for every column on every frame. Returns the colour at position
// numerator/denominator, clamped into range. Built lazily and once.
constexpr int GRADIENT_LUT_SIZE = 320;
uint16_t gradient565Lut(int numerator, int denominator);

}  // namespace display
