/*
  Battery sense: a 2k2:2k2 divider on GPIO1.

  There is no charge-status line on the v1 board, so "charging" is inferred
  from the voltage sitting above what a Li-Po reaches on its own.
*/
#pragma once

#include <Arduino.h>

namespace power {

void begin();
void update();

float volts();
float percent();   // 0..100, from a Li-Po curve rather than a linear map
bool charging();   // inferred, see above
uint16_t raw();

}  // namespace power
