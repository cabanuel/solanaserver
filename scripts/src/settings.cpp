#include "settings.h"

#include <Preferences.h>
#include <WiFi.h>

#include "badge_log.h"
#include "config.h"

namespace settings {
namespace {

Preferences sSys;
Preferences sKv;
bool sReady = false;

// NVS keys are capped at 15 characters, so these are abbreviated rather than
// spelled out.
constexpr char NS_SYS[] = "sysconf";
constexpr char NS_KV[] = "luakv";

String defaultDeviceName() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char name[20];
  snprintf(name, sizeof(name), "badge-%02X%02X", mac[4], mac[5]);
  return String(name);
}

String randomPairingCode() {
  char code[7];
  snprintf(code, sizeof(code), "%06lu", (unsigned long)(esp_random() % 1000000UL));
  return String(code);
}

}  // namespace

void begin() {
  if (sReady) return;
  sSys.begin(NS_SYS, false);
  sKv.begin(NS_KV, false);
  sReady = true;

  if (sSys.getString("pair", "").length() != 6) regeneratePairingCode();
  badge_log::tagf("cfg", "settings loaded, device name '%s'", deviceName().c_str());
}

uint8_t brightness() { return sSys.getUChar("bright", LCD_BRIGHTNESS); }
bool setBrightness(uint8_t value) {
  const bool ok = sSys.putUChar("bright", value) == sizeof(value);
  if (!ok) badge_log::tagf("cfg", "failed to persist brightness");
  return ok;
}

uint8_t ledBrightness() { return sSys.getUChar("ledbright", LED_DEFAULT_BRIGHTNESS); }
void setLedBrightness(uint8_t value) { sSys.putUChar("ledbright", value); }

String deviceName() {
  const String stored = sSys.getString("name", "");
  return stored.length() ? stored : defaultDeviceName();
}
void setDeviceName(const String &name) { sSys.putString("name", name); }

String wifiSsid() { return sSys.getString("ssid", ""); }
String wifiPassword() { return sSys.getString("pass", ""); }

bool setWifiCredentials(const String &ssid, const String &password) {
  // putString returns the byte count stored; for these fields that must equal
  // the source length or the credential did not persist. An empty password
  // (open network) legitimately stores 0 bytes, which matches length() == 0.
  const bool ssidOk = sSys.putString("ssid", ssid) == ssid.length();
  const bool passOk = sSys.putString("pass", password) == password.length();
  sSys.putBool("eap", false);
  const bool ok = ssidOk && passOk;
  if (!ok) badge_log::tagf("cfg", "failed to persist wifi credentials");
  return ok;
}

void forgetWifi() {
  sSys.remove("ssid");
  sSys.remove("pass");
  sSys.remove("eap");
  sSys.remove("eapmethod");
  sSys.remove("eapident");
  sSys.remove("eapuser");
  sSys.remove("eappass");
  sSys.remove("eapca");
  sSys.remove("eapdomain");
  sSys.remove("eapphase2");
}

bool wifiIsEnterprise() { return sSys.getBool("eap", false); }

wifi_mgr::Enterprise enterpriseConfig() {
  wifi_mgr::Enterprise config;
  config.method = wifi_mgr::eapFromString(sSys.getString("eapmethod", "peap"));
  config.identity = sSys.getString("eapident", "");
  config.username = sSys.getString("eapuser", "");
  config.password = sSys.getString("eappass", "");
  config.caCertName = sSys.getString("eapca", "");
  config.domain = sSys.getString("eapdomain", "");
  config.ttlsPhase2 = sSys.getString("eapphase2", "mschapv2");
  return config;
}

void setEnterpriseCredentials(const String &ssid, const wifi_mgr::Enterprise &config) {
  sSys.putString("ssid", ssid);
  sSys.putBool("eap", true);
  sSys.putString("eapmethod", wifi_mgr::eapToString(config.method));
  sSys.putString("eapident", config.identity);
  sSys.putString("eapuser", config.username);
  sSys.putString("eappass", config.password);
  sSys.putString("eapca", config.caCertName);
  sSys.putString("eapdomain", config.domain);
  sSys.putString("eapphase2", config.ttlsPhase2);
  // The PSK field would otherwise be offered by the UI alongside an enterprise
  // profile for the same SSID.
  sSys.remove("pass");
}

bool wifiAutoConnect() { return sSys.getBool("wifiauto", true); }
void setWifiAutoConnect(bool enabled) { sSys.putBool("wifiauto", enabled); }

bool bleEnabledAtBoot() { return sSys.getBool("bleboot", false); }
void setBleEnabledAtBoot(bool enabled) { sSys.putBool("bleboot", enabled); }

bool espnowEnabledAtBoot() { return sSys.getBool("nowboot", true); }
void setEspnowEnabledAtBoot(bool enabled) { sSys.putBool("nowboot", enabled); }

uint8_t espnowChannel() { return sSys.getUChar("nowchan", ESPNOW_DEFAULT_CHANNEL); }
void setEspnowChannel(uint8_t channel) {
  if (channel < 1 || channel > 13) return;
  sSys.putUChar("nowchan", channel);
}

String pairingCode() { return sSys.getString("pair", "000000"); }
void regeneratePairingCode() { sSys.putString("pair", randomPairingCode()); }

bool pushRequiresPairing() { return sSys.getBool("pushauth", true); }
void setPushRequiresPairing(bool required) { sSys.putBool("pushauth", required); }

bool brokerEnabled() { return sSys.getBool("brkon", true); }
void setBrokerEnabled(bool enabled) { sSys.putBool("brkon", enabled); }

String brokerUrl() { return sSys.getString("brkurl", DEFAULT_BROKER_URL); }
void setBrokerUrl(const String &url) {
  // Trailing slashes would double up against the "/api/v1/..." the client
  // appends, and a broker that 404s for that reason is miserable to diagnose.
  String trimmed = url;
  trimmed.trim();
  while (trimmed.endsWith("/")) trimmed.remove(trimmed.length() - 1);
  sSys.putString("brkurl", trimmed);
}

String brokerToken() { return sSys.getString("brktok", ""); }
void setBrokerToken(const String &token) {
  if (token.length()) {
    sSys.putString("brktok", token);
  } else {
    sSys.remove("brktok");
  }
}

String autostartApp() { return sSys.getString("autostart", ""); }
void setAutostartApp(const String &appId) { sSys.putString("autostart", appId); }

String kvGet(const String &key, const String &fallback) {
  if (key.length() == 0 || key.length() > 15) return fallback;
  return sKv.getString(key.c_str(), fallback);
}

bool kvSet(const String &key, const String &value) {
  if (key.length() == 0 || key.length() > 15) return false;
  const size_t written = sKv.putString(key.c_str(), value);
  if (value.length() > 0) {
    // putString returns the number of value bytes stored; a short count is a
    // failed write, which must not be reported as success.
    return written == value.length();
  }
  // An empty value legitimately writes 0 bytes, so the count cannot tell a
  // success from a failure. Confirm the key was actually created instead of
  // trusting the old `|| value.length() == 0`, which reported success even when
  // the underlying NVS write failed.
  return sKv.isKey(key.c_str());
}

bool kvRemove(const String &key) {
  if (key.length() == 0 || key.length() > 15) return false;
  return sKv.remove(key.c_str());
}

void kvClear() { sKv.clear(); }

}  // namespace settings
