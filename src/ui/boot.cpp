#include "boot.h"

#include "../../splash_images.h"
#include "../badge_log.h"
#include "../config.h"
#include "../hal/display.h"
#include "../hal/leds.h"
#include "../settings.h"
#include "theme.h"

namespace boot {
namespace {

// Every blocking wait in the boot path runs through this so the LED animation
// keeps ticking. Passed to display::fadeTo() as its step hook, and used
// directly by hold().
void tick() { leds::update(); }

void hold(uint32_t ms) {
  const uint32_t until = millis() + ms;
  while ((int32_t)(millis() - until) < 0) {
    tick();
    delay(2);
  }
}

// Decode into an off-screen sprite at native size, then push it down-scaled
// with anti-aliasing. Decoding straight to the target with a fractional zoom
// drops whole pixel rows, which chews visible notches out of the thin strokes
// in both logos.
void drawArtwork(const uint8_t *png, uint32_t length, uint16_t w, uint16_t h) {
  auto &canvas = display::canvas();
  const float scale = (float)SPLASH_TARGET_WIDTH / (float)w;

  LGFX_Sprite sprite(&canvas);
  sprite.setPsram(true);
  sprite.setColorDepth(16);
  if (sprite.createSprite(w, h)) {
    sprite.fillScreen(theme::BLACK);
    sprite.drawPng(png, length, 0, 0);
    sprite.setPivot(w / 2.0f, h / 2.0f);
    sprite.pushRotateZoomWithAA(display::width() / 2.0f, display::height() / 2.0f, 0.0f, scale,
                               scale);
    sprite.deleteSprite();
  } else {
    // Not enough memory for the intermediate sprite: fall back to a direct
    // scaled decode and accept the notching.
    const int drawW = (int)(w * scale + 0.5f);
    const int drawH = (int)(h * scale + 0.5f);
    canvas.drawPng(png, length, (display::width() - drawW) / 2,
                   (display::height() - drawH) / 2, 0, 0, 0, 0, scale, scale);
  }
  display::touch();
}

void showSplash(const uint8_t *png, uint32_t length, uint16_t w, uint16_t h) {
  display::setBrightness(0);
  display::canvas().fillScreen(theme::BLACK);
  drawArtwork(png, length, w, h);
  display::invalidate();
  display::flush();

  // The artwork is white-on-black, so ramping the backlight gives a clean fade
  // without touching the framebuffer or needing per-pixel blending.
  display::fadeTo(settings::brightness(), SPLASH_FADE_MS, tick);
  hold(SPLASH_HOLD_MS);
  display::fadeTo(0, SPLASH_FADE_MS, tick);
}

}  // namespace

void progress(const char *step, const char *detail, uint8_t percent) {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);

  display::textCentered(SOLANA_OS_NAME, display::width() / 2, 52, theme::WHITE, 2);
  display::textCentered(SOLANA_OS_VERSION, display::width() / 2, 78, theme::MUTED, 1);
  display::textCentered(step, display::width() / 2, 108, theme::GREEN, 1);

  const int barX = 40;
  const int barY = 132;
  const int barW = display::width() - 80;
  constexpr int barH = 10;
  canvas.drawRoundRect(barX, barY, barW, barH, 4, theme::BORDER);

  // The fill is drawn as the brand gradient rather than a flat colour, so the
  // bar reads as Solana even before the logo is on screen.
  const int fill = (percent * (barW - 4)) / 100;
  for (int x = 0; x < fill; ++x) {
    canvas.drawFastVLine(barX + 2 + x, barY + 2, barH - 4,
                         display::gradient565Lut(x, barW - 4));
  }

  display::textCentered(detail, display::width() / 2, 156, theme::MUTED, 1);
  display::touch();
  display::flush();
  tick();
}

void run() {
  leds::playBoot();

  badge_log::tagf("boot", "splash 1/2: Solana");
  showSplash(SPLASH_SOLANA_PNG, sizeof(SPLASH_SOLANA_PNG), SPLASH_SOLANA_W, SPLASH_SOLANA_H);

  badge_log::tagf("boot", "splash 2/2: developed by SKYRIZZ");
  showSplash(SPLASH_SKYRIZZ_PNG, sizeof(SPLASH_SKYRIZZ_PNG), SPLASH_SKYRIZZ_W, SPLASH_SKYRIZZ_H);

  display::canvas().fillScreen(theme::BG);
  display::invalidate();
  display::flush();
  display::setBrightness(settings::brightness());

  // Let whatever is left of the LED sequence finish rather than cutting it off
  // mid-heartbeat, but never wait more than a second for it.
  const uint32_t until = millis() + 1000;
  while (leds::animating() && (int32_t)(millis() - until) < 0) {
    tick();
    delay(4);
  }
}

}  // namespace boot
