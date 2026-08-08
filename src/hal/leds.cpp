#include "leds.h"

#include <math.h>

#include "../badge_log.h"
#include "../ui/theme.h"

namespace leds {
namespace {

struct Rgb {
  uint8_t r, g, b;
};

Rgb sPixels[RGB_LED_COUNT] = {{0, 0, 0}, {0, 0, 0}};
bool sReady = false;
// Latched once rmtInit() fails, so playBoot/pulse/playIdle - each of which calls
// begin() - do not re-run the (failing) init and re-log it on every animation,
// which used to flood the log ring on a board with a dead RMT channel.
bool sInitFailed = false;
uint8_t sBrightness = LED_DEFAULT_BRIGHTNESS;

enum class Anim : uint8_t { None, Boot, Pulse, Idle };
Anim sAnim = Anim::None;
uint32_t sAnimStart = 0;
uint32_t sAnimDuration = 0;

// Whether the badge is meant to be idling, and when that idle began.
//
// A pulse is a transient laid over whatever the LEDs were doing, so when it
// ends the badge has to go back to idling rather than to black. Without this a
// single app-store notification - or a delete confirmation - left the LEDs dark
// for the rest of the session, since a finished animation used to hand back to
// off() unconditionally. The start time is kept separately so resuming picks
// the hue cycle up where it would have been, instead of restarting it.
bool sIdleActive = false;
uint32_t sIdleStart = 0;
Rgb sPulseColor = {0, 0, 0};
uint32_t sLastFrameAt = 0;

// ~60 fps. Each show() holds interrupts off for about 60us, so there is no
// value in going faster and some cost in trying.
constexpr uint32_t FRAME_MS = 16;

inline uint8_t scale(uint8_t value) {
  return (uint8_t)((uint16_t)value * sBrightness / 255);
}

// WS2812B bit timings, in RMT ticks. The channel runs at 10 MHz, so one tick is
// 100ns. Datasheet is T0H 400ns / T1H 800ns with a 1.25us period, each +/-150ns;
// these are the values Arduino's own neopixelWrite uses.
constexpr uint32_t RMT_RESOLUTION_HZ = 10000000;
constexpr uint16_t T0H_TICKS = 4;   // 400ns high for a 0
constexpr uint16_t T0L_TICKS = 8;   // 800ns low
constexpr uint16_t T1H_TICKS = 8;   // 800ns high for a 1
constexpr uint16_t T1L_TICKS = 4;   // 400ns low

// One RMT symbol per bit, GRB order, MSB first.
rmt_data_t sSymbols[RGB_LED_COUNT * 24];

void encodeByte(uint8_t value, rmt_data_t *out) {
  for (uint8_t i = 0; i < 8; ++i) {
    const bool one = value & (0x80 >> i);
    out[i].level0 = 1;
    out[i].duration0 = one ? T1H_TICKS : T0H_TICKS;
    out[i].level1 = 0;
    out[i].duration1 = one ? T1L_TICKS : T0L_TICKS;
  }
}

// Smoothstep, so the boot ramps ease in and out instead of moving linearly.
inline float ease(float t) {
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

void setGradient(uint8_t index, float position, float intensity) {
  uint8_t r, g, b;
  theme::gradient(position, r, g, b);
  set(index, (uint8_t)(r * intensity), (uint8_t)(g * intensity), (uint8_t)(b * intensity));
}

// ---------------------------------------------------------------------------
// Boot animation timeline, in milliseconds from the start.
//
// With only two LEDs there is no room for anything spatial, so the sequence
// leans on colour and timing instead: a wipe that reads as the Solana gradient
// being drawn, a travelling wave that keeps the two out of phase, then a
// heartbeat in brand green to land on.
// ---------------------------------------------------------------------------
constexpr uint32_t BOOT_WIPE_MS  = 900;   // purple fills in, then runs to green
constexpr uint32_t BOOT_WAVE_MS  = 1900;  // gradient wave, LEDs half a cycle apart
constexpr uint32_t BOOT_BEAT_MS  = 1000;  // three green pulses
constexpr uint32_t BOOT_FADE_MS  = 400;   // out
constexpr uint32_t BOOT_TOTAL_MS = BOOT_WIPE_MS + BOOT_WAVE_MS + BOOT_BEAT_MS + BOOT_FADE_MS;

void renderBoot(uint32_t elapsed) {
  if (elapsed < BOOT_WIPE_MS) {
    const float t = (float)elapsed / BOOT_WIPE_MS;
    // First half: both LEDs rise in purple. Second half: they run up the
    // gradient to green together.
    if (t < 0.5f) {
      const float intensity = ease(t * 2.0f);
      setGradient(0, 0.0f, intensity);
      setGradient(1, 0.0f, intensity);
    } else {
      const float position = ease((t - 0.5f) * 2.0f);
      setGradient(0, position, 1.0f);
      setGradient(1, position, 1.0f);
    }
    return;
  }
  elapsed -= BOOT_WIPE_MS;

  if (elapsed < BOOT_WAVE_MS) {
    // Two and a half cycles of the gradient, the LEDs half a period apart so
    // the pair looks like a wave passing through rather than one colour.
    const float phase = (float)elapsed / BOOT_WAVE_MS * 2.5f;
    const float a = 0.5f + 0.5f * sinf(phase * TWO_PI);
    const float b = 0.5f + 0.5f * sinf(phase * TWO_PI + PI);
    setGradient(0, a, 0.75f + 0.25f * a);
    setGradient(1, b, 0.75f + 0.25f * b);
    return;
  }
  elapsed -= BOOT_WAVE_MS;

  if (elapsed < BOOT_BEAT_MS) {
    // |sin| gives a beat that snaps up and decays, unlike a plain sine.
    const float beat = fabsf(sinf((float)elapsed / BOOT_BEAT_MS * 3.0f * PI));
    setGradient(0, 1.0f, beat);
    setGradient(1, 1.0f, beat);
    return;
  }
  elapsed -= BOOT_BEAT_MS;

  const float intensity = 1.0f - ease((float)elapsed / BOOT_FADE_MS);
  setGradient(0, 1.0f, intensity);
  setGradient(1, 1.0f, intensity);
}

void renderPulse(uint32_t elapsed) {
  const float remaining = 1.0f - (float)elapsed / (float)sAnimDuration;
  const float intensity = ease(remaining);
  setAll((uint8_t)(sPulseColor.r * intensity),
         (uint8_t)(sPulseColor.g * intensity),
         (uint8_t)(sPulseColor.b * intensity));
}

// Full-saturation hue wheel. The idle animation is the one place the badge is
// not being brand-consistent on purpose: at rest it should read as a colourful
// object on a lanyard, not as two purple dots.
void hueToRgb(float hue, float intensity, uint8_t &r, uint8_t &g, uint8_t &b) {
  hue -= floorf(hue);  // wrap into 0..1
  const float sector = hue * 6.0f;
  const float f = sector - floorf(sector);
  const float q = 1.0f - f;

  float rf = 0.0f, gf = 0.0f, bf = 0.0f;
  switch ((int)sector % 6) {
    case 0: rf = 1.0f; gf = f;     bf = 0.0f;  break;
    case 1: rf = q;    gf = 1.0f;  bf = 0.0f;  break;
    case 2: rf = 0.0f; gf = 1.0f;  bf = f;     break;
    case 3: rf = 0.0f; gf = q;     bf = 1.0f;  break;
    case 4: rf = f;    gf = 0.0f;  bf = 1.0f;  break;
    default: rf = 1.0f; gf = 0.0f; bf = q;     break;
  }
  r = (uint8_t)(rf * intensity * 255.0f);
  g = (uint8_t)(gf * intensity * 255.0f);
  b = (uint8_t)(bf * intensity * 255.0f);
}

// One fade in and back out. Long enough that it reads as breathing rather than
// blinking - at 6s a full breath is slower than a resting human one.
constexpr uint32_t IDLE_BREATH_MS = 6000;
// A full trip around the wheel. Deliberately much longer than one breath, so
// consecutive breaths are only slightly different colours and the change is
// something you notice on the second look.
constexpr uint32_t IDLE_CYCLE_MS = 90000;
// The bottom of the fade. Not zero: a badge that goes fully dark reads as a
// badge that crashed, and the whole point of the idle animation is to say the
// opposite. Low enough to still land as a fade to off.
constexpr float IDLE_FLOOR = 0.04f;
// Hue gap between the two LEDs. Small - they should look like one object lit
// from two points, not like two independent lights.
constexpr float IDLE_HUE_SPREAD = 0.06f;

void renderIdle(uint32_t elapsed) {
  // sin over half a period gives 0 -> 1 -> 0; ease() on top makes it linger at
  // both ends instead of moving fastest where it is brightest.
  const float breath = (float)(elapsed % IDLE_BREATH_MS) / (float)IDLE_BREATH_MS;
  const float envelope = IDLE_FLOOR + (1.0f - IDLE_FLOOR) * ease(sinf(breath * PI));

  const float hue = (float)(elapsed % IDLE_CYCLE_MS) / (float)IDLE_CYCLE_MS;

  uint8_t r, g, b;
  hueToRgb(hue, envelope, r, g, b);
  set(0, r, g, b);
  hueToRgb(hue + IDLE_HUE_SPREAD, envelope, r, g, b);
  set(1, r, g, b);
}

}  // namespace

void begin() {
  if (sReady || sInitFailed) return;
  pinMode(PIN_RGB, OUTPUT);
  digitalWrite(PIN_RGB, LOW);
  delay(2);

  // The strip is driven by the RMT peripheral rather than by a bit-banged loop.
  //
  // Bit-banging works on a board that is doing nothing else - it is what the
  // test kit does - but it cannot survive this firmware. A WS2812 frame is 60us
  // of 100ns-accurate pulses, and noInterrupts() on the S3 does not mask the
  // high-priority interrupts the Wi-Fi and BLE stacks run on. One of those
  // landing mid-frame stretches a pulse, and every bit after it is garbage: the
  // first LED's 24 bits often make it out intact while the second LED's do not,
  // which shows up as the far LED flickering while the near one looks fine.
  //
  // RMT clocks the whole frame out in hardware from a symbol buffer, so an
  // interrupt can delay when a frame starts but can no longer corrupt one.
  sReady = rmtInit(PIN_RGB, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, RMT_RESOLUTION_HZ);
  if (!sReady) {
    // Latch so the next animation does not retry and re-log; this line should
    // appear once, not on every pulse for the rest of the session.
    sInitFailed = true;
    badge_log::tagf("led", "RMT init failed on GPIO%d - LEDs disabled", PIN_RGB);
    return;
  }
  // The line must sit LOW between frames: that gap is what latches the data
  // into the strip. Set it rather than assume the driver's default.
  rmtSetEOT(PIN_RGB, 0);
  off();
}

void setBrightness(uint8_t value) {
  // Store only. The brightness slider on the LEDs settings screen calls this on
  // every auto-repeat tick while held, and pushing a full RMT frame per call
  // just to record a number is wasteful; the next regular show() (the idle
  // animation runs one every 16ms, or an explicit preview) applies it.
  sBrightness = value;
}

uint8_t brightness() { return sBrightness; }

void show() {
  if (!sReady) return;

  for (uint8_t i = 0; i < RGB_LED_COUNT; ++i) {
    rmt_data_t *pixel = &sSymbols[i * 24];
    encodeByte(scale(sPixels[i].g), pixel);       // WS2812B takes GRB,
    encodeByte(scale(sPixels[i].r), pixel + 8);   // not RGB
    encodeByte(scale(sPixels[i].b), pixel + 16);
  }

  // Blocking, so sSymbols cannot be rewritten while the peripheral is still
  // reading it. A frame is 60us; the caller is a 60fps animation with 16ms to
  // spend, so waiting costs nothing worth measuring. The wait is bounded rather
  // than RMT_WAIT_FOR_EVER so a wedged RMT channel drops a frame instead of
  // hanging the badge from inside a 16ms update() tick.
  constexpr uint32_t RMT_WRITE_TIMEOUT_MS = 10;
  rmtWrite(PIN_RGB, sSymbols, RGB_LED_COUNT * 24, RMT_WRITE_TIMEOUT_MS);
}

void set(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= RGB_LED_COUNT) return;
  sPixels[index] = {r, g, b};
}

void setAll(uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t i = 0; i < RGB_LED_COUNT; ++i) sPixels[i] = {r, g, b};
}

