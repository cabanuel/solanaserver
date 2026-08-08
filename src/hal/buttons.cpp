#include "buttons.h"

#include "../badge_log.h"
#include "badge_i2c.h"

namespace buttons {
namespace {

// TCA9534 registers.
constexpr uint8_t REG_INPUT    = 0x00;
constexpr uint8_t REG_OUTPUT   = 0x01;
constexpr uint8_t REG_POLARITY = 0x02;
constexpr uint8_t REG_CONFIG   = 0x03;

// How often to re-probe an expander that is not answering, and how many
// consecutive failed reads of a working one before we go back to probing.
constexpr uint32_t PROBE_INTERVAL_MS = 3000;
constexpr uint8_t  READ_FAILURES_BEFORE_REPROBE = 20;

// The six wired pins. P6 and P7 are unconnected on this board, and a TCA9534
// input has no pull of its own, so those two bits float and can flip between
// any two reads. Everything downstream of the read must therefore work on the
// masked byte - see the note in update() for what happens when it does not.
constexpr uint8_t KEY_BITS = (uint8_t)((1U << BUTTON_COUNT) - 1);

bool sPresent = false;
bool sInterruptAttached = false;
uint32_t sNextProbeAt = 0;
uint8_t sReadFailures = 0;
volatile bool sInterruptFlag = false;

// Both hold the masked byte, never the raw one.
uint8_t sRawStable = KEY_BITS;
uint8_t sRawCandidate = KEY_BITS;
uint32_t sCandidateSince = 0;
uint32_t sLastPoll = 0;

uint8_t sDownMask = 0;
uint8_t sPressedMask = 0;
uint8_t sReleasedMask = 0;
uint8_t sRepeatMask = 0;

uint32_t sDownSince[BUTTON_COUNT] = {0};
uint32_t sNextRepeatAt[BUTTON_COUNT] = {0};

void IRAM_ATTR onExpanderInterrupt() { sInterruptFlag = true; }

uint8_t decode(uint8_t raw) {
  return BUTTON_ACTIVE_LOW ? (uint8_t)((~raw) & KEY_BITS) : (uint8_t)(raw & KEY_BITS);
}

}  // namespace

bool begin() {
  pinMode(PIN_BUTTON_INT, INPUT_PULLUP);
  if (retry()) return true;

  // One clocking-out before giving up on this attempt, the way the test kit
  // checks the bus before its own expander init. A badge that was reset in the
  // middle of a transfer comes up with a slave still holding SDA, and the first
  // probe is the one that meets it.
  if (!badge_i2c::linesHigh()) {
    badge_log::tagf("btn", "bus not idle at first probe - recovering");
    badge_i2c::recover();
    if (retry()) return true;
  }
  return false;
}

bool retry() {
  uint8_t config = 0;
  if (!badge_i2c::readReg(TCA9534_ADDR, REG_CONFIG, &config, 1)) {
    sPresent = false;
    sNextProbeAt = millis() + PROBE_INTERVAL_MS;
    return false;
  }

  // All six as inputs, read as they are, output latch clear. The polarity
  // register is the one that must be written: it is what decides whether a
  // pressed key reads as a 0 or a 1, and BUTTON_ACTIVE_LOW below assumes it is
  // clear. It powers up clear, but "powers up" is not the same as "is", and a
  // badge that reset without losing rail keeps whatever was in it.
  badge_i2c::writeReg(TCA9534_ADDR, REG_CONFIG, 0xFF);
  badge_i2c::writeReg(TCA9534_ADDR, REG_POLARITY, 0x00);
  badge_i2c::writeReg(TCA9534_ADDR, REG_OUTPUT, 0x00);

  uint8_t initial = 0xFF;
  if (badge_i2c::readReg(TCA9534_ADDR, REG_INPUT, &initial, 1)) {
    initial &= KEY_BITS;
    sRawStable = initial;
    sRawCandidate = initial;
    sDownMask = decode(initial);
  }
  sPresent = true;
  sReadFailures = 0;

  // Attached only once a probe has succeeded, and only once. The flag it sets
  // is an optimisation - the poll below runs on its own timer regardless - so
  // an expander that answers reads but never pulls INT still works.
  if (!sInterruptAttached) {
    attachInterrupt(digitalPinToInterrupt(PIN_BUTTON_INT), onExpanderInterrupt, FALLING);
    sInterruptAttached = true;
  }

  // The raw byte is the single most useful thing in the log when a key does
  // nothing: with no button held it should read 0x3F (all six pulled high).
  badge_log::tagf("btn", "TCA9534 @0x%02X ready, input=0x%02X", TCA9534_ADDR, initial);
  return true;
}

bool present() { return sPresent; }

void update() {
  // Edge masks are per-tick: clear them first so a consumer that skips a tick
  // cannot see a stale press.
  sPressedMask = 0;
  sReleasedMask = 0;
  sRepeatMask = 0;

  const uint32_t now = millis();

  if (!sPresent) {
    // Re-probe on a slow cadence, the way the test kit's retryMissingI2cDevices
    // does. Without this a single failed probe at boot - a bus still settling, a
    // badge reset mid-transfer - leaves the badge with no input at all for the
    // rest of the session, and the only way to ask for another probe is
    // Settings > Device info, which needs the buttons that are not working.
    if ((int32_t)(now - sNextProbeAt) >= 0) {
      sNextProbeAt = now + PROBE_INTERVAL_MS;
      retry();  // logs the raw input byte when it succeeds, silent when it does not
    }
  } else {
    // A bus that has been given up on is polled slower, so the expander - by far
    // the most frequent I2C caller - does not sit on a dead bus generating a
    // failure every 15ms. Slow, not stopped: this is the only input the badge
    // has, and a bus marked down that is actually fine must not cost the user
    // their buttons.
    const uint32_t interval = badge_i2c::down() ? BUTTON_POLL_DOWN_MS : BUTTON_POLL_MS;
    const bool due = (now - sLastPoll) >= interval;
    if (sInterruptFlag || due) {
      sInterruptFlag = false;
      sLastPoll = now;

      uint8_t raw = 0xFF;
      if (badge_i2c::readReg(TCA9534_ADDR, REG_INPUT, &raw, 1)) {
        sReadFailures = 0;
        // Mask before anything compares it. The debounce below only accepts a
        // new value once the byte has read back identical for BUTTON_DEBOUNCE_MS
        // - so a single floating bit anywhere in it resets the candidate on
        // every poll, the timer never expires, and not one key ever registers.
        // P6/P7 are that floating bit, and how much they wander depends on what
        // else the board is doing: quiet under the test kit, not necessarily so
        // with the radios up.
        raw &= KEY_BITS;
        if (raw != sRawCandidate) {
          sRawCandidate = raw;
          sCandidateSince = now;
        }
        if (raw != sRawStable && (now - sCandidateSince) >= BUTTON_DEBOUNCE_MS) {
          sRawStable = raw;
          const uint8_t next = decode(raw);
          sPressedMask = (uint8_t)(next & ~sDownMask);
          sReleasedMask = (uint8_t)(~next & sDownMask & KEY_BITS);
          sDownMask = next;

          // One line per debounced press. It costs nothing at rest and it is
          // the only way to answer the two questions a dead-key report raises:
          // is the press reaching the expander at all, and is P0..P5 mapped the
          // way config.h assumes? Read it with the silkscreen in hand.
          for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
            if (sPressedMask & (uint8_t)(1U << i)) {
              badge_log::tagf("btn", "P%u %s down (raw=0x%02X)", i, name(i), raw);
            }
          }
        }
      } else if (++sReadFailures >= READ_FAILURES_BEFORE_REPROBE) {
        // The part stopped answering. Go back to probing rather than reading a
        // register that is no longer there: a re-probe re-writes the config and
        // polarity registers, which is what a part that browned out or was
        // knocked off the bus actually needs.
        badge_log::tagf("btn", "TCA9534 stopped answering (error %u: %s) - re-probing",
                        badge_i2c::lastError(), badge_i2c::errorText(badge_i2c::lastError()));
        sPresent = false;
        sReadFailures = 0;
        sNextProbeAt = now + PROBE_INTERVAL_MS;
        // Release whatever was held, so a key that was down when the bus went
        // does not stay down forever - APP_ESCAPE_HOLD_MS on a stuck B would
        // force-quit the running app on a loop.
        sReleasedMask = sDownMask;
        sDownMask = 0;
        sRawStable = KEY_BITS;
        sRawCandidate = KEY_BITS;
      }
    }
  }

  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    const uint8_t bit = (uint8_t)(1U << i);
    if (sPressedMask & bit) {
      sDownSince[i] = now;
      sNextRepeatAt[i] = now + BUTTON_REPEAT_DELAY_MS;
    } else if (sReleasedMask & bit) {
      sDownSince[i] = 0;
    } else if ((sDownMask & bit) && sDownSince[i] &&
               (int32_t)(now - sNextRepeatAt[i]) >= 0) {
      sRepeatMask |= bit;
      sNextRepeatAt[i] = now + BUTTON_REPEAT_PERIOD_MS;
    }
  }
}

