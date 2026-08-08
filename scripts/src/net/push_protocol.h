/*
  The line-oriented app-push protocol, spoken over BLE (and over the USB serial
  console, which is handy when debugging without a radio).

  It exists because BLE has no REST: the Wi-Fi path gets a proper HTTP API in
  push_server.h, and this is the same set of operations reduced to text lines
  that fit in a 20-byte GATT notification.

  Commands (case-insensitive), one per line:

    PING                      -> OK pong
    INFO                      -> OK <name> <version> <free-heap> <fs-free>
    AUTH <code>               -> OK authed | ERR bad code
    LIST                      -> + <id> <bytes> <name>   (one per app)
                                 OK <count>
    BEGIN <id> <path>         -> OK begin      start (or truncate) a file
    DATA <base64>             -> OK <bytes>    append a decoded chunk
    END                       -> OK <total>    close the file
    ABORT                     -> OK aborted    discard the transfer
    DEL <id>                  -> OK deleted
    RUN <id>                  -> OK launching
    STOP                      -> OK stopped
    ECHO <text>               -> OK <text>

  Every command except PING, INFO and AUTH needs a successful AUTH first,
  unless pairing has been turned off in Settings. The session is authorised
  until reset() - i.e. until the transport disconnects.
*/
#pragma once

#include <Arduino.h>
#include <functional>

namespace push_protocol {

using Reply = std::function<void(const String &)>;

// Handles one line and emits zero or more reply lines through `reply`.
void handleLine(const String &line, const Reply &reply);

// Drops authorisation and any half-finished transfer. Call on disconnect.
void reset();

bool authorised();

}  // namespace push_protocol
