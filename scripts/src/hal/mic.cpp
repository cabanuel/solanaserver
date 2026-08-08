#include "mic.h"

#include <ESP_I2S.h>
#include <math.h>

#include "../badge_log.h"
#include "../config.h"

namespace mic {
namespace {

I2SClass sI2S;
bool sEnabled = false;

int16_t sBuffer[512];
uint32_t sLastUpdate = 0;

float sLevelL = 0.0f, sLevelR = 0.0f;
float sDbL = -99.0f, sDbR = -99.0f;
// PDM mics carry a large DC offset that swamps the RMS if it is not removed.
// These track it with a slow leaky integrator.
float sDcL = 0.0f, sDcR = 0.0f;

float levelFromDb(float db) {
  return constrain(((db + 58.0f) / 58.0f) * 100.0f, 0.0f, 100.0f);
}

}  // namespace

bool enable() {
  if (sEnabled) return true;
  sI2S.setPinsPdmRx(PIN_MIC_CLK, PIN_MIC_DATA);
  sEnabled = sI2S.begin(I2S_MODE_PDM_RX, PDM_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_STEREO);
  if (sEnabled) {
    // Never block the main loop waiting for audio; a short read is fine.
    sI2S.setTimeout(4);
    badge_log::tagf("mic", "PDM stereo up at %u Hz", (unsigned)PDM_SAMPLE_RATE);
  } else {
    badge_log::tagf("mic", "PDM init failed");
  }
  return sEnabled;
}

void disable() {
  if (!sEnabled) return;
  sI2S.end();
  sEnabled = false;
  sLevelL = sLevelR = 0.0f;
  sDbL = sDbR = -99.0f;
  sDcL = sDcR = 0.0f;
}

bool enabled() { return sEnabled; }

size_t readSamples(int16_t *out, size_t maxSamples) {
  if (!sEnabled || out == nullptr || maxSamples == 0) return 0;
  const size_t bytes = sI2S.readBytes((char *)out, maxSamples * sizeof(int16_t));
  return bytes / sizeof(int16_t);
}

void update() {
  if (!sEnabled) return;
  const uint32_t now = millis();
  if (now - sLastUpdate < 25) return;
  sLastUpdate = now;

  const size_t samples = readSamples(sBuffer, sizeof(sBuffer) / sizeof(sBuffer[0]));
  if (samples < 4) return;

  int64_t sumL = 0, sumR = 0;
  size_t frames = 0;
  for (size_t i = 0; i + 1 < samples; i += 2) {
    const float first = sBuffer[i];
    const float second = sBuffer[i + 1];
    const float leftIn = MIC_SWAP_LR ? second : first;
    const float rightIn = MIC_SWAP_LR ? first : second;

    sDcL = sDcL * 0.995f + leftIn * 0.005f;
    sDcR = sDcR * 0.995f + rightIn * 0.005f;

    const int32_t l = (int32_t)(leftIn - sDcL);
    const int32_t r = (int32_t)(rightIn - sDcR);
    sumL += (int64_t)l * l;
    sumR += (int64_t)r * r;
    ++frames;
  }
  if (frames == 0) return;

  const float rmsL = sqrtf((float)sumL / frames);
  const float rmsR = sqrtf((float)sumR / frames);
  sDbL = 20.0f * log10f(fmaxf(rmsL, 1.0f) / 32768.0f);
  sDbR = 20.0f * log10f(fmaxf(rmsR, 1.0f) / 32768.0f);

  sLevelL = sLevelL * 0.68f + levelFromDb(sDbL) * 0.32f;
  sLevelR = sLevelR * 0.68f + levelFromDb(sDbR) * 0.32f;
}

float levelLeft() { return sLevelL; }
float levelRight() { return sLevelR; }
float dbLeft() { return sDbL; }
float dbRight() { return sDbR; }

}  // namespace mic
