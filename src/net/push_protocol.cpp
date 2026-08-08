#include "push_protocol.h"

#include <mbedtls/base64.h>

#include "../apps/app_store.h"
#include "../badge_log.h"
#include "../config.h"
#include "../hal/leds.h"
#include "../lua_sdk/lua_runtime.h"
#include "../settings.h"
#include "../ui/theme.h"
#include "cert_store.h"
#include "wifi_mgr.h"

namespace push_protocol {
namespace {

bool sAuthorised = false;

// Brute-force throttling for AUTH. A 6-digit code is ~20 bits; over a 20-byte
// GATT link an attacker can still try thousands of codes a minute, so a lockout
// is what makes the code meaningful. Local constants because config.h is owned
// by another module (a follow-up could centralise them there).
constexpr int AUTH_FAIL_THRESHOLD = 5;
constexpr uint32_t AUTH_LOCK_MS = 30000;
constexpr uint32_t AUTH_LOCK_MAX_MS = 300000;
int sAuthFailCount = 0;
uint32_t sAuthLockUntil = 0;

// Constant-time comparison so the code cannot be recovered from reply timing.
bool constantTimeEquals(const String &a, const String &b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); ++i) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
  return diff == 0;
}

// In-flight transfer. A certificate transfer sets sTargetCert instead of
// sTargetApp/sTargetPath and is buffered whole, because cert_store::write()
// validates the PEM header and so needs the file in one piece.
String sTargetApp;
String sTargetPath;
String sTargetCert;
String sCertBuffer;
bool sTransferOpen = false;
size_t sTransferBytes = 0;

// Staged Wi-Fi profile, filled one SETWIFI line at a time and applied by
// JOINWIFI. Split that way because a WPA2-Enterprise profile has eight fields
// and no single line of a 20-byte-MTU protocol can carry them.
String sWifiSsid;
String sWifiPassword;
bool sWifiEnterprise = false;
wifi_mgr::Enterprise sWifiEap;

String nextToken(String &rest) {
  rest.trim();
  const int space = rest.indexOf(' ');
  if (space < 0) {
    const String token = rest;
    rest = "";
    return token;
  }
  const String token = rest.substring(0, space);
  rest = rest.substring(space + 1);
  return token;
}

bool requireAuth(const Reply &reply) {
  if (sAuthorised || !settings::pushRequiresPairing()) return true;
  reply("ERR not authorised - send AUTH <code>");
  return false;
}

void abortTransfer() {
  sTransferOpen = false;
  sTargetApp = "";
  sTargetPath = "";
  sTargetCert = "";
  sCertBuffer = "";
  sTransferBytes = 0;
}

}  // namespace

void reset() {
  sAuthorised = false;
  abortTransfer();

  // A staged profile holds a plaintext Wi-Fi password. Dropping it with the
  // session keeps it from sitting in RAM until the next transport connects.
  sWifiSsid = "";
  sWifiPassword = "";
  sWifiEnterprise = false;
  sWifiEap = wifi_mgr::Enterprise();
}

bool authorised() { return sAuthorised || !settings::pushRequiresPairing(); }

