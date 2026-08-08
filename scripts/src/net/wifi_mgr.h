/*
  Wi-Fi.

  One thing worth knowing before touching this: ESP-NOW and Wi-Fi share a
  single radio and therefore a single channel. Joining an access point moves the
  station to that AP's channel, which drags ESP-NOW along with it - so two
  badges that are on different Wi-Fi networks cannot see each other over
  ESP-NOW. espnow_mgr is told about every channel change so it can report the
  channel it is actually on rather than the one that was configured.
*/
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace wifi_mgr {

enum class Mode : uint8_t { Off, Station, AccessPoint };

// EAP outer method. PEAP is what DEF CON (and most conference networks) use.
enum class Eap : uint8_t { Peap = 0, Ttls = 1, Tls = 2 };

// One WPA2-Enterprise profile.
//
// For DEF CON: method Peap, `username`/`password` from wifireg.defcon.org,
// `caCertName` the PEM uploaded to /certs, `domain` "wifireg.defcon.org".
//
// `identity` is the outer (anonymous) identity sent in the clear. DEF CON's
// published config uses the real username there, so leaving it empty copies
// `username` into it rather than sending nothing.
struct Enterprise {
  Eap method = Eap::Peap;
  String identity;      // outer identity; defaults to `username`
  String username;      // inner identity (PEAP/TTLS)
  String password;
  String caCertName;    // a name in cert_store; empty disables validation
  String domain;        // expected cert subject/SAN, e.g. "wifireg.defcon.org"
  // TTLS inner method. Ignored for PEAP, which is always MSCHAPv2 here.
  String ttlsPhase2 = "mschapv2";
  // Explicit opt-in to associate with no CA (server not validated). Off by
  // default: connectEnterprise() refuses an empty caCertName unless the caller
  // sets this, so an unvalidated join is always a deliberate decision at the
  // call site rather than a silent fallback. See connectEnterprise().
  bool allowNoCa = false;

  bool valid() const { return username.length() > 0 || method == Eap::Tls; }
};

void begin();
void update();

// Connects and, when `save` is set, stores the credentials for auto-connect.
// Returns immediately; poll connected() or watch status().
bool connect(const String &ssid, const String &password, bool save = true);

// WPA2-Enterprise. Same asynchronous contract as connect().
//
// Association will fail outright if `caCertName` names a certificate that is
// not installed - that is deliberate. Silently downgrading to an unvalidated
// connection because a file was missing is exactly the failure mode enterprise
// authentication exists to prevent, and DEF CON is the last place to do it.
bool connectEnterprise(const String &ssid, const Enterprise &config, bool save = true);

void disconnect();

// True when the current (or saved) network is WPA2-Enterprise.
bool usingEnterprise();

// Parses "peap" / "ttls" / "tls"; anything else returns Peap.
Eap eapFromString(const String &text);
const char *eapToString(Eap method);

// SoftAP named after settings::deviceName(). Always 192.168.4.1.
bool startAccessPoint(const String &password = String());

void stop();

Mode mode();
bool connected();
const char *statusText();

String ssid();
IPAddress ip();
int rssi();
String macAddress();
uint8_t channel();

// -- Scanning ----------------------------------------------------------------
// Asynchronous: kick it off, poll scanning(), then read the results.
bool startScan();
bool scanning();
int scanResultCount();
String scanSsid(int index);
int scanRssi(int index);
bool scanEncrypted(int index);
void clearScan();

}  // namespace wifi_mgr