void off() {
  setAll(0, 0, 0);
  show();
}

void playBoot() {
  begin();
  sAnim = Anim::Boot;
  sIdleActive = false;  // the boot sequence ends in black on purpose
  sAnimStart = millis();
  sAnimDuration = BOOT_TOTAL_MS;
}

void pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t durationMs) {
  begin();
  sAnim = Anim::Pulse;
  sAnimStart = millis();
  sAnimDuration = durationMs ? durationMs : 1;
  sPulseColor = {r, g, b};
}

void playIdle() {
  begin();
  sAnim = Anim::Idle;
  sIdleActive = true;
  sIdleStart = millis();
  sAnimStart = sIdleStart;
  sAnimDuration = 0;  // runs until replaced
}

bool animating() {
  if (sAnim == Anim::None) return false;
  if (sAnim == Anim::Idle) return true;
  return (millis() - sAnimStart) < sAnimDuration;
}

// Hands the LEDs to whoever asked - a launching app, usually. That app's own
// pulses must not drag the shell's idle animation back on top of them, so the
// intent to idle is dropped here too; the shell re-arms it in onAppStopped().
void stopAnimation() {
  sAnim = Anim::None;
  sIdleActive = false;
}

void update() {
  if (sAnim == Anim::None || !sReady) return;

  const uint32_t now = millis();
  if (now - sLastFrameAt < FRAME_MS) return;
  sLastFrameAt = now;

  const uint32_t elapsed = now - sAnimStart;
  switch (sAnim) {
    case Anim::Boot:
      if (elapsed >= sAnimDuration) {
        sAnim = Anim::None;
        off();
        return;
      }
      renderBoot(elapsed);
      break;
    case Anim::Pulse:
      if (elapsed >= sAnimDuration) {
        if (sIdleActive) {
          // Resume, rather than restart: sAnimStart goes back to when the idle
          // began, so the hue is where it would have been had the pulse never
          // happened and the badge does not visibly jump colour.
          sAnim = Anim::Idle;
          sAnimStart = sIdleStart;
          renderIdle(now - sIdleStart);
          break;
        }
        sAnim = Anim::None;
        off();
        return;
      }
      renderPulse(elapsed);
      break;
    case Anim::Idle:
      renderIdle(elapsed);
      break;
    default:
      return;
  }
  show();
}

}  // namespace leds
