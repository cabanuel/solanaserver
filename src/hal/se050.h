/*
  NXP SE050 secure element - link presence only.

  Solana OS does not drive the applet; it performs the same T=1-over-I2C soft
  reset and ATR read the test kit uses, which is enough to tell an app whether
  the part is alive and to show its ATR in Settings. Anything beyond that wants
  a full T=1 stack and NXP's middleware, which is out of scope here.
*/
#pragma once

#include <Arduino.h>

#include "../config.h"

namespace se050 {

void begin();

// Soft-resets the part and reads its 35-byte ATR. Safe to call repeatedly.
bool test();

bool present();
const uint8_t *atr();
uint8_t atrLength();
uint8_t lastError();

// Lowercase hex of the ATR, empty when the part did not answer.
String atrHex();

// Fills `out` with `length` bytes from the SE050's own hardware RNG, using the
// T=1 transport in se050_t1.
//
// The contract that matters: a true return means every one of those bytes came
// out of the secure element. There is no fallback in here and there must never
// be one - quietly substituting another entropy source would make the function
// useless for the only reasons anyone would call it. On false the buffer is
// zeroed, so a caller that ignores the return value gets something obviously
// wrong rather than something plausible.
//
// Returns false if the part does not answer, the applet refuses (non-0x9000),
// or the response TLV does not parse or does not carry exactly `length` bytes.
//
// Costs one full T=1 exchange per 64 bytes - tens of milliseconds, bounded by
// se050_t1::BUDGET_MS - so callers should draw a block and buffer it rather
// than calling per byte. Only safe from the main loop.
//
// The GetRandom encoding is documented in AN12413 Table 271 and matches NXP's
// Se05x_API_GetRandom, but has not been observed against real silicon here; see
// the CONFIDENCE section in se050_t1.h before relying on it.
bool randomBytes(uint8_t *out, size_t length);

}  // namespace se050
