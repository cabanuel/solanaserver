#include "identity.h"

#include <Preferences.h>
#include <string.h>

#include "../badge_log.h"
#include "../config.h"
#include "../hal/se050.h"       // se050::randomBytes() for seed defense-in-depth
#include "../hal/se050_apdu.h"
#include "ed25519.h"

namespace identity {
namespace {

// Its own NVS namespace, deliberately not "sysconf" and deliberately not
// "luakv": a Lua app that fills its key/value store, or a settings reset, must
// not be able to take the badge's identity with it.
Preferences sStore;

// NVS keys are capped at 15 characters.
constexpr char KEY_SOURCE[] = "src";
constexpr char KEY_PUBLIC[] = "pub";
constexpr char KEY_SEED[] = "seed";

// What a signature has to survive before we will call a new key good. Signing
// and verifying it costs a second or two of TweetNaCl on the software path, so
// it happens once at creation and never again - but it is worth that once. On
// the SE050 path it is doing something more interesting than a smoke test: it
// checks a signature made inside the part against a software verifier, which is
// the only way to catch the byte-order handling in se050_apdu.cpp being wrong.
const char SELF_TEST_MESSAGE[] = "solana-badge identity self test";

// How long the secure element gets before we stop waiting and take the software
// key instead. Generous, because it only ever applies on the first boot of a
// badge, and boot is already showing a splash screen - but bounded, because a
// half-dead part that answers slowly must not hold the badge at the splash.
constexpr uint32_t SE050_BUDGET_MS = 12000;

bool sReady = false;
Source sSource = Source::None;
uint8_t sPublicKey[32] = {0};
uint8_t sSeed[32] = {0};
bool sHaveSeed = false;
String sPublicKeyBase58;

// Cleared when a stored SecureElement identity is loaded but the part's live
// public key no longer matches what we persisted (see load()). While false the
// SE050 sign path refuses rather than producing signatures against a key the
// broker will not recognise.
bool sSeKeyTrusted = true;

// A seed of all-zeros, or every byte identical, is not something a working RNG
// produces; it is the signature of an entropy source that failed. Reject it and
// draw again rather than persisting a key an attacker could reproduce.
bool seedLooksDegenerate(const uint8_t *seed) {
  bool allZero = true;
  bool allSame = true;
  for (size_t i = 0; i < 32; ++i) {
    if (seed[i] != 0) allZero = false;
    if (seed[i] != seed[0]) allSame = false;
  }
  return allZero || allSame;
}

bool selfTest(bool useSecureElement) {
  const uint8_t *message = (const uint8_t *)SELF_TEST_MESSAGE;
  const size_t length = sizeof(SELF_TEST_MESSAGE) - 1;
  uint8_t signature[64];

  if (useSecureElement) {
    if (!se050_apdu::signEd25519(SE050_IDENTITY_KEY_ID, message, length, signature)) return false;
  } else {
    if (!ed25519::sign(message, length, sSeed, sPublicKey, signature)) return false;
  }
  return ed25519::verify(message, length, signature, sPublicKey);
}

void cache() {
  sPublicKeyBase58 = base58Encode(sPublicKey, sizeof(sPublicKey));
}

void persist(Source source) {
  sStore.putUChar(KEY_SOURCE, (uint8_t)source);
  sStore.putBytes(KEY_PUBLIC, sPublicKey, sizeof(sPublicKey));
  if (source == Source::Software) {
    // SECURITY / PROVISIONING: the software seed is the private key, and it is
    // written to NVS in the clear. NVS holds it in plaintext flash unless flash
    // encryption is enabled, so on a badge without flash encryption this key
    // CAN be recovered by anyone who dumps the SPI flash - it does not "never
    // leave the device". Flash encryption and secure boot must be turned on at
    // provisioning time (an irreversible eFuse burn, so it is a build/factory
    // step and deliberately NOT done from firmware). The SE050 path avoids this
    // entirely, which is why it is preferred. See identity.h.
    sStore.putBytes(KEY_SEED, sSeed, sizeof(sSeed));
  } else {
    sStore.remove(KEY_SEED);
  }
}

// Brings up the applet and makes sure our key object is there, creating it if
// it is not. Every failure is logged with the stage and the status word,
// because from a serial console that is the whole diagnosis.
bool createOnSecureElement(uint32_t deadline) {
  if (!se050_apdu::begin()) {
    badge_log::tagf("id", "SE050 unavailable (%s)", se050_apdu::lastStage());
    return false;
  }

  bool exists = false;
  if (!se050_apdu::objectExists(SE050_IDENTITY_KEY_ID, exists)) {
    badge_log::tagf("id", "SE050 refused at %s, sw %04x", se050_apdu::lastStage(),
                    se050_apdu::lastStatusWord());
    return false;
  }

  if (!exists) {
    if ((int32_t)(millis() - deadline) >= 0) {
      badge_log::tagf("id", "SE050 out of time before key generation");
      return false;
    }
    badge_log::tagf("id", "generating Ed25519 key 0x%08lx in SE050", (unsigned long)SE050_IDENTITY_KEY_ID);
    if (!se050_apdu::generateEd25519Pair(SE050_IDENTITY_KEY_ID)) {
      badge_log::tagf("id", "SE050 refused at %s, sw %04x", se050_apdu::lastStage(),
                      se050_apdu::lastStatusWord());
      return false;
    }
  }

  if (!se050_apdu::readEd25519PublicKey(SE050_IDENTITY_KEY_ID, sPublicKey)) {
    badge_log::tagf("id", "SE050 refused at %s, sw %04x", se050_apdu::lastStage(),
                    se050_apdu::lastStatusWord());
    return false;
  }

  if (!selfTest(true)) {
    badge_log::tagf("id", "SE050 signature failed local verification, not trusting it");
    return false;
  }

  sSource = Source::SecureElement;
  sHaveSeed = false;
  memset(sSeed, 0, sizeof(sSeed));
  return true;
}

bool createInSoftware() {
  // A degenerate seed is rare enough that a handful of retries is plenty; a
  // source that keeps producing them is broken in a way retrying will not fix.
  constexpr int kMaxAttempts = 8;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    // Defense in depth: if the secure element answered, fold 32 bytes of its
    // hardware RNG into the seed the ESP32 is about to draw. randomBytes()
    // zeroes its buffer and returns false when the part is absent, so on a
    // software-only badge this mixes in nothing and the path is unchanged. The
    // mix happens *inside* generate()'s seed draw (see ed25519::mixEntropy), so
    // the seed and public key stay a matched pair.
    uint8_t seEntropy[32];
    if (se050::randomBytes(seEntropy, sizeof(seEntropy))) {
      ed25519::mixEntropy(seEntropy, sizeof(seEntropy));
    }
    memset(seEntropy, 0, sizeof(seEntropy));

    ed25519::generate(sSeed, sPublicKey);

    if (seedLooksDegenerate(sSeed)) {
      badge_log::tagf("id", "rejected a degenerate seed, drawing again");
      memset(sSeed, 0, sizeof(sSeed));
      continue;
    }

    sHaveSeed = true;
    if (!selfTest(false)) {
      badge_log::tagf("id", "software key failed its own self test");
      sHaveSeed = false;
      memset(sSeed, 0, sizeof(sSeed));
      return false;  // a self-test failure is a bug, not something a retry fixes
    }
    sSource = Source::Software;
    return true;
  }

