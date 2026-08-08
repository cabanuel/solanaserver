/*
  Solana OS palette.

  The brand colours are the source of truth; everything else is a neutral
  derived from them. Colours are RGB565 because that is what both the panel and
  every LovyanGFX call want - rgb565() does the conversion at compile time so
  the hex in the comments stays readable.
*/
#pragma once

#include <Arduino.h>

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

namespace theme {

// -- Solana brand ------------------------------------------------------------
constexpr uint32_t BRAND_PURPLE_RGB = 0x9945FF;
constexpr uint32_t BRAND_GREEN_RGB  = 0x14F195;
constexpr uint32_t BRAND_TEAL_RGB   = 0x00FFA3;
constexpr uint32_t BRAND_MAGENTA_RGB = 0xDC1FFF;

constexpr uint16_t PURPLE  = rgb565(0x99, 0x45, 0xFF);
constexpr uint16_t GREEN   = rgb565(0x14, 0xF1, 0x95);
constexpr uint16_t TEAL    = rgb565(0x00, 0xFF, 0xA3);
constexpr uint16_t MAGENTA = rgb565(0xDC, 0x1F, 0xFF);

// -- Surfaces ----------------------------------------------------------------
constexpr uint16_t BLACK   = rgb565(0x00, 0x00, 0x00);
constexpr uint16_t BG      = rgb565(0x0B, 0x0B, 0x12);
constexpr uint16_t HEADER  = rgb565(0x16, 0x12, 0x24);
constexpr uint16_t PANEL   = rgb565(0x1A, 0x1A, 0x26);
constexpr uint16_t PANEL_2 = rgb565(0x24, 0x24, 0x33);
constexpr uint16_t BORDER  = rgb565(0x3A, 0x3A, 0x4E);

// -- Text --------------------------------------------------------------------
constexpr uint16_t WHITE = rgb565(0xFF, 0xFF, 0xFF);
constexpr uint16_t TEXT  = rgb565(0xE8, 0xE8, 0xF0);
constexpr uint16_t MUTED = rgb565(0x93, 0x93, 0xA8);

// -- Status ------------------------------------------------------------------
constexpr uint16_t OK      = GREEN;
constexpr uint16_t WARN    = rgb565(0xFF, 0xB0, 0x20);
constexpr uint16_t ERR     = rgb565(0xFF, 0x45, 0x45);
constexpr uint16_t ACCENT  = PURPLE;

// Interpolates the Solana gradient, purple at 0.0 -> green at 1.0. Used for
// the LED boot animation and for any UI element that wants the brand ramp.
void gradient(float t, uint8_t &r, uint8_t &g, uint8_t &b);
uint16_t gradient565(float t);

}  // namespace theme
