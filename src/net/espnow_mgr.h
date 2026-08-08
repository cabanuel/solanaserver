/*
  ESP-NOW: connectionless badge-to-badge messaging, and the signal meter behind
  Settings -> ESP-NOW.

  Every badge broadcasts a small presence beacon carrying its name. Beacons from
  other badges populate the peer table with a name, an RSSI and a last-seen
  timestamp, which is what the radar screen draws and what badge.espnow.peers()
  returns. Application traffic is tagged differently and delivered to the
  running Lua app, so beacons never surface as app messages.
*/
#pragma once

#include <Arduino.h>
#include <functional>

#include "../config.h"

namespace espnow_mgr {

struct Peer {
  uint8_t mac[6];
  char name[24];
  int8_t rssi;
  uint32_t lastSeenMs;
  uint32_t packets;
};

// (mac, payload, length, rssi)
using ReceiveHandler = std::function<void(const uint8_t *, const uint8_t *, size_t, int8_t)>;

bool begin(uint8_t channel);
void end();
bool enabled();

void update();

// nullptr mac broadcasts. Payload is capped at ESPNOW_MAX_PAYLOAD.
bool send(const uint8_t *mac, const uint8_t *data, size_t length);
bool broadcast(const uint8_t *data, size_t length);

// -- Peer table --------------------------------------------------------------
size_t peerCount();
const Peer *peerAt(size_t index);
const Peer *peerByMac(const uint8_t *mac);
void clearPeers();

// -- Presence beacon ---------------------------------------------------------
void setBeaconEnabled(bool on);
bool beaconEnabled();

uint8_t channel();

// Called by wifi_mgr when joining an AP moves the radio to another channel.
void notifyChannelChanged(uint8_t channel);

// Application-payload handler. Replaces any previous one; the Lua runtime
// installs its own on app start and clears it on stop.
void onReceive(ReceiveHandler handler);
void clearReceiveHandler();

// "aa:bb:cc:dd:ee:ff"
String macToString(const uint8_t *mac);
bool macFromString(const String &text, uint8_t *out);

}  // namespace espnow_mgr
