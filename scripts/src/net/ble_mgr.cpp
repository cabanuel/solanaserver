#include "ble_mgr.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include <utility>  // std::swap

#include "../badge_log.h"
#include "push_protocol.h"

namespace ble_mgr {
namespace {

constexpr char SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char RX_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char TX_UUID[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

// Default ATT MTU is 23, leaving 20 bytes of payload per notification. We ask
// for more but must not assume we got it.
constexpr size_t NOTIFY_CHUNK = 20;

BLEServer *sServer = nullptr;
BLECharacteristic *sTx = nullptr;
bool sEnabled = false;
volatile bool sConnected = false;

LineHandler sHandler;

// The GATT write callback runs on the BLE stack's task. Lines are queued here
// and dispatched from update() on the main loop, for the same reason ESP-NOW
// packets are.
constexpr size_t QUEUE_LEN = 8;
String sQueue[QUEUE_LEN];
volatile size_t sQueueHead = 0;
volatile size_t sQueueTail = 0;
portMUX_TYPE sQueueMux = portMUX_INITIALIZER_UNLOCKED;

// Partial line accumulated across writes.
String sPending;

void enqueue(const String &line) {
  // Copy (which mallocs) OUTSIDE the critical section. Taking the heap mutex
  // while interrupts are disabled by portENTER_CRITICAL can deadlock or abort,
  // so under the spinlock we only swap String buffers (a pointer exchange, no
  // allocation). `copy` then holds the slot's previous (empty) buffer and is
  // freed here, after the lock is released.
  String copy = line;
  bool room;
  portENTER_CRITICAL(&sQueueMux);
  const size_t next = (sQueueHead + 1) % QUEUE_LEN;
  room = (next != sQueueTail);
  if (room) {
    std::swap(sQueue[sQueueHead], copy);
    sQueueHead = next;
  }
  portEXIT_CRITICAL(&sQueueMux);
  if (!room) badge_log::tagf("ble", "rx queue full, line dropped");
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    (void)server;
    sConnected = true;
    badge_log::tagf("ble", "central connected");
  }
  void onDisconnect(BLEServer *server) override {
    sConnected = false;
    sPending = "";
    // Drop the push-protocol session with the link. Without this the next
    // central to connect inherits the previous one's authorisation (and any
    // half-finished transfer / staged Wi-Fi password), because sAuthorised is a
    // sticky global that otherwise survives disconnect.
    push_protocol::reset();
    badge_log::tagf("ble", "central disconnected");
    // Without this the badge stops being discoverable after the first client.
    if (server) server->startAdvertising();
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const uint8_t *data = characteristic->getData();
    const size_t length = characteristic->getLength();
    if (data == nullptr || length == 0) return;

    for (size_t i = 0; i < length; ++i) {
      const char c = (char)data[i];
      if (c == '\n') {
        enqueue(sPending);
        sPending = "";
      } else if (c != '\r') {
        // Bound the buffer: a peer that never sends a newline must not be able
        // to grow this without limit.
        if (sPending.length() < 4096) sPending += c;
      }
    }
  }
};

ServerCallbacks sServerCallbacks;
RxCallbacks sRxCallbacks;

}  // namespace

bool begin(const String &deviceName) {
  if (sEnabled) return true;

  BLEDevice::init(deviceName.c_str());
  BLEDevice::setMTU(185);  // request; the central decides what we actually get

  sServer = BLEDevice::createServer();
  if (sServer == nullptr) {
    badge_log::tagf("ble", "createServer failed");
    return false;
  }
  sServer->setCallbacks(&sServerCallbacks);

  BLEService *service = sServer->createService(SERVICE_UUID);

  // No explicit 2902 descriptor: this core builds BLE on NimBLE, which adds the
  // client-configuration descriptor itself for any characteristic that declares
  // NOTIFY. Adding one by hand is deprecated and will stop compiling.
  sTx = service->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);

  BLECharacteristic *rx = service->createCharacteristic(
      RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(&sRxCallbacks);

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);  // iOS connection-interval workaround
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  sEnabled = true;
  badge_log::tagf("ble", "advertising as '%s' (%s)", deviceName.c_str(),
                  BLEDevice::getAddress().toString().c_str());
  return true;
}

void end() {
  if (!sEnabled) return;
  BLEDevice::deinit(true);
  sServer = nullptr;
  sTx = nullptr;
  sEnabled = false;
  sConnected = false;
  sQueueHead = 0;
  sQueueTail = 0;
  sPending = "";
  badge_log::tagf("ble", "off");
}

bool enabled() { return sEnabled; }
bool connected() { return sEnabled && sConnected; }

void update() {
  if (!sEnabled) return;
  while (true) {
    String line;
    portENTER_CRITICAL(&sQueueMux);
    const bool empty = (sQueueTail == sQueueHead);
    if (!empty) {
      // Move the slot's buffer out by swapping (no alloc/free under the lock);
      // the slot is left empty and `line`'s old empty buffer goes into it. The
      // real free happens when `line` is destroyed at the end of the iteration,
      // outside the critical section.
      std::swap(line, sQueue[sQueueTail]);
      sQueueTail = (sQueueTail + 1) % QUEUE_LEN;
    }
    portEXIT_CRITICAL(&sQueueMux);
    if (empty) break;

    if (sHandler) {
      sHandler(line);
    } else {
      push_protocol::handleLine(line, [](const String &reply) { sendLine(reply); });
    }
  }
}

bool send(const String &text) {
  if (!connected() || sTx == nullptr) return false;
  const size_t length = text.length();
  for (size_t offset = 0; offset < length; offset += NOTIFY_CHUNK) {
    const size_t chunk = min(NOTIFY_CHUNK, length - offset);
    sTx->setValue((uint8_t *)(text.c_str() + offset), chunk);
    sTx->notify();
    delay(4);  // give the stack time to drain; without it long replies are lost
  }
  return true;
}

bool sendLine(const String &text) {
  return send(text.endsWith("\n") ? text : text + "\n");
}

void onLine(LineHandler handler) { sHandler = std::move(handler); }
void clearLineHandler() { sHandler = nullptr; }
bool hasLineHandler() { return (bool)sHandler; }

String address() {
  if (!sEnabled) return String();
  return String(BLEDevice::getAddress().toString().c_str());
}

}  // namespace ble_mgr
