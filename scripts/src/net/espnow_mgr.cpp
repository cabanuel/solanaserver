#include "espnow_mgr.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "../badge_log.h"
#include "../settings.h"

namespace espnow_mgr {
namespace {

// Frame layout: a 4-byte magic, a type byte, then the payload. The magic keeps
// badge traffic from being confused with any other ESP-NOW device sharing the
// channel, and the type byte is what separates presence beacons from app data.
constexpr uint8_t MAGIC[4] = {'S', 'B', 'D', 'G'};
constexpr uint8_t TYPE_BEACON = 0x01;
constexpr uint8_t TYPE_APP = 0x02;
constexpr size_t HEADER_LEN = sizeof(MAGIC) + 1;

const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

bool sEnabled = false;
uint8_t sChannel = ESPNOW_DEFAULT_CHANNEL;
bool sBeacon = true;
uint32_t sLastBeaconAt = 0;

Peer sPeers[ESPNOW_MAX_PEERS];
size_t sPeerCount = 0;
// The peer table is written by touchPeer() on the Wi-Fi task (from the receive
// callback) and read/compacted by the main loop. Without this all of those
// race: sPeerCount can be observed mid-update, and the compaction in update()
// can move entries under a reader. A dedicated spinlock (separate from the
// packet-queue mux) serialises every access below. It is held only for the
// bounded array work - never across a UI/Lua callback.
portMUX_TYPE sPeerMux = portMUX_INITIALIZER_UNLOCKED;

ReceiveHandler sHandler;

// The receive callback runs on the Wi-Fi task, not the main loop. Touching the
// Lua state from there would be a data race, so app payloads are parked in this
// queue and drained by update() on the main loop.
struct QueuedPacket {
  uint8_t mac[6];
  uint8_t data[ESPNOW_MAX_PAYLOAD];
  uint16_t length;
  int8_t rssi;
};
constexpr size_t QUEUE_LEN = 8;
QueuedPacket sQueue[QUEUE_LEN];
volatile size_t sQueueHead = 0;  // next write
volatile size_t sQueueTail = 0;  // next read
portMUX_TYPE sQueueMux = portMUX_INITIALIZER_UNLOCKED;

int findPeer(const uint8_t *mac) {
  for (size_t i = 0; i < sPeerCount; ++i) {
    if (memcmp(sPeers[i].mac, mac, 6) == 0) return (int)i;
  }
  return -1;
}

// Must be called WITHOUT sPeerMux held; it takes the lock itself. Runs on the
// Wi-Fi task from the receive callback.
void touchPeer(const uint8_t *mac, int8_t rssi, const char *name) {
  portENTER_CRITICAL(&sPeerMux);
  int index = findPeer(mac);
  if (index < 0) {
    if (sPeerCount >= ESPNOW_MAX_PEERS) {
      // Table full: evict whichever peer has been quiet longest.
      size_t oldest = 0;
      for (size_t i = 1; i < sPeerCount; ++i) {
        if (sPeers[i].lastSeenMs < sPeers[oldest].lastSeenMs) oldest = i;
      }
      index = (int)oldest;
    } else {
      index = (int)sPeerCount++;
    }
    memset(&sPeers[index], 0, sizeof(Peer));
    memcpy(sPeers[index].mac, mac, 6);
    strcpy(sPeers[index].name, "?");
  }

  sPeers[index].rssi = rssi;
  sPeers[index].lastSeenMs = millis();
  sPeers[index].packets++;
  if (name && name[0]) {
    strncpy(sPeers[index].name, name, sizeof(sPeers[index].name) - 1);
    sPeers[index].name[sizeof(sPeers[index].name) - 1] = '\0';
  }
  portEXIT_CRITICAL(&sPeerMux);
}

// Unicast requires the peer to be registered with esp_now first. Broadcast is
// registered once at begin().
bool ensureRegistered(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;  // 0 = whatever channel the interface is on
  info.ifidx = WIFI_IF_STA;
  info.encrypt = false;
  return esp_now_add_peer(&info) == ESP_OK;
}

void onDataReceived(const esp_now_recv_info_t *info, const uint8_t *data, int length) {
  if (info == nullptr || data == nullptr) return;
  if (length < (int)HEADER_LEN) return;
  if (memcmp(data, MAGIC, sizeof(MAGIC)) != 0) return;

  const uint8_t type = data[sizeof(MAGIC)];
  const uint8_t *payload = data + HEADER_LEN;
  const size_t payloadLen = (size_t)length - HEADER_LEN;
  const int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;

  if (type == TYPE_BEACON) {
    char name[24] = {0};
    const size_t copy = payloadLen < sizeof(name) - 1 ? payloadLen : sizeof(name) - 1;
    memcpy(name, payload, copy);
    touchPeer(info->src_addr, rssi, name);
    return;
  }

  if (type != TYPE_APP) return;
  touchPeer(info->src_addr, rssi, nullptr);

  if (payloadLen > ESPNOW_MAX_PAYLOAD) return;

  portENTER_CRITICAL_ISR(&sQueueMux);
  const size_t next = (sQueueHead + 1) % QUEUE_LEN;
  if (next != sQueueTail) {  // drop rather than overwrite unread packets
    QueuedPacket &slot = sQueue[sQueueHead];
    memcpy(slot.mac, info->src_addr, 6);
    memcpy(slot.data, payload, payloadLen);
    slot.length = (uint16_t)payloadLen;
    slot.rssi = rssi;
    sQueueHead = next;
  }
  portEXIT_CRITICAL_ISR(&sQueueMux);
}

bool sendFramed(const uint8_t *mac, uint8_t type, const uint8_t *data, size_t length) {
  if (!sEnabled || length > ESPNOW_MAX_PAYLOAD) return false;

  uint8_t frame[HEADER_LEN + ESPNOW_MAX_PAYLOAD];
  memcpy(frame, MAGIC, sizeof(MAGIC));
  frame[sizeof(MAGIC)] = type;
  if (length && data) memcpy(frame + HEADER_LEN, data, length);

  const uint8_t *target = mac ? mac : BROADCAST_MAC;
  if (!ensureRegistered(target)) return false;
  return esp_now_send(target, frame, HEADER_LEN + length) == ESP_OK;
}

void sendBeacon() {
  const String name = settings::deviceName();
  sendFramed(nullptr, TYPE_BEACON, (const uint8_t *)name.c_str(), name.length());
}

}  // namespace

bool begin(uint8_t channel) {
  if (sEnabled) return true;

  // ESP-NOW rides on the Wi-Fi driver, so the station interface has to exist
  // even when the badge is not joined to anything.
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);

