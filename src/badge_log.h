/*
  Logging.

  Every line goes to UART0 (GPIO43/44), to the USB CDC port when one is
  enumerated, and into an in-memory ring buffer. The ring is what the on-screen
  console and GET /api/logs read, so a badge with no cable attached still has a
  usable log - which matters because Lua app errors are the main thing anyone
  will want to read, and they happen when the badge is on a lanyard.
*/
#pragma once

#include <Arduino.h>

namespace badge_log {

constexpr size_t RING_LINES = 64;
constexpr size_t RING_LINE_LEN = 120;

void begin(unsigned long baud);

// Bounded - a badge with nothing on USB must still boot.
void waitForUsb(uint32_t timeoutMs);

void write(const char *text, size_t len);
void print(const char *text);
void println(const char *text);
void printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

// A tagged line: "[tag] message". Tags make the on-screen console scannable.
void tagf(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));

// Ring buffer access, newest last. index 0 is the oldest retained line.
size_t lineCount();
const char *line(size_t index);
uint32_t revision();  // bumps on every appended line; cheap change detection

// Serial input for the debug console, -1 when nothing is waiting.
int readCommand();

}  // namespace badge_log
