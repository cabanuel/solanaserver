/*
  Stereo PDM microphones (2x SPH0641LM4H on one data line).

  Kept off by default: the I2S peripheral runs a DMA ring whether or not anyone
  is listening, and most apps never touch audio. badge.mic.enable(true) from Lua
  or the mic level meter in Settings brings it up.
*/
#pragma once

#include <Arduino.h>

namespace mic {

bool enable();
void disable();
bool enabled();

void update();

// Smoothed 0..100 meter levels.
float levelLeft();
float levelRight();

// Raw RMS in dBFS, roughly -58..0.
float dbLeft();
float dbRight();

// Interleaved L/R 16-bit frames straight from the DMA ring. Returns the number
// of int16 samples written, which may be less than requested.
size_t readSamples(int16_t *out, size_t maxSamples);

}  // namespace mic
