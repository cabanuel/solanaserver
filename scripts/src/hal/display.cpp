#include "display.h"

#include "../badge_log.h"
#include "../config.h"
#include "../net/espnow_mgr.h"
#include "../net/wifi_mgr.h"
#include "../ui/theme.h"
#include "power.h"

namespace display {
namespace {

class BadgePanel final : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

 public:
  BadgePanel() {
    {  // SPI bus
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = PIN_LCD_MISO;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {  // ILI9341
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = LCD_INVERT;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {  // Backlight
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 0;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

BadgePanel sPanel;
LGFX_Sprite sCanvas(&sPanel);
bool sCanvasReady = false;
bool sDirty = true;
uint8_t sBrightness = LCD_BRIGHTNESS;

// Precomputed brand-gradient ramp, indexed by column. Filled once - the entries
// are the same every frame, so the float interpolation in theme::gradient()
// only runs GRADIENT_LUT_SIZE times for the life of the process instead of once
// per column per frame.
uint16_t sGradientLut[GRADIENT_LUT_SIZE];
bool sGradientLutReady = false;

void buildGradientLut() {
  for (int i = 0; i < GRADIENT_LUT_SIZE; ++i) {
    sGradientLut[i] = theme::gradient565((float)i / (float)(GRADIENT_LUT_SIZE - 1));
  }
  sGradientLutReady = true;
}

}  // namespace

bool begin() {
  sPanel.init();
  sPanel.setRotation(LCD_ROTATION);
  sPanel.setBrightness(0);
  sPanel.fillScreen(theme::BLACK);

  // 320x240x16bpp is 150 KB. It only fits in PSRAM, and a badge whose PSRAM did
  // not come up cannot run the compositor at all - so say so loudly rather than
  // limping along with a half-broken UI.
  sCanvas.setPsram(true);
  sCanvas.setColorDepth(16);
  sCanvasReady = sCanvas.createSprite(sPanel.width(), sPanel.height()) != nullptr;
  if (!sCanvasReady) {
    badge_log::tagf("lcd", "FATAL: could not allocate %dx%d framebuffer (PSRAM missing?)",
                    (int)sPanel.width(), (int)sPanel.height());
    return false;
  }
  sCanvas.fillScreen(theme::BG);
  sCanvas.setTextWrap(false);
  buildGradientLut();
  badge_log::tagf("lcd", "ready %dx%d, %u KB framebuffer in PSRAM",
                  (int)sPanel.width(), (int)sPanel.height(),
                  (unsigned)(sPanel.width() * sPanel.height() * 2 / 1024));
  return true;
}

LGFX_Sprite &canvas() { return sCanvas; }
lgfx::LGFX_Device &panel() { return sPanel; }

int16_t width() { return sPanel.width(); }
int16_t height() { return sPanel.height(); }

void touch() { sDirty = true; }
void invalidate() { sDirty = true; }

bool flush() {
  if (!sCanvasReady || !sDirty) return false;
  sCanvas.pushSprite(0, 0);
  sDirty = false;
  return true;
}

void setBrightness(uint8_t value) {
  sBrightness = value;
  sPanel.setBrightness(value);
}

uint8_t brightness() { return sBrightness; }

void fadeTo(uint8_t target, uint16_t durationMs, void (*stepHook)()) {
  const int16_t from = (int16_t)sPanel.getBrightness();
  const int16_t span = (int16_t)target - from;
  constexpr uint8_t steps = 32;
  if (span == 0) {
    setBrightness(target);
    return;
  }
  for (uint8_t i = 0; i <= steps; ++i) {
    sPanel.setBrightness((uint8_t)(from + (span * i) / steps));
    const uint32_t stepMs = durationMs / steps;
    const uint32_t until = millis() + stepMs;
    do {
      if (stepHook) stepHook();
      delay(1);
    } while ((int32_t)(millis() - until) < 0);
  }
  setBrightness(target);
}

// ---------------------------------------------------------------------------

void text(const char *string, int x, int y, uint16_t color, uint8_t size) {
  sCanvas.setTextSize(size);
  sCanvas.setTextColor(color);
  sCanvas.setTextDatum(textdatum_t::top_left);
  sCanvas.drawString(string, x, y);
  sDirty = true;
}

void textCentered(const char *string, int centerX, int y, uint16_t color, uint8_t size) {
  sCanvas.setTextSize(size);
  sCanvas.setTextColor(color);
  sCanvas.setTextDatum(textdatum_t::top_center);
  sCanvas.drawString(string, centerX, y);
  sCanvas.setTextDatum(textdatum_t::top_left);
  sDirty = true;
}

void textRight(const char *string, int rightX, int y, uint16_t color, uint8_t size) {
  sCanvas.setTextSize(size);
  sCanvas.setTextColor(color);
  sCanvas.setTextDatum(textdatum_t::top_right);
  sCanvas.drawString(string, rightX, y);
  sCanvas.setTextDatum(textdatum_t::top_left);
  sDirty = true;
}

void card(int x, int y, int w, int h, uint16_t fill, uint16_t border) {
  sCanvas.fillRoundRect(x, y, w, h, 6, fill);
  sCanvas.drawRoundRect(x, y, w, h, 6, border);
  sDirty = true;
}

uint16_t gradient565Lut(int numerator, int denominator) {
  if (!sGradientLutReady) buildGradientLut();
  if (denominator <= 0) return sGradientLut[0];
  long idx = (long)numerator * (GRADIENT_LUT_SIZE - 1) / denominator;
  if (idx < 0) idx = 0;
  if (idx >= GRADIENT_LUT_SIZE) idx = GRADIENT_LUT_SIZE - 1;
  return sGradientLut[idx];
}

void statusBar(const char *title) {
  sCanvas.fillRect(0, 0, width(), STATUS_BAR_HEIGHT, theme::HEADER);
  // A two-pixel brand rule under the bar; cheap, and it reads as Solana.
  for (int x = 0; x < width(); ++x) {
    sCanvas.drawFastVLine(x, STATUS_BAR_HEIGHT, 2, gradient565Lut(x, width()));
  }

  text(title, 8, 7, theme::WHITE, 1);

  // Right-hand cluster: battery percent, then the radios that are up.
  char right[48];
  char radios[24] = "";
  if (wifi_mgr::mode() != wifi_mgr::Mode::Off) {
    strcat(radios, wifi_mgr::connected() || wifi_mgr::mode() == wifi_mgr::Mode::AccessPoint
                       ? "WiFi "
                       : "wifi ");
  }
  if (espnow_mgr::enabled()) strcat(radios, "NOW ");
  snprintf(right, sizeof(right), "%s%d%%", radios, (int)power::percent());
  textRight(right, width() - 8, 7, theme::MUTED, 1);
  sDirty = true;
}

}  // namespace display