  // Only force the channel when we are not associated - an associated station
  // cannot leave its AP's channel, and trying produces a confusing failure.
  if (WiFi.status() != WL_CONNECTED) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  }
  sChannel = WiFi.channel() ? WiFi.channel() : channel;

  if (esp_now_init() != ESP_OK) {
    badge_log::tagf("now", "esp_now_init failed");
    return false;
  }
  esp_now_register_recv_cb(onDataReceived);

  esp_now_peer_info_t broadcast = {};
  memcpy(broadcast.peer_addr, BROADCAST_MAC, 6);
  broadcast.channel = 0;
  broadcast.ifidx = WIFI_IF_STA;
  broadcast.encrypt = false;
  esp_now_add_peer(&broadcast);

  sEnabled = true;
  sLastBeaconAt = 0;
  badge_log::tagf("now", "up on channel %u as '%s'", (unsigned)sChannel,
                  settings::deviceName().c_str());
  return true;
}

void end() {
  if (!sEnabled) return;
  esp_now_unregister_recv_cb();
  esp_now_deinit();
  sEnabled = false;
  sPeerCount = 0;
  sQueueHead = 0;
  sQueueTail = 0;
  badge_log::tagf("now", "off");
}

bool enabled() { return sEnabled; }
uint8_t channel() { return sChannel; }

