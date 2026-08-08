/*
  Persistent settings, backed by NVS.

  Two namespaces: "sysconf" for the values below, and "luakv" for the key/value
  store Lua apps get through badge.storage.kv. Keeping them apart means a
  misbehaving app cannot clobber the Wi-Fi credentials.
*/
#pragma once

#include <Arduino.h>

#include "net/wifi_mgr.h"

namespace settings {

void begin();

// -- Display -----------------------------------------------------------------
uint8_t brightness();
// Returns false if the value could not be persisted; callers may ignore it.
bool setBrightness(uint8_t value);

// -- LEDs --------------------------------------------------------------------
uint8_t ledBrightness();
void setLedBrightness(uint8_t value);

// -- Identity ----------------------------------------------------------------
// Shown on the ESP-NOW radar and advertised over BLE. Defaults to
// "badge-XXXX" from the low MAC bytes.
String deviceName();
void setDeviceName(const String &name);

// -- Wi-Fi -------------------------------------------------------------------
// One saved network, either WPA2-Personal or WPA2-Enterprise. Which one is
// stored is what wifiIsEnterprise() reports, and it decides how begin()
// auto-connects.
String wifiSsid();
String wifiPassword();
// Returns false if either field could not be persisted; callers may ignore it.
bool setWifiCredentials(const String &ssid, const String &password);
void forgetWifi();
bool wifiAutoConnect();
void setWifiAutoConnect(bool enabled);

bool wifiIsEnterprise();
wifi_mgr::Enterprise enterpriseConfig();
void setEnterpriseCredentials(const String &ssid, const wifi_mgr::Enterprise &config);

// -- Radios at boot ----------------------------------------------------------
bool bleEnabledAtBoot();
void setBleEnabledAtBoot(bool enabled);
bool espnowEnabledAtBoot();
void setEspnowEnabledAtBoot(bool enabled);
uint8_t espnowChannel();
void setEspnowChannel(uint8_t channel);

// -- App push ----------------------------------------------------------------
// Six digits, regenerated on demand. A pusher must present it as X-Badge-Token.
String pairingCode();
void regeneratePairingCode();
bool pushRequiresPairing();
void setPushRequiresPairing(bool required);

// -- App-store broker --------------------------------------------------------
// See broker/PROTOCOL.md. The URL gates everything: with none set the client
// never opens a socket, whatever brokerEnabled() says, so the default-on
// toggle costs nothing on a badge that was never pointed at a broker.
bool brokerEnabled();
void setBrokerEnabled(bool enabled);
String brokerUrl();
void setBrokerUrl(const String &url);
// The bearer token from registration. Kept so a reboot does not burn a new
// challenge/register round-trip; cleared by "Forget registration".
String brokerToken();
void setBrokerToken(const String &token);

// -- Autostart ---------------------------------------------------------------
// App id launched instead of the launcher at boot; empty for none.
String autostartApp();
void setAutostartApp(const String &appId);

// -- Lua key/value store -----------------------------------------------------
String kvGet(const String &key, const String &fallback = String());
bool kvSet(const String &key, const String &value);
bool kvRemove(const String &key);
void kvClear();

}  // namespace settings
