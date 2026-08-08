#include "power.h"

#include "../config.h"

namespace power {
namespace {

float sVolts = 0.0f;
uint16_t sRaw = 0;
uint32_t sLastReadAt = 0;

// A single-cell Li-Po's discharge curve is flat through the middle and steep at
// both ends, so a linear volts->percent map reads badly: it sits at "80%" for
// hours and then falls off a cliff. These breakpoints are the usual
// approximation and at least move at a believable rate.
struct Point {
  float volts;
  float percent;
};
constexpr Point CURVE[] = {
    {3.30f, 0.0f},  {3.60f, 10.0f}, {3.70f, 25.0f}, {3.75f, 40.0f},
    {3.85f, 60.0f}, {3.95f, 75.0f}, {4.05f, 88.0f}, {4.20f, 100.0f},
};
constexpr size_t CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

}  // namespace

void begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY, ADC_11db);
  update();
  // Seed the filter with the first real reading instead of easing up from zero.
  sLastReadAt = 0;
  update();
}

void update() {
  const uint32_t now = millis();
  if (sLastReadAt != 0 && (now - sLastReadAt) < BATTERY_POLL_MS) return;
  sLastReadAt = now;

  constexpr uint8_t SAMPLES = 12;
  uint32_t rawSum = 0;
  uint32_t mvSum = 0;
  for (uint8_t i = 0; i < SAMPLES; ++i) {
    rawSum += analogRead(PIN_BATTERY);
    mvSum += analogReadMilliVolts(PIN_BATTERY);
  }
  sRaw = (uint16_t)(rawSum / SAMPLES);

  const float adcVolts = (mvSum / (float)SAMPLES) / 1000.0f;
  const float measured = adcVolts * BATTERY_DIVIDER_RATIO;
  if (sVolts <= 0.1f) {
    sVolts = measured;
  } else {
    sVolts = sVolts * 0.75f + measured * 0.25f;
  }
}

float volts() { return sVolts; }
uint16_t raw() { return sRaw; }

float percent() {
  if (sVolts <= CURVE[0].volts) return 0.0f;
  if (sVolts >= CURVE[CURVE_LEN - 1].volts) return 100.0f;
  for (size_t i = 1; i < CURVE_LEN; ++i) {
    if (sVolts < CURVE[i].volts) {
      const Point &lo = CURVE[i - 1];
      const Point &hi = CURVE[i];
      const float t = (sVolts - lo.volts) / (hi.volts - lo.volts);
      return lo.percent + t * (hi.percent - lo.percent);
    }
  }
  return 100.0f;
}

bool charging() {
  // 4.25V is above a resting full cell; only the charger holds it there.
  return sVolts > 4.25f;
}

}  // namespace power