void notifyChannelChanged(uint8_t channel) {
  sChannel = channel;
  if (sEnabled) {
    // Peers on the old channel are unreachable now, and their stale RSSI would
    // be misleading on the radar. Clear under the lock; log outside it, since
    // badge_log may allocate and must not run inside a critical section.
    portENTER_CRITICAL(&sPeerMux);
    sPeerCount = 0;
    portEXIT_CRITICAL(&sPeerMux);
    badge_log::tagf("now", "channel moved to %u, peer table cleared", (unsigned)channel);
  }
}

void update() {
  if (!sEnabled) return;

  const uint32_t now = millis();
  if (sBeacon && (now - sLastBeaconAt) >= ESPNOW_BEACON_MS) {
    sLastBeaconAt = now;
    sendBeacon();
  }

  // Age out peers that have gone quiet. Locked because touchPeer() on the Wi-Fi
  // task may be inserting/evicting concurrently; the work is bounded (at most
  // ESPNOW_MAX_PEERS entries) so the section stays short.
  portENTER_CRITICAL(&sPeerMux);
  for (size_t i = 0; i < sPeerCount;) {
    if ((now - sPeers[i].lastSeenMs) > ESPNOW_PEER_TIMEOUT_MS) {
      sPeers[i] = sPeers[--sPeerCount];
    } else {
      ++i;
    }
  }
  portEXIT_CRITICAL(&sPeerMux);

  // Drain the receive queue on the main loop, where touching Lua is safe.
  while (true) {
    QueuedPacket packet;
    portENTER_CRITICAL(&sQueueMux);
    const bool empty = (sQueueTail == sQueueHead);
    if (!empty) {
      packet = sQueue[sQueueTail];
      sQueueTail = (sQueueTail + 1) % QUEUE_LEN;
    }
    portEXIT_CRITICAL(&sQueueMux);
    if (empty) break;
    if (sHandler) sHandler(packet.mac, packet.data, packet.length, packet.rssi);
  }
}

bool send(const uint8_t *mac, const uint8_t *data, size_t length) {
  return sendFramed(mac, TYPE_APP, data, length);
}

bool broadcast(const uint8_t *data, size_t length) {
  return sendFramed(nullptr, TYPE_APP, data, length);
}

size_t peerCount() {
  portENTER_CRITICAL(&sPeerMux);
  const size_t n = sPeerCount;
  portEXIT_CRITICAL(&sPeerMux);
  return n;
}

// NOTE: the returned pointer aliases the shared table. Callers run on the main
// loop and consume it immediately; a concurrent touchPeer() on the Wi-Fi task
// could still overwrite that slot's contents afterwards (the array is static so
// the pointer never dangles). Copy out fields you need to keep. The bounds
// check itself is taken under the lock so it can never read past sPeerCount.
const Peer *peerAt(size_t index) {
  portENTER_CRITICAL(&sPeerMux);
  const Peer *p = index < sPeerCount ? &sPeers[index] : nullptr;
  portEXIT_CRITICAL(&sPeerMux);
  return p;
}

const Peer *peerByMac(const uint8_t *mac) {
  portENTER_CRITICAL(&sPeerMux);
  const int index = findPeer(mac);
  const Peer *p = index < 0 ? nullptr : &sPeers[index];
  portEXIT_CRITICAL(&sPeerMux);
  return p;
}

void clearPeers() {
  portENTER_CRITICAL(&sPeerMux);
  sPeerCount = 0;
  portEXIT_CRITICAL(&sPeerMux);
}

void setBeaconEnabled(bool on) { sBeacon = on; }
bool beaconEnabled() { return sBeacon; }

void onReceive(ReceiveHandler handler) { sHandler = std::move(handler); }
void clearReceiveHandler() { sHandler = nullptr; }

String macToString(const uint8_t *mac) {
  char text[18];
  snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  return String(text);
}

bool macFromString(const String &text, uint8_t *out) {
  unsigned values[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2],
             &values[3], &values[4], &values[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; ++i) {
    if (values[i] > 0xFF) return false;
    out[i] = (uint8_t)values[i];
  }
  return true;
}

}  // namespace espnow_mgr