void handleLine(const String &line, const Reply &reply) {
  String rest = line;
  rest.trim();
  if (rest.length() == 0) return;

  String command = nextToken(rest);
  command.toUpperCase();

  // PING stays pre-auth: it reveals nothing but "something is listening".
  if (command == "PING") {
    reply("OK pong");
    return;
  }

  if (command == "AUTH") {
    const String code = nextToken(rest);
    if (!settings::pushRequiresPairing()) {
      sAuthorised = true;
      reply("OK pairing disabled, authed");
      return;
    }
    // Throttle: while locked out, refuse without even comparing, so an attacker
    // cannot keep the guess rate up. The lockout is module state and is NOT
    // cleared by reset(), so reconnecting does not wipe the counter.
    if ((int32_t)(millis() - sAuthLockUntil) < 0) {
      reply("ERR locked out - too many attempts");
      return;
    }
    if (constantTimeEquals(code, settings::pairingCode())) {
      sAuthorised = true;
      sAuthFailCount = 0;
      badge_log::tagf("push", "session authorised");
      reply("OK authed");
    } else {
      // A wrong code invalidates whatever the session had, so a guessed code
      // cannot be used to escalate an already-open session.
      sAuthorised = false;
      if (++sAuthFailCount >= AUTH_FAIL_THRESHOLD) {
        uint32_t window = AUTH_LOCK_MS * (uint32_t)(sAuthFailCount - AUTH_FAIL_THRESHOLD + 1);
        if (window > AUTH_LOCK_MAX_MS) window = AUTH_LOCK_MAX_MS;
        sAuthLockUntil = millis() + window;
        badge_log::tagf("push", "pairing lockout: %d failures, blocked for %us", sAuthFailCount,
                        (unsigned)(window / 1000));
      } else {
        badge_log::tagf("push", "bad pairing code");
      }
      reply("ERR bad code");
    }
    return;
  }

  // Everything else needs auth. INFO and ECHO used to sit above this gate, but
  // INFO leaks version/heap/auth-state and ECHO reflects attacker-controlled
  // data to an unauthenticated peer, so both now live below it.
  if (!requireAuth(reply)) return;

  if (command == "INFO") {
    char text[128];
    snprintf(text, sizeof(text), "OK %s %s heap=%u fs_free=%u", SOLANA_OS_NAME, SOLANA_OS_VERSION,
             (unsigned)ESP.getFreeHeap(),
             (unsigned)(app_store::totalBytes() - app_store::usedBytes()));
    reply(String(text));
    return;
  }

  if (command == "ECHO") {
    reply("OK " + rest);
    return;
  }

  if (command == "LIST") {
    app_store::Info info;
    for (size_t i = 0; i < app_store::count(); ++i) {
      if (!app_store::at(i, info)) continue;
      reply("+ " + info.id + " " + String((unsigned)info.sizeBytes) + " " + info.name);
    }
    reply("OK " + String((unsigned)app_store::count()));
    return;
  }

  if (command == "BEGIN") {
    const String id = nextToken(rest);
    String path = nextToken(rest);
    if (path.length() == 0) path = APP_ENTRY;

    if (!app_store::isValidId(id)) {
      reply("ERR bad app id");
      return;
    }
    // Truncate now so a BEGIN with no DATA still leaves a well-defined result.
    if (!app_store::writeFile(id, path, nullptr, 0, false)) {
      reply("ERR cannot open " + path);
      return;
    }
    sTargetApp = id;
    sTargetPath = path;
    sTransferOpen = true;
    sTransferBytes = 0;
    reply("OK begin");
    return;
  }

  if (command == "CERTBEGIN") {
    const String name = nextToken(rest);
    if (!cert_store::isValidName(name)) {
      reply("ERR bad certificate name");
      return;
    }
    sTargetCert = name;
    sTargetApp = "";
    sTargetPath = "";
    sCertBuffer = "";
    sTransferOpen = true;
    sTransferBytes = 0;
    reply("OK begin");
    return;
  }

  if (command == "CERTS") {
    String name;
    for (size_t i = 0; i < cert_store::count(); ++i) {
      if (!cert_store::nameAt(i, name)) continue;
      reply("+ " + name + " " + String((unsigned)cert_store::sizeOf(name)));
    }
    reply("OK " + String((unsigned)cert_store::count()));
    return;
  }

  if (command == "CERTDEL") {
    const String name = nextToken(rest);
    if (!cert_store::exists(name)) {
      reply("ERR no such certificate");
      return;
    }
    reply(cert_store::remove(name) ? "OK deleted" : "ERR delete failed");
    return;
  }

  // -- Wi-Fi ---------------------------------------------------------------
  // SETWIFI takes the rest of the line verbatim as the value, so passwords
  // containing spaces survive.
  if (command == "SETWIFI") {
    String key = nextToken(rest);
    key.toLowerCase();
    const String value = rest;

    if (key == "ssid") sWifiSsid = value;
    else if (key == "pass") { sWifiPassword = value; sWifiEnterprise = false; }
    else if (key == "security") sWifiEnterprise = (value == "eap" || value == "enterprise");
    else if (key == "method") { sWifiEap.method = wifi_mgr::eapFromString(value); sWifiEnterprise = true; }
    else if (key == "identity") sWifiEap.identity = value;
    else if (key == "user") { sWifiEap.username = value; sWifiEnterprise = true; }
    else if (key == "eappass") { sWifiEap.password = value; sWifiEnterprise = true; }
    else if (key == "ca") sWifiEap.caCertName = value;
    else if (key == "domain") sWifiEap.domain = value;
    else if (key == "phase2") sWifiEap.ttlsPhase2 = value;
    else {
      reply("ERR unknown field - ssid pass security method identity user eappass ca domain phase2");
      return;
    }
    reply("OK " + key);
    return;
  }

  if (command == "JOINWIFI") {
    if (sWifiSsid.length() == 0) {
      reply("ERR no ssid staged - send SETWIFI ssid <name>");
      return;
    }
    if (sWifiEnterprise) {
      if (!sWifiEap.valid()) {
        reply("ERR enterprise needs a username");
        return;
      }
      if (sWifiEap.caCertName.length() && !cert_store::exists(sWifiEap.caCertName)) {
        reply("ERR no such certificate - upload it with CERTBEGIN first");
        return;
      }
      // A JOINWIFI over the authenticated push link with no CA staged is a
      // deliberate operator action, so opt in explicitly rather than letting
      // wifi_mgr fail the (validated-by-default) enterprise path closed.
      sWifiEap.allowNoCa = (sWifiEap.caCertName.length() == 0);
      // Replied before associating: joining drops the current network, and on
      // the BLE transport that is also when the badge stops answering.
      reply("OK joining " + sWifiSsid);
      wifi_mgr::connectEnterprise(sWifiSsid, sWifiEap, true);
    } else {
      reply("OK joining " + sWifiSsid);
      wifi_mgr::connect(sWifiSsid, sWifiPassword, true);
    }
    return;
  }

  if (command == "WIFI") {
    char text[160];
    snprintf(text, sizeof(text), "OK %s ssid=%s ip=%s rssi=%d%s", wifi_mgr::statusText(),
             wifi_mgr::ssid().c_str(), wifi_mgr::ip().toString().c_str(), wifi_mgr::rssi(),
             wifi_mgr::usingEnterprise() ? " enterprise" : "");
    reply(String(text));
    return;
  }

  if (command == "DATA") {
    if (!sTransferOpen) {
      reply("ERR no transfer - send BEGIN or CERTBEGIN first");
      return;
    }
    const String encoded = rest;
    size_t decodedLen = 0;
    // 3 bytes out per 4 in, rounded up.
    const size_t capacity = (encoded.length() / 4 + 1) * 3 + 4;
    if (sTransferBytes + capacity > PUSH_MAX_FILE_BYTES) {
      abortTransfer();
      reply("ERR file too large");
      return;
    }
    uint8_t *buffer = (uint8_t *)malloc(capacity);
    if (buffer == nullptr) {
      // Tear the transfer down: leaving sTransferOpen set after a failed chunk
      // would strand a half-written file and let later DATA lines append to it.
      abortTransfer();
      reply("ERR out of memory");
      return;
    }
    const int result = mbedtls_base64_decode(buffer, capacity, &decodedLen,
                                             (const unsigned char *)encoded.c_str(),
                                             encoded.length());
    if (result != 0) {
      free(buffer);
      reply("ERR bad base64");
      return;
    }

    bool ok;
    if (sTargetCert.length()) {
      // Certificates are accumulated and written once at END, because
      // cert_store::write() checks the PEM header and cannot judge a fragment.
      if (sTransferBytes + decodedLen > cert_store::MAX_CERT_BYTES) {
        free(buffer);
        abortTransfer();
        reply("ERR certificate too large");
        return;
      }
      sCertBuffer.concat((const char *)buffer, decodedLen);
      ok = true;
    } else {
      ok = app_store::writeFile(sTargetApp, sTargetPath, buffer, decodedLen, true);
    }
    free(buffer);

    if (!ok) {
      abortTransfer();
      reply("ERR write failed");
      return;
    }
    sTransferBytes += decodedLen;
    reply("OK " + String((unsigned)decodedLen));
    return;
  }

  if (command == "END") {
    if (!sTransferOpen) {
      reply("ERR no transfer");
      return;
    }
    const size_t total = sTransferBytes;

    if (sTargetCert.length()) {
      const String name = sTargetCert;
      const bool ok =
          cert_store::write(name, (const uint8_t *)sCertBuffer.c_str(), sCertBuffer.length());
      abortTransfer();
      if (!ok) {
        reply("ERR not a PEM certificate");
        return;
      }
      leds::pulse(0x99, 0x45, 0xFF, 600);
      reply("OK " + String((unsigned)total));
      return;
    }

    badge_log::tagf("push", "wrote %s/%s (%u bytes)", sTargetApp.c_str(), sTargetPath.c_str(),
                    (unsigned)total);
    abortTransfer();
    app_store::refresh();
    leds::pulse(0x14, 0xF1, 0x95, 600);  // brand green: "landed"
    reply("OK " + String((unsigned)total));
    return;
  }

  if (command == "ABORT") {
    abortTransfer();
    reply("OK aborted");
    return;
  }

  if (command == "DEL") {
    const String id = nextToken(rest);
    if (!app_store::exists(id)) {
      reply("ERR no such app");
      return;
    }
    // Deleting the app that is currently running would pull its files out from
    // under the VM. Stopped immediately rather than requested, because a
    // deferred stop would not have happened yet by the time removeApp() runs -
    // and this path is always on the main loop with no Lua frame on the stack.
    if (runtime::currentApp() == id) runtime::stop();
    reply(app_store::removeApp(id) ? "OK deleted" : "ERR delete failed");
    return;
  }

  if (command == "RUN") {
    const String id = nextToken(rest);
    if (!app_store::exists(id)) {
      reply("ERR no such app");
      return;
    }
    runtime::requestLaunch(id);
    reply("OK launching");
    return;
  }

  if (command == "STOP") {
    runtime::requestStop();
    reply("OK stopped");
    return;
  }

  reply("ERR unknown command");
}

}  // namespace push_protocol
