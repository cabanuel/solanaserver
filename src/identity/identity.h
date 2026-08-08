/*
  The badge's cryptographic identity: one Ed25519 keypair, created on first
  boot. How well the private half is protected depends on which of the two
  sources below holds it: in the SecureElement it truly never leaves the part,
  but on the Software path the seed is stored in NVS in plaintext and CAN be
  recovered from a flash dump unless flash encryption was enabled at
  provisioning (see the SECURITY note in identity.cpp's persist()). So "never
  leaves the device" is only true of the secure-element path.

  Ed25519 because that is what Solana signs with, so the public key is a Solana
  address and the base58 the rest of the system passes around is the same
  encoding a wallet would show. The first eight characters of that base58 are
  the badge ID a human types into the App Store page - see broker/PROTOCOL.md.

  Two places the private key can live:

    SecureElement  generated inside the SE050 and used there. The private half
                   is never readable over I2C, by us or by anyone who takes the
                   badge apart. This is what the part is for.
    Software       a key in NVS, signed with a vendored Ed25519. Used when the
                   SE050 does not answer, or when its applet refuses the
                   commands - a badge with a dead secure element still has a
                   working identity rather than no identity at all.

  Which one is in use is not cosmetic and is reported honestly through source()
  and sourceName(), on the Settings > Identity screen and in the register call.

  begin() is called once from setup(), after badge_i2c and se050 are up and
  after settings::begin(), and may take a second or two the first time because
  key generation is slow on both paths.
*/
#pragma once

#include <Arduino.h>

namespace identity {

enum class Source : uint8_t {
  None,           // begin() has not run, or it failed outright
  SecureElement,  // key generated in and signed by the SE050
  Software,       // key in NVS, signed on the ESP32
};

// Loads the stored identity, or creates one. Returns false only when both
// paths failed, which leaves ready() false and every accessor empty; the rest
// of the firmware has to keep working in that state.
bool begin();

bool ready();
Source source();
const char *sourceName();  // "secure element" | "software" | "none"

// 32 raw bytes. Valid only while ready(); null otherwise.
const uint8_t *publicKey();

// The 32-byte key, base58-encoded (Bitcoin alphabet). 43-44 characters.
String publicKeyBase58();

// First 8 characters of publicKeyBase58(), which is the badge ID the broker
// and the App Store page use. Empty when not ready.
String badgeId();

// Ed25519 signature over `length` bytes, 64 bytes into `out`. False if the
// identity is not ready or the SE050 refused - callers must check, since a
// silent zero signature would be rejected by the broker in a way that is much
// harder to diagnose than a local failure.
bool sign(const uint8_t *message, size_t length, uint8_t out[64]);

// Same, base64 (standard alphabet, padded) - the form broker/PROTOCOL.md wants.
// Empty string on failure.
String signBase64(const uint8_t *message, size_t length);
String signBase64(const String &message);

// Throws the current keypair away and makes a new one. The badge ID changes, so
// this un-registers the badge from any broker; Settings warns before calling.
bool regenerate();

// One short line for the UI, e.g. "SE050, key 0xF0000001" or "software key".
String status();

// Base58 (Bitcoin alphabet) and base64 helpers, exposed because the broker
// client needs them too and there is no reason for two copies.
String base58Encode(const uint8_t *data, size_t length);
String base64Encode(const uint8_t *data, size_t length);

}  // namespace identity