  badge_log::tagf("id", "could not produce a non-degenerate seed after %d tries", kMaxAttempts);
  return false;
}

bool load() {
  const uint8_t stored = sStore.getUChar(KEY_SOURCE, (uint8_t)Source::None);
  if (stored != (uint8_t)Source::SecureElement && stored != (uint8_t)Source::Software) return false;
  if (sStore.getBytes(KEY_PUBLIC, sPublicKey, sizeof(sPublicKey)) != sizeof(sPublicKey)) return false;

  if (stored == (uint8_t)Source::Software) {
    if (sStore.getBytes(KEY_SEED, sSeed, sizeof(sSeed)) != sizeof(sSeed)) return false;
    sHaveSeed = true;
    sSource = Source::Software;
    return true;
  }

  // A stored secure-element identity is kept even when the part does not answer
  // this boot. Regenerating instead would hand the badge a new badge ID and
  // silently unregister it from its broker, which is a far worse outcome than a
  // boot where signing fails and says so.
  sSource = Source::SecureElement;
  if (!se050_apdu::begin()) {
    badge_log::tagf("id", "SE050 did not answer; identity present but cannot sign this boot");
    return true;  // it may answer later; keep the identity and let sign() retry
  }

  // The part answered, so verify it is still holding the key we stored. Re-read
  // the public-key object and compare it against KEY_PUBLIC. A mismatch means
  // the object was regenerated or replaced out from under us; signing with it
  // would silently produce signatures that verify against a different key, so
  // we mark the identity untrusted and let sign() refuse rather than persisting
  // a lie. We do NOT regenerate here - that would change the badge ID on its
  // own, which is exactly the surprise we are trying to avoid. A transient read
  // failure is not treated as a mismatch: it leaves trust intact so a flaky bus
  // does not disable a good identity.
  uint8_t livePublic[32];
  if (!se050_apdu::readEd25519PublicKey(SE050_IDENTITY_KEY_ID, livePublic)) {
    badge_log::tagf("id", "SE050 answered but its public key could not be read at %s, sw %04x",
                    se050_apdu::lastStage(), se050_apdu::lastStatusWord());
    return true;  // transient; keep the identity, do not distrust on a bad read
  }
  if (memcmp(livePublic, sPublicKey, sizeof(sPublicKey)) != 0) {
    badge_log::tagf("id", "SE050 public key does not match stored identity; refusing to sign with it");
    sSeKeyTrusted = false;
  }
  return true;
}

bool create(uint32_t startedAt) {
  const uint32_t deadline = startedAt + SE050_BUDGET_MS;
  if (createOnSecureElement(deadline)) return true;

  badge_log::tagf("id", "falling back to a software key");
  return createInSoftware();
}

}  // namespace

