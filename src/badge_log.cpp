#include "badge_log.h"

#include <stdarg.h>

// When CDC-on-boot is set, `Serial` is the USB port - USBCDC under TinyUSB,
// HWCDC under Hardware CDC/JTAG - and is a different object from UART0.
// Targeting `Serial` rather than `USBSerial` keeps the USB log working in BOTH
// USB modes; USBSerial only exists under TinyUSB.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  #define USB_LOG_AVAILABLE 1
#else
  #define USB_LOG_AVAILABLE 0
#endif

namespace badge_log {
namespace {

char sRing[RING_LINES][RING_LINE_LEN];
size_t sHead = 0;     // next slot to write
size_t sCount = 0;    // retained lines, saturating at RING_LINES
uint32_t sRevision = 0;

// Partial line being accumulated - write() takes arbitrary chunks, and Lua's
// print() in particular arrives as several writes ending in a lone "\n".
char sPending[RING_LINE_LEN];
size_t sPendingLen = 0;

// write() is called from the main loop AND from the BLE and ESP-NOW tasks
// (which run on either core), so every touch of the ring/pending state above
// has to be serialised. A portMUX spinlock is the right tool here: it is safe
// across cores and from ISR-adjacent task context, and the critical sections
// are a handful of instructions each. The slow Serial writes are kept OUTSIDE
// it (see write()); only the in-memory bookkeeping is guarded.
portMUX_TYPE sRingMux = portMUX_INITIALIZER_UNLOCKED;

// The largest content length a line may hold, leaving one byte for the NUL so a
// full line can never index sPending[RING_LINE_LEN]. Also bounds the memcpy into
// sRing[] at exactly RING_LINE_LEN.
constexpr size_t RING_LINE_MAX = RING_LINE_LEN - 1;

// Caller must already hold sRingMux.
void commitPending() {
  if (sPendingLen > RING_LINE_MAX) sPendingLen = RING_LINE_MAX;  // defensive clamp
  sPending[sPendingLen] = '\0';
  memcpy(sRing[sHead], sPending, sPendingLen + 1);
  sHead = (sHead + 1) % RING_LINES;
  if (sCount < RING_LINES) ++sCount;
  ++sRevision;
  sPendingLen = 0;
}

void ringPush(const char *text, size_t len) {
  portENTER_CRITICAL(&sRingMux);
  for (size_t i = 0; i < len; ++i) {
    const char c = text[i];
    if (c == '\n') {
      commitPending();
      continue;
    }
    if (c == '\r') continue;
    if (sPendingLen < RING_LINE_MAX) {
      sPending[sPendingLen++] = c;
    } else {
      // Overlong line: commit what we have and keep going on the next row
      // rather than silently dropping the tail.
      commitPending();
      sPending[sPendingLen++] = c;
    }
  }
  portEXIT_CRITICAL(&sRingMux);
}

}  // namespace

void begin(unsigned long baud) {
  Serial0.begin(baud);
#if USB_LOG_AVAILABLE
  Serial.begin(baud);
#endif
}

void waitForUsb(uint32_t timeoutMs) {
#if USB_LOG_AVAILABLE
  const uint32_t startedAt = millis();
  while (!Serial && millis() - startedAt < timeoutMs) delay(10);
#else
  (void)timeoutMs;
#endif
}

void write(const char *text, size_t len) {
  Serial0.write((const uint8_t *)text, len);
#if USB_LOG_AVAILABLE
  if (Serial) Serial.write((const uint8_t *)text, len);
#endif
  ringPush(text, len);
}

void print(const char *text) { write(text, strlen(text)); }

void println(const char *text) {
  write(text, strlen(text));
  write("\n", 1);
}

void printf(const char *format, ...) {
  char buffer[RING_LINE_LEN * 2];
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (written > 0) {
    write(buffer, (size_t)min((int)sizeof(buffer) - 1, written));
  }
}

void tagf(const char *tag, const char *format, ...) {
  char buffer[RING_LINE_LEN * 2];
  // snprintf returns the length it WOULD have written, which for a very long
  // tag exceeds the buffer. Clamp before doing pointer arithmetic with it, or
  // `buffer + prefix` runs off the end and `sizeof(buffer) - prefix` underflows
  // into a huge size_t.
  const int p = snprintf(buffer, sizeof(buffer), "[%s] ", tag);
  size_t prefix = (p < 0) ? 0 : (size_t)p;
  if (prefix > sizeof(buffer) - 1) prefix = sizeof(buffer) - 1;
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(buffer + prefix, sizeof(buffer) - prefix, format, args);
  va_end(args);
  size_t total = prefix + (written > 0 ? (size_t)written : 0);
  if (total > sizeof(buffer) - 2) total = sizeof(buffer) - 2;
  buffer[total++] = '\n';
  write(buffer, total);
}

size_t lineCount() { return sCount; }

const char *line(size_t index) {
  if (index >= sCount) return "";
  // sHead points one past the newest; the oldest retained line sits at
  // sHead - sCount, modulo the ring.
  const size_t slot = (sHead + RING_LINES - sCount + index) % RING_LINES;
  return sRing[slot];
}

uint32_t revision() { return sRevision; }

int readCommand() {
  if (Serial0.available()) return Serial0.read();
#if USB_LOG_AVAILABLE
  if (Serial && Serial.available()) return Serial.read();
#endif
  return -1;
}

}  // namespace badge_log