bool down(uint8_t key) { return key < BUTTON_COUNT && (sDownMask & (1U << key)); }
bool pressed(uint8_t key) { return key < BUTTON_COUNT && (sPressedMask & (1U << key)); }
bool released(uint8_t key) { return key < BUTTON_COUNT && (sReleasedMask & (1U << key)); }
bool repeated(uint8_t key) { return pressed(key) || (key < BUTTON_COUNT && (sRepeatMask & (1U << key))); }

uint8_t downMask() { return sDownMask; }
uint8_t pressedMask() { return sPressedMask; }

uint32_t heldMs(uint8_t key) {
  if (key >= BUTTON_COUNT || !down(key) || sDownSince[key] == 0) return 0;
  return millis() - sDownSince[key];
}

const char *name(uint8_t key) {
  switch (key) {
    case BTN_UP:    return "UP";
    case BTN_DOWN:  return "DOWN";
    case BTN_LEFT:  return "LEFT";
    case BTN_RIGHT: return "RIGHT";
    case BTN_A:     return "SELECT";
    case BTN_B:     return "CANCEL";
    default:        return "?";
  }
}

const char *shortName(uint8_t key) {
  switch (key) {
    case BTN_UP:    return "up";
    case BTN_DOWN:  return "down";
    case BTN_LEFT:  return "left";
    case BTN_RIGHT: return "right";
    case BTN_A:     return "a";
    case BTN_B:     return "b";
    default:        return "?";
  }
}

}  // namespace buttons
