/*
  BLE transport - a Nordic UART Service (NUS), which every BLE terminal app and
  Web Bluetooth page already knows how to talk to.

    Service  6e400001-b5a3-f393-e0a9-e50e24dcca9e
    RX       6e400002-... write / write-no-response   host -> badge
    TX       6e400003-... notify                      badge -> host

  Traffic is newline-delimited text. Where a line goes depends on whether a Lua
  app has claimed the link:

    - no handler installed (i.e. the launcher is up) -> push_protocol, so a
      phone can install and launch apps over BLE
    - handler installed by a running app             -> straight to the app

  That rule is what keeps "push an app over BLE" working without an app being
  able to be silently talked over.
*/
#pragma once

#include <Arduino.h>
#include <functional>

namespace ble_mgr {

// Called with one complete line, newline stripped.
using LineHandler = std::function<void(const String &)>;

bool begin(const String &deviceName);
void end();
bool enabled();
bool connected();

void update();

// Splits into 20-byte notifications automatically; a newline is appended if the
// text does not already end with one.
bool send(const String &text);
bool sendLine(const String &text);

// Installs the app-level handler. Clearing it hands the link back to
// push_protocol.
void onLine(LineHandler handler);
void clearLineHandler();
bool hasLineHandler();

String address();

}  // namespace ble_mgr
