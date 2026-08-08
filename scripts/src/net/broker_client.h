/*
  Client for the app-store broker - see broker/PROTOCOL.md for the wire format.

  The badge registers once with its Ed25519 identity, then polls for offers.
  An offer is a handful of Lua scripts someone pointed at this badge's ID from
  the web App Store. Nothing is downloaded and nothing is written to flash until
  the wearer accepts it on the device: update() only ever *fetches the offer*,
  and the shell is what turns that into a prompt.

  Everything here is driven from the main loop, so no call may block for long.
  Polling is on a timer (BROKER_POLL_MS) rather than a long-poll or a background
  task, because a 25-second held request would stall the UI and a second task
  would need a lock around app_store and LittleFS. Six seconds of latency on
  "someone sent you a script" is not worth either of those.

  The install itself is the one long operation - it fetches and writes one
  script per update() tick rather than all of them in one call, so the progress
  bar keeps moving and the button scan keeps running.
*/
#pragma once

#include <Arduino.h>

#include "../config.h"

namespace broker {

enum class State : uint8_t {
  Off,          // disabled in Settings
  NoNetwork,    // enabled, waiting for Wi-Fi
  NoIdentity,   // enabled, but identity::ready() is false
  Registering,  // challenge/register in flight
  Idle,         // registered, polling
  Offered,      // an offer is waiting for the wearer
  Installing,   // the wearer accepted; scripts are being written
  Error,        // last exchange failed; retries with backoff
};

// One script inside an offer.
struct ScriptInfo {
  String name;         // sanitised app id, [a-z0-9._-]
  String file;         // original filename, for display
  uint32_t bytes = 0;
  String sha256;       // 64 lowercase hex, verified before install
  String description;  // first `--` line of the script, may be empty
};

// What the shell draws on the prompt screen.
struct Offer {
  String id;
  String repo;    // "owner/name"
  String ref;     // branch or tag
  String url;
  String sender;
  uint8_t count = 0;
  ScriptInfo scripts[BROKER_MAX_SCRIPTS];
};

void begin();

// One tick. Cheap when there is nothing to do; at most one HTTP request per
// call, never more than one script written per call.
void update();

// -- Configuration (persisted through settings::) ----------------------------
bool enabled();
void setEnabled(bool value);
String url();                       // e.g. "http://192.168.1.20:8787"
void setUrl(const String &value);

// Drops the stored token and re-registers on the next tick. Use after changing
// the broker URL or regenerating the identity.
void forget();

// -- Status ------------------------------------------------------------------
State state();
const char *stateText();  // short, for the Settings row: "idle", "offline", ...
bool registered();
String lastError();       // empty when the last exchange succeeded
uint32_t lastContactMs(); // millis() of the last successful exchange, 0 = never

// -- The pending offer -------------------------------------------------------
// True from the moment an offer arrives until it is accepted, declined, or
// dropped. The shell watches this to raise its prompt screen.
bool hasOffer();
const Offer &offer();

// Both are safe to call only while hasOffer(). accept() starts the install and
// returns immediately - watch installing() and installProgress().
void accept();
void decline();

bool installing();
uint8_t installProgress();  // 0-100
uint8_t installedCount();
uint8_t failedCount();

// Set when an install finishes: "3 installed" or "2 installed, 1 failed".
// Cleared when the wearer dismisses the result screen.
String lastResult();
void clearResult();

}  // namespace broker
