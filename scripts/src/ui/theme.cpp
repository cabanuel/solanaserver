#include "theme.h"

namespace theme {

void gradient(float t, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  constexpr uint8_t r0 = (BRAND_PURPLE_RGB >> 16) & 0xFF;
  constexpr uint8_t g0 = (BRAND_PURPLE_RGB >> 8) & 0xFF;
  constexpr uint8_t b0 = BRAND_PURPLE_RGB & 0xFF;
  constexpr uint8_t r1 = (BRAND_GREEN_RGB >> 16) & 0xFF;
  constexpr uint8_t g1 = (BRAND_GREEN_RGB >> 8) & 0xFF;
  constexpr uint8_t b1 = BRAND_GREEN_RGB & 0xFF;

  r = (uint8_t)(r0 + (int)((r1 - r0) * t));
  g = (uint8_t)(g0 + (int)((g1 - g0) * t));
  b = (uint8_t)(b0 + (int)((b1 - b0) * t));
}

uint16_t gradient565(float t) {
  uint8_t r, g, b;
  gradient(t, r, g, b);
  return rgb565(r, g, b);
}

}  // namespace theme
