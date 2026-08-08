#include "broker_client.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "mbedtls/sha256.h"

#include "../apps/app_store.h"
#include "../badge_log.h"
#include "../identity/identity.h"
#include "../settings.h"
#include "broker_ca.h"
#include "cert_store.h"
#include "wifi_mgr.h"

namespace broker {
namespace {

// -- Transport security ------------------------------------------------------
// Local constants because config.h is owned by another module; a follow-up
// could move these there.
//
// When the broker URL is https, its certificate is pinned against a CA uploaded
// to cert_store under this name - mirroring how wifi_mgr consumes a CA - rather
// than falling back to setInsecure(). An https broker with no pinned CA is
// refused, so "https" can no longer be decorative.
constexpr char BROKER_CA_CERT_NAME[] = "broker-ca";

// Plain-http brokers are unauthenticated: anyone on-path can rewrite both a
// script and the sha256 the badge checks it against, so the hash proves
// nothing. The conference use-case is a broker on the same LAN, so http is
// permitted, but ONLY behind this explicit opt-in and with a logged warning.
// Flip to false to require https+pinning.
//
// TODO(cross-module): true end-to-end authenticity needs a publisher Ed25519
// signature over each offer (and its script hashes), verified on the badge
// against a known publisher key. That closes the MITM hole regardless of
// transport and should be specified in broker/PROTOCOL.md. Transport pinning
// below only authenticates the broker, not the offer's author.
constexpr bool BROKER_ALLOW_INSECURE_HTTP = true;

// Validation of tokens that get concatenated into request lines (offer ids) or
// signed (nonces). Rejecting anything outside a safe set defeats header/request
// injection via decodeString()'s CR/LF unescaping.
bool isSafeToken(const String &s, size_t maxLen) {
  if (s.length() == 0 || s.length() > maxLen) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// Nonces are base64/hex-ish; allow a slightly wider set but still no whitespace
// or control characters, and bound the length before it is signed or echoed.
bool isSafeNonce(const String &s) {
  if (s.length() == 0 || s.length() > 256) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-' || c == '+' || c == '/' || c == '=';
    if (!ok) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

State sState = State::Off;

// Mirrored out of NVS at begin() and kept in step by the setters. update() runs
// every pass of the main loop and reads all three; going to Preferences for
// them a thousand times a second would mean an NVS lock and a fresh String
// allocation per tick for values that change about once a month.
bool sEnabled = false;
String sUrl;
String sToken;
String sNonce;        // held between the challenge tick and the register tick
String sLastError;
String sLastResult;
uint32_t sLastContactMs = 0;
uint32_t sNextRequestAt = 0;

Offer sOffer;
bool sHasOffer = false;

// The install walks the offer one script per update() tick. `sInstallStep`
// counts fetched scripts and, at count(), the accept POST - so it runs from 0
// to count inclusive, which is also what the progress bar divides by.
bool sInstalling = false;
uint8_t sInstallStep = 0;
uint8_t sInstalledCount = 0;
uint8_t sFailedCount = 0;
// Accumulated as JSON array bodies while the install runs, because the accept
// POST wants exactly this shape and building it here avoids a second set of
// per-script arrays that would only ever be re-serialised.
String sInstalledJson;
String sFailedJson;

void fail(const char *what, const String &detail) {
  sLastError = detail.length() ? String(what) + ": " + detail : String(what);
  sState = State::Error;
  sNextRequestAt = millis() + BROKER_RETRY_MS;
  badge_log::tagf("broker", "%s", sLastError.c_str());
}

// ---------------------------------------------------------------------------
// JSON
//
// The ESP32 Arduino core bundles no JSON library and this sketch has no library
// manifest, so pulling one in would mean asking every builder to install it for
// the sake of six fields. push_server.cpp already hand-rolls its parsing for
// the same reason; this goes a little further than that because offer bodies
// are nested and Lua descriptions are full of quotes and backslashes, which a
// naive indexOf() scan gets wrong.
//
// Everything below works on [start, end) spans into one String so an offer is
// never copied just to look inside it.
// ---------------------------------------------------------------------------

int skipSpace(const String &s, int at, int end) {
  while (at < end && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) ++at;
  return at;
}

// Index just past the JSON value starting at `at`. Strings honour escapes;
// objects and arrays count their own bracket only, which is sufficient because
// well-formed JSON nests, and any inner bracket of the other kind is balanced
// inside a value this walk skips over as a whole.
int endOfValue(const String &s, int at, int end) {
  if (at >= end) return end;
  const char first = s[at];

  if (first == '"') {
    for (int i = at + 1; i < end; ++i) {
      if (s[i] == '\\') {
        ++i;
        continue;
      }
      if (s[i] == '"') return i + 1;
    }
    return end;
  }

  if (first == '{' || first == '[') {
    const char open = first;
    const char close = first == '{' ? '}' : ']';
    int depth = 0;
    int i = at;
    while (i < end) {
      const char c = s[i];
      if (c == '"') {
        i = endOfValue(s, i, end);
        continue;
      }
      if (c == open) {
        ++depth;
      } else if (c == close) {
        ++i;
        if (--depth == 0) return i;
        continue;
      }
      ++i;
    }
    return end;
  }

  int i = at;
  while (i < end && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' &&
         s[i] != '\r' && s[i] != '\t') {
    ++i;
  }
  return i;
}

bool keyEquals(const String &s, int quoteAt, int pastQuote, const char *key) {
  const int length = pastQuote - quoteAt - 2;  // strip both quotes
  if (length < 0) return false;
  if ((int)strlen(key) != length) return false;
  for (int i = 0; i < length; ++i) {
    if (s[quoteAt + 1 + i] != key[i]) return false;
  }
  return true;
}

// Index of `key`'s value inside the object spanning [start, end), or -1.
//
// The span may be a whole object or one already narrowed to its members, so a
// caller holding a complete response body does not have to strip the outer
// braces before asking it a question.
int member(const String &s, int start, int end, const char *key) {
  int i = skipSpace(s, start, end);
  if (i < end && s[i] == '{') {
    const int close = endOfValue(s, i, end);
    end = close > i + 1 ? close - 1 : i + 1;
    i = skipSpace(s, i + 1, end);
  }
  while (i < end && s[i] == '"') {
    const int keyStart = i;
    const int keyEnd = endOfValue(s, i, end);
    const bool match = keyEquals(s, keyStart, keyEnd, key);

    i = skipSpace(s, keyEnd, end);
    if (i >= end || s[i] != ':') return -1;
    i = skipSpace(s, i + 1, end);
    if (match) return i;

    i = skipSpace(s, endOfValue(s, i, end), end);
    if (i < end && s[i] == ',') i = skipSpace(s, i + 1, end);
  }
  return -1;
}

// Narrows onto the inside of the object or array at `at`. False if it is not one.
bool inside(const String &s, int at, int end, int &innerStart, int &innerEnd) {
  if (at < 0 || at >= end || (s[at] != '{' && s[at] != '[')) return false;
  const int close = endOfValue(s, at, end);
  innerStart = at + 1;
  innerEnd = close > at + 1 ? close - 1 : at + 1;
  return true;
}

String decodeString(const String &s, int at, int end) {
  String out;
  if (at >= end || s[at] != '"') return out;
  const int stop = endOfValue(s, at, end) - 1;  // the closing quote
  out.reserve(stop - at);
  for (int i = at + 1; i < stop && i < end; ++i) {
    const char c = s[i];
    if (c != '\\') {
      out += c;
      continue;
    }
    if (++i >= stop) break;
    switch (s[i]) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        // Only the BMP-ASCII range is meaningful here (script descriptions land
        // on a 320px screen); anything else becomes '?' rather than mojibake.
        uint16_t code = 0;
        for (int digit = 0; digit < 4 && i + 1 < stop; ++digit) {
          const char h = s[++i];
          code = (uint16_t)(code << 4);
          if (h >= '0' && h <= '9') code |= (uint16_t)(h - '0');
          else if (h >= 'a' && h <= 'f') code |= (uint16_t)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') code |= (uint16_t)(h - 'A' + 10);
        }
        out += (code >= 0x20 && code < 0x7F) ? (char)code : '?';
        break;
      }
      default: out += s[i]; break;
    }
  }
  return out;
}

String stringMember(const String &s, int start, int end, const char *key) {
  const int at = member(s, start, end, key);
  if (at < 0) return String();
  return decodeString(s, at, end);
}

uint32_t numberMember(const String &s, int start, int end, const char *key) {
  const int at = member(s, start, end, key);
  if (at < 0) return 0;
  uint32_t value = 0;
  for (int i = at; i < end && s[i] >= '0' && s[i] <= '9'; ++i) {
    value = value * 10 + (uint32_t)(s[i] - '0');
  }
  return value;
}

// Walks an array. `cursor` starts at the array's inner start; each call yields
// the next element's span and advances past it.
bool nextElement(const String &s, int end, int &cursor, int &elementStart, int &elementEnd) {
  cursor = skipSpace(s, cursor, end);
  if (cursor >= end) return false;
  elementStart = cursor;
  elementEnd = endOfValue(s, cursor, end);
  if (elementEnd <= elementStart) return false;
  cursor = skipSpace(s, elementEnd, end);
  if (cursor < end && s[cursor] == ',') ++cursor;
  return true;
}

String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char escape[7];
          snprintf(escape, sizeof(escape), "\\u%04x", c);
          out += escape;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

// Prepares `http` for `fullUrl`, choosing the transport:
//   - https: pin against the cert_store CA named BROKER_CA_CERT_NAME. No silent
//            setInsecure() fallback; a missing CA is a hard error.
//   - http : allowed only when BROKER_ALLOW_INSECURE_HTTP is set.
// The passed-in client objects and `caHolder` MUST outlive every use of `http`
// (setCACert stores the pointer without copying), so they are owned by the
// caller's stack frame and the request completes synchronously before they die.
bool beginRequest(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &secure, String &caHolder,
                  const String &fullUrl, String &errOut) {
  http.setConnectTimeout(BROKER_HTTP_TIMEOUT_MS);
  http.setTimeout(BROKER_HTTP_TIMEOUT_MS);
  http.setReuse(false);

  if (fullUrl.startsWith("https://")) {
    // Prefer an operator-provisioned CA (e.g. a self-hosted broker); otherwise
    // fall back to the trust anchor compiled in for the default Let's Encrypt
    // broker (broker_ca.h), so a stock badge validates the default broker out of
    // the box. Still no setInsecure(): if neither is present this fails closed.
    caHolder = cert_store::read(BROKER_CA_CERT_NAME);
    if (caHolder.length() == 0) {
      caHolder = BROKER_DEFAULT_CA_PEM;
    }
    if (caHolder.length() == 0) {
      errOut = "https broker needs a pinned CA (upload cert 'broker-ca')";
      return false;
    }
    secure.setCACert(caHolder.c_str());
    if (!http.begin(secure, fullUrl)) {
      errOut = "bad url";
      return false;
    }
    return true;
  }

  if (fullUrl.startsWith("http://")) {
    if (!BROKER_ALLOW_INSECURE_HTTP) {
      errOut = "insecure http broker rejected";
      return false;
    }
    if (!http.begin(plain, fullUrl)) {
      errOut = "bad url";
      return false;
    }
    return true;
  }

  errOut = "unsupported url scheme";
  return false;
}

// `path` starts with '/'. Returns the HTTP status, or a negative HTTPClient
// error code. `out` gets the body on any status that carried one. See
// beginRequest() for how the transport is secured.
int request(const char *method, const String &path, const String &body, const char *contentType,
            bool authorised, String &out) {
  out = "";

  const String base = url();
  if (base.length() == 0) return -1;

  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  String caHolder;  // must outlive http (setCACert keeps the pointer)
  String err;
  if (!beginRequest(http, plain, secure, caHolder, base + path, err)) {
    badge_log::tagf("broker", "request setup failed: %s", err.c_str());
    return -1;
  }

  if (authorised && sToken.length()) http.addHeader("Authorization", "Bearer " + sToken);
  if (body.length()) http.addHeader("Content-Type", contentType);

  int status;
  if (strcmp(method, "POST") == 0) {
    status = http.POST((uint8_t *)body.c_str(), body.length());
  } else {
    status = http.GET();
  }

  if (status > 0) out = http.getString();
  http.end();
  return status;
}

// Pulls "error" out of a broker error body so the Settings screen can show the
// broker's own words rather than a bare status code.
String errorCode(const String &body, int status) {
  const String code = stringMember(body, 0, (int)body.length(), "error");
  if (code.length()) return code;
  return String("HTTP ") + status;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void requestChallenge() {
  sState = State::Registering;

  String body = "{\"pubkey\":\"" + identity::publicKeyBase58() + "\"}";
  String reply;
  const int status = request("POST", "/api/v1/badge/challenge", body, "application/json", false,
                             reply);
  if (status != 200) {
    fail("challenge refused", status > 0 ? errorCode(reply, status)
                                         : HTTPClient::errorToString(status));
    return;
  }

  sNonce = stringMember(reply, 0, (int)reply.length(), "nonce");
  if (sNonce.length() == 0) {
    fail("challenge refused", "no nonce in reply");
    return;
  }
  // Bound the nonce's length and charset before it is signed and echoed back in
  // the register body: a broker (or MITM) returning a megabyte of CRLF here
  // should not get it signed or injected into the next request.
  if (!isSafeNonce(sNonce)) {
    sNonce = "";
    fail("challenge refused", "malformed nonce");
    return;
  }
  // Deliberately no backoff: the nonce expires in 120s and the register call is
  // the very next tick, so there is nothing to wait for.
  sNextRequestAt = millis();
}

void submitRegistration() {
  sState = State::Registering;

  const String pubkey = identity::publicKeyBase58();
  // Exactly as broker/PROTOCOL.md specifies: ASCII, no trailing newline. A
  // stray newline here would produce a valid signature over the wrong bytes,
  // which fails as "bad_signature" and looks like a broken key.
  const String message = "solana-badge-register:" + pubkey + ":" + sNonce;
  const String signature = identity::signBase64(message);
  if (signature.length() == 0) {
    sNonce = "";
    fail("cannot register", "identity refused to sign");
    return;
  }

  String body = "{\"pubkey\":\"" + pubkey + "\",";
  body += "\"name\":\"" + jsonEscape(settings::deviceName()) + "\",";
  body += "\"firmware\":\"" SOLANA_OS_VERSION "\",";
  body += "\"nonce\":\"" + jsonEscape(sNonce) + "\",";
  body += "\"signature\":\"" + jsonEscape(signature) + "\"}";

  String reply;
  const int status = request("POST", "/api/v1/badge/register", body, "application/json", false,
                             reply);
  sNonce = "";
  if (status != 200) {
    fail("register refused", status > 0 ? errorCode(reply, status)
                                        : HTTPClient::errorToString(status));
    return;
  }

  const String token = stringMember(reply, 0, (int)reply.length(), "token");
  if (token.length() == 0) {
    fail("register refused", "no token in reply");
    return;
  }

  sToken = token;
  settings::setBrokerToken(token);
  sLastError = "";
  sLastContactMs = millis();
  sState = State::Idle;
  sNextRequestAt = millis();
  badge_log::tagf("broker", "registered as %s (%s)",
                  stringMember(reply, 0, (int)reply.length(), "badgeId").c_str(),
                  identity::sourceName());
}

// A 401 means the token this badge holds is not one the broker knows - it was
// rotated, or the broker's registry was wiped. Re-running challenge/register
// with the same keypair is the documented recovery, and it keeps the badge ID.
void dropToken() {
  sToken = "";
  settings::setBrokerToken("");
  sNextRequestAt = millis();
  sState = State::Registering;
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

void readOffer(const String &json, int start, int end) {
  sOffer = Offer();
  sOffer.id = stringMember(json, start, end, "id");
  // The id is concatenated into request paths (offers/<id>/scripts, .../accept,
  // .../decline). decodeString() turns \r and \n escapes into real CRLF, so an
  // unvalidated id is a request/header-injection vector. Reject anything but a
  // safe id by clearing it - poll() then treats the offer as empty.
  if (!isSafeToken(sOffer.id, 128)) {
    badge_log::tagf("broker", "rejecting offer with unsafe id");
    sOffer = Offer();
    return;
  }
  sOffer.repo = stringMember(json, start, end, "repo");
  sOffer.ref = stringMember(json, start, end, "ref");
  sOffer.url = stringMember(json, start, end, "url");
  sOffer.sender = stringMember(json, start, end, "sender");

  int scriptsStart, scriptsEnd;
  if (inside(json, member(json, start, end, "scripts"), end, scriptsStart, scriptsEnd)) {
    int cursor = scriptsStart;
    int itemStart, itemEnd;
    while (sOffer.count < BROKER_MAX_SCRIPTS &&
           nextElement(json, scriptsEnd, cursor, itemStart, itemEnd)) {
      int fieldStart, fieldEnd;
      if (!inside(json, itemStart, itemEnd, fieldStart, fieldEnd)) continue;
      ScriptInfo &script = sOffer.scripts[sOffer.count];
      script.name = stringMember(json, fieldStart, fieldEnd, "name");
      script.file = stringMember(json, fieldStart, fieldEnd, "file");
      script.bytes = numberMember(json, fieldStart, fieldEnd, "bytes");
      script.sha256 = stringMember(json, fieldStart, fieldEnd, "sha256");
      script.description = stringMember(json, fieldStart, fieldEnd, "description");
      if (script.name.length()) ++sOffer.count;
    }
  }
}

void poll() {
  String reply;
  const int status = request("GET", "/api/v1/badge/inbox", "", nullptr, true, reply);

  if (status == 401 || status == 403) {
    dropToken();
    return;
  }
  if (status != 200) {
    fail("inbox failed",
         status > 0 ? errorCode(reply, status) : HTTPClient::errorToString(status));
    return;
  }

  sLastError = "";
  sLastContactMs = millis();
  sState = State::Idle;
  sNextRequestAt = millis() + BROKER_POLL_MS;

  const int end = (int)reply.length();
  int offersStart, offersEnd;
  if (!inside(reply, member(reply, 0, end, "offers"), end, offersStart, offersEnd)) return;

  // Only offers[0]. The wearer answers one prompt at a time, and the broker
  // keeps the rest queued until the next poll.
  int cursor = offersStart;
  int itemStart, itemEnd;
  if (!nextElement(reply, offersEnd, cursor, itemStart, itemEnd)) return;

  int fieldStart, fieldEnd;
  if (!inside(reply, itemStart, itemEnd, fieldStart, fieldEnd)) return;

  readOffer(reply, fieldStart, fieldEnd);
  if (sOffer.id.length() == 0 || sOffer.count == 0) return;

  sHasOffer = true;
  sState = State::Offered;
  badge_log::tagf("broker", "offer %s from %s: %u script%s", sOffer.id.c_str(),
                  sOffer.repo.c_str(), (unsigned)sOffer.count, sOffer.count == 1 ? "" : "s");
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void noteFailure(const String &name, const char *reason) {
  ++sFailedCount;
  if (sFailedJson.length()) sFailedJson += ",";
  sFailedJson += "{\"name\":\"" + jsonEscape(name) + "\",\"reason\":\"" + jsonEscape(reason) + "\"}";
  badge_log::tagf("broker", "install '%s' failed: %s", name.c_str(), reason);
}

bool sha256Matches(const uint8_t *data, size_t length, const String &expected) {
  if (expected.length() != 64) return false;

  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  // 0 = SHA-256 rather than SHA-224.
  if (mbedtls_sha256_starts(&ctx, 0) != 0 || mbedtls_sha256_update(&ctx, data, length) != 0 ||
      mbedtls_sha256_finish(&ctx, digest) != 0) {
    mbedtls_sha256_free(&ctx);
    return false;
  }
  mbedtls_sha256_free(&ctx);

  char hex[65];
  for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);

  String actual(hex);
  String want(expected);
  want.toLowerCase();
  return actual == want;
}

// Fetches one script body into a caller-freed buffer.
//
// Scripts run to 96 KB and internal heap with Wi-Fi up is not reliably that
// roomy, so the buffer comes from PSRAM. Reading straight off the stream into
// one exact-sized allocation also avoids the doubling that String
// concatenation - or HTTPClient::getString() on a chunked reply - would cost.
bool fetchScript(const String &name, uint8_t **dataOut, size_t *lengthOut, String &reasonOut) {
  *dataOut = nullptr;
  *lengthOut = 0;

  // sOffer.id is validated in readOffer() and `name` in installOne(), so both
  // are [A-Za-z0-9._-] and safe to place in the request line.
  const String path = "/api/v1/badge/offers/" + sOffer.id + "/scripts/" + name;

  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  String caHolder;  // must outlive http (setCACert keeps the pointer)
  String err;
  if (!beginRequest(http, plain, secure, caHolder, url() + path, err)) {
    reasonOut = err;
    return false;
  }
  if (sToken.length()) http.addHeader("Authorization", "Bearer " + sToken);

  const int status = http.GET();
  if (status != 200) {
    reasonOut = status > 0 ? errorCode(http.getString(), status)
                           : HTTPClient::errorToString(status);
    http.end();
    return false;
  }

  const int declared = http.getSize();
  if (declared > (int)BROKER_MAX_SCRIPT_BYTES) {
    reasonOut = "too large";
    http.end();
    return false;
  }
  const size_t capacity = declared > 0 ? (size_t)declared : BROKER_MAX_SCRIPT_BYTES;

  uint8_t *buffer = (uint8_t *)ps_malloc(capacity);
  // A badge whose PSRAM did not train still has internal heap; try it rather
  // than refusing the install outright.
  if (buffer == nullptr && capacity < ESP.getFreeHeap() / 2) {
    buffer = (uint8_t *)malloc(capacity);
  }
  if (buffer == nullptr) {
    reasonOut = "out of memory";
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t got = 0;
  const uint32_t deadline = millis() + BROKER_HTTP_TIMEOUT_MS;
  while (got < capacity && (int32_t)(millis() - deadline) < 0) {
    if (!http.connected() && stream->available() == 0) break;
    const int available = stream->available();
    if (available <= 0) {
      delay(1);
      continue;
    }
    const int read = stream->readBytes(buffer + got, min((size_t)available, capacity - got));
    if (read <= 0) break;
    got += (size_t)read;
    if (declared > 0 && got >= (size_t)declared) break;
  }
  http.end();

  if (got == 0 || (declared > 0 && got != (size_t)declared)) {
    free(buffer);
    reasonOut = "truncated download";
    return false;
  }

  *dataOut = buffer;
  *lengthOut = got;
  return true;
}

void installOne(const ScriptInfo &script) {
  if (!app_store::isValidId(script.name)) {
    noteFailure(script.name, "invalid app id");
    return;
  }

  uint8_t *data = nullptr;
  size_t length = 0;
  String reason;
  if (!fetchScript(script.name, &data, &length, reason)) {
    noteFailure(script.name, reason.c_str());
    return;
  }

  // Verified before a byte reaches flash. The offer's hash is what the sender
  // saw in the App Store page, so a mismatch means the bytes changed somewhere
  // between there and here - which is exactly the case worth refusing.
  //
  // CAVEAT: over plaintext http the hash and the script arrive on the SAME
  // unauthenticated channel, so an on-path attacker rewrites both and this check
  // passes. It only protects against accidental corruption there. Real
  // authenticity needs transport pinning (https+CA, see beginRequest) AND,
  // ideally, a publisher Ed25519 signature over the offer (TODO, cross-module).
  if (!sha256Matches(data, length, script.sha256)) {
    free(data);
    noteFailure(script.name, "sha256 mismatch");
    return;
  }

  const bool written = app_store::writeFile(script.name, APP_ENTRY, data, length, false);
  free(data);
  if (!written) {
    noteFailure(script.name, "write failed");
    return;
  }

  app_store::Info info;
  info.id = script.name;
  info.name = script.name;
  info.description = script.description;
  info.author = sOffer.repo;
  info.entry = APP_ENTRY;
  info.sizeBytes = length;
  app_store::writeManifest(info);

  ++sInstalledCount;
  if (sInstalledJson.length()) sInstalledJson += ",";
  sInstalledJson += "\"" + jsonEscape(script.name) + "\"";
  badge_log::tagf("broker", "installed '%s' (%u bytes)", script.name.c_str(), (unsigned)length);
}

void finishInstall() {
  String body = "{\"installed\":[" + sInstalledJson + "],\"failed\":[" + sFailedJson + "]}";
  String reply;
  const int status = request("POST", "/api/v1/badge/offers/" + sOffer.id + "/accept", body,
                             "application/json", true, reply);
  if (status == 401 || status == 403) dropToken();

  // The result is what actually landed on flash, not what the broker made of
  // the report - a broker that has gone away must not turn a good install into
  // a failure message on the wearer's screen.
  if (sFailedCount == 0) {
    sLastResult = String(sInstalledCount) + " installed";
  } else if (sInstalledCount == 0) {
    sLastResult = String(sFailedCount) + " failed";
  } else {
    sLastResult = String(sInstalledCount) + " installed, " + String(sFailedCount) + " failed";
  }

  app_store::refresh();

  sInstalling = false;
  sOffer = Offer();
  sInstalledJson = "";
  sFailedJson = "";
  sState = State::Idle;
  sLastContactMs = millis();
  sNextRequestAt = millis() + BROKER_POLL_MS;
  badge_log::tagf("broker", "install done: %s", sLastResult.c_str());
}

void installStep() {
  if (sInstallStep < sOffer.count) {
    installOne(sOffer.scripts[sInstallStep]);
    ++sInstallStep;
    return;
  }
  finishInstall();
}

}  // namespace

// ---------------------------------------------------------------------------

void begin() {
  sEnabled = settings::brokerEnabled();
  sUrl = settings::brokerUrl();
  sToken = settings::brokerToken();
  sState = sEnabled ? State::NoNetwork : State::Off;
  sNextRequestAt = millis();
  badge_log::tagf("broker", "%s, %s", sEnabled ? "enabled" : "disabled",
                  sUrl.length() ? sUrl.c_str() : "no address set");
  if (sUrl.startsWith("http://")) {
    badge_log::tagf("broker", "WARNING: broker over plaintext http - offers are unauthenticated, "
                              "the sha256 check cannot detect an on-path rewrite");
  }
}

void update() {
  if (!enabled()) {
    sState = State::Off;
    return;
  }
  if (url().length() == 0) {
    // Nothing to talk to. Reported as Off rather than Error because an unset
    // address is a configuration state, not a failure to retry.
    sState = State::Off;
    return;
  }
  if (!wifi_mgr::connected()) {
    sState = State::NoNetwork;
    return;
  }
  if (!identity::ready()) {
    sState = State::NoIdentity;
    return;
  }

  // An install owns the client until it finishes; polling in the middle would
  // fetch a second offer nobody can answer yet.
  if (sInstalling) {
    installStep();
    return;
  }
  // Likewise while the prompt is on screen.
  if (sHasOffer) return;

  if ((int32_t)(millis() - sNextRequestAt) < 0) return;

  if (sToken.length() == 0) {
    if (sNonce.length() == 0) {
      requestChallenge();
    } else {
      submitRegistration();
    }
    return;
  }

  poll();
}

bool enabled() { return sEnabled; }

void setEnabled(bool value) {
  sEnabled = value;
  settings::setBrokerEnabled(value);
  sLastError = "";
  sNextRequestAt = millis();
  sState = value ? State::NoNetwork : State::Off;
}

String url() { return sUrl; }

void setUrl(const String &value) {
  settings::setBrokerUrl(value);
  // Read back rather than stored directly: settings normalises the trailing
  // slash, and the two copies drifting apart would be a nasty little bug.
  const String normalised = settings::brokerUrl();
  if (normalised == sUrl) return;
  sUrl = normalised;
  if (sUrl.startsWith("http://")) {
    badge_log::tagf("broker", "WARNING: broker over plaintext http - offers are unauthenticated, "
                              "the sha256 check cannot detect an on-path rewrite");
  }
  // A token is only meaningful to the broker that issued it.
  forget();
}

void forget() {
  sToken = "";
  sNonce = "";
  settings::setBrokerToken("");
  sLastError = "";
  sLastContactMs = 0;
  sNextRequestAt = millis();
  if (enabled()) sState = State::NoNetwork;
  badge_log::tagf("broker", "registration forgotten");
}

State state() { return sState; }

const char *stateText() {
  switch (sState) {
    case State::Off: return url().length() == 0 ? "no address" : "off";
    case State::NoNetwork: return "offline";
    case State::NoIdentity: return "no identity";
    case State::Registering: return "registering";
    case State::Idle: return "idle";
    case State::Offered: return "offer waiting";
    case State::Installing: return "installing";
    case State::Error: return "error";
  }
  return "";
}

bool registered() { return sToken.length() > 0; }
String lastError() { return sLastError; }
uint32_t lastContactMs() { return sLastContactMs; }

bool hasOffer() { return sHasOffer; }
const Offer &offer() { return sOffer; }

void accept() {
  if (!sHasOffer) return;
  sHasOffer = false;
  sInstalling = true;
  sInstallStep = 0;
  sInstalledCount = 0;
  sFailedCount = 0;
  sInstalledJson = "";
  sFailedJson = "";
  sLastResult = "";
  sState = State::Installing;
  badge_log::tagf("broker", "accepted %s", sOffer.id.c_str());
}

void decline() {
  if (!sHasOffer) return;
  sHasOffer = false;

  // The one place a button press pays for a request. It is bounded by
  // BROKER_HTTP_TIMEOUT_MS, and telling the sender "no" promptly is worth a
  // couple of dropped frames.
  String reply;
  const int status = request("POST", "/api/v1/badge/offers/" + sOffer.id + "/decline", "{}",
                             "application/json", true, reply);
  if (status == 401 || status == 403) dropToken();

  badge_log::tagf("broker", "declined %s", sOffer.id.c_str());
  sOffer = Offer();
  if (sState == State::Offered) sState = State::Idle;
  sNextRequestAt = millis() + BROKER_POLL_MS;
}

bool installing() { return sInstalling; }

uint8_t installProgress() {
  if (!sInstalling) return sLastResult.length() ? 100 : 0;
  // count + 1 steps: one per script, plus the accept POST that closes it out.
  const uint16_t total = (uint16_t)sOffer.count + 1;
  return (uint8_t)((uint16_t)sInstallStep * 100 / total);
}

uint8_t installedCount() { return sInstalledCount; }
uint8_t failedCount() { return sFailedCount; }

String lastResult() { return sLastResult; }
void clearResult() { sLastResult = ""; }

}  // namespace broker