bool begin() {
  const uint32_t startedAt = millis();
  sReady = false;
  sSource = Source::None;
  sHaveSeed = false;
  sSeKeyTrusted = true;

  if (!sStore.begin(IDENTITY_NVS_NAMESPACE, false)) {
    badge_log::tagf("id", "NVS namespace '%s' would not open", IDENTITY_NVS_NAMESPACE);
    return false;
  }

  bool haveKey = load();
  if (!haveKey) {
    haveKey = create(startedAt);
    if (haveKey) persist(sSource);
  }

  if (!haveKey) {
    badge_log::tagf("id", "no identity: both the secure element and software failed");
    return false;
  }

  cache();
  sReady = true;
  badge_log::tagf("id", "%s, %s (%lu ms)", badgeId().c_str(), sourceName(),
                  (unsigned long)(millis() - startedAt));
  return true;
}

bool ready() { return sReady; }
Source source() { return sSource; }

const char *sourceName() {
  switch (sSource) {
    case Source::SecureElement: return "secure element";
    case Source::Software: return "software";
    default: return "none";
  }
}

const uint8_t *publicKey() { return sReady ? sPublicKey : nullptr; }

String publicKeyBase58() { return sReady ? sPublicKeyBase58 : String(); }

String badgeId() { return sReady ? sPublicKeyBase58.substring(0, 8) : String(); }

bool sign(const uint8_t *message, size_t length, uint8_t out[64]) {
  if (!sReady || !out) return false;

  if (sSource == Source::SecureElement) {
    // load() cleared this when the part's live public key did not match what we
    // stored. Signing anyway would hand out signatures against the wrong key.
    if (!sSeKeyTrusted) {
      badge_log::tagf("id", "cannot sign: SE050 key does not match the stored identity");
      return false;
    }
    // The link may have been dropped by an earlier failed exchange; bringing it
    // back is cheap and is the difference between one bad transaction and a
    // badge that cannot sign again until it is power-cycled.
    if (!se050_apdu::ready() && !se050_apdu::begin()) {
      badge_log::tagf("id", "cannot sign: SE050 holds the key and is not answering");
      return false;
    }
    if (se050_apdu::signEd25519(SE050_IDENTITY_KEY_ID, message, length, out)) return true;
    badge_log::tagf("id", "SE050 refused to sign at %s, sw %04x", se050_apdu::lastStage(),
                    se050_apdu::lastStatusWord());
    return false;
  }

  if (!sHaveSeed) return false;
  return ed25519::sign(message, length, sSeed, sPublicKey, out);
}

String signBase64(const uint8_t *message, size_t length) {
  uint8_t signature[64];
  if (!sign(message, length, signature)) return String();
  return base64Encode(signature, sizeof(signature));
}

String signBase64(const String &message) {
  return signBase64((const uint8_t *)message.c_str(), message.length());
}

bool regenerate() {
  const uint32_t startedAt = millis();
  const Source previous = sSource;

  sReady = false;
  sSource = Source::None;
  sHaveSeed = false;
  memset(sSeed, 0, sizeof(sSeed));
  memset(sPublicKey, 0, sizeof(sPublicKey));
  sPublicKeyBase58 = String();

  sStore.remove(KEY_SOURCE);
  sStore.remove(KEY_PUBLIC);
  sStore.remove(KEY_SEED);

  // The object inside the part has to go too, or the create path below finds it
  // still there and hands back the very key we were asked to throw away. A
  // failure here is logged rather than fatal: if the part cannot be reached the
  // create path will not reach it either, and the badge ends up with a fresh
  // software key, which is the outcome the user asked for.
  if (previous == Source::SecureElement || se050_apdu::ready()) {
    if (se050_apdu::begin() && !se050_apdu::deleteObject(SE050_IDENTITY_KEY_ID)) {
      badge_log::tagf("id", "could not delete SE050 key 0x%08lx, sw %04x",
                      (unsigned long)SE050_IDENTITY_KEY_ID, se050_apdu::lastStatusWord());
    }
  }

  if (!create(startedAt)) {
    badge_log::tagf("id", "regenerate failed, badge has no identity");
    return false;
  }
  persist(sSource);
  cache();
  sReady = true;
  badge_log::tagf("id", "regenerated: %s, %s (%lu ms)", badgeId().c_str(), sourceName(),
                  (unsigned long)(millis() - startedAt));
  return true;
}

String status() {
  switch (sSource) {
    case Source::SecureElement: {
      char line[32];
      snprintf(line, sizeof(line), "SE050, key 0x%08lX", (unsigned long)SE050_IDENTITY_KEY_ID);
      return String(line);
    }
    case Source::Software:
      return String("software key");
    default:
      return String("no identity");
  }
}

}  // namespace identity
