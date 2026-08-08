/*
  Shared I2C bus helpers.

  The TCA9534 button expander and the SE050 secure element share one bus, and a
  wedged bus takes both down at once. The recovery path (clock out stuck slaves,
  re-init the peripheral) is lifted from the test kit, where it earned its keep.
*/
#pragma once

#include <Arduino.h>

namespace badge_i2c {

void begin();

bool writeReg(uint8_t address, uint8_t reg, uint8_t value);
bool readReg(uint8_t address, uint8_t reg, uint8_t *buffer, size_t length);

// True when both lines read high with the peripheral released - i.e. nothing is
// holding the bus down.
bool idle();

// The same question asked cheaply: digitalRead follows the pin whoever is
// driving it, so this reports a clamped bus without the Wire teardown and
// re-init that idle() costs. Use this on any path that runs per-transfer.
bool linesHigh();

// Clocks SCL until a stuck slave lets go of SDA, then re-inits the peripheral.
bool recover();

// Consecutive-failure bookkeeping. After enough failures in a row the bus is
// recovered automatically - but rate-limited, with exponential backoff, and
// eventually abandoned. Without those limits a device that simply is not there
// turns a 15ms poll loop into ~22 bus resets per second.
void noteFailure(uint8_t errorCode);
void noteSuccess();

// True once recovery has been abandoned. Callers should stop polling (or slow
// right down) rather than keep generating failures.
bool down();

// Clears the given-up state and attempts one recovery. Called from the boot
// path and from Settings > Device info.
void retry();

uint8_t lastError();
const char *errorText(uint8_t code);
const char *knownDeviceName(uint8_t address);

// Logs every responding address. Used at boot and by the serial 's' command.
void scan();

}  // namespace badge_i2c
