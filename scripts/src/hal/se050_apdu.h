/*
  The SE05x APDUs the badge's Ed25519 identity needs.

  This is the applet layer. Everything below it - the T=1 block framing, the
  CRC, the sequence numbers, chaining, waiting-time extensions - belongs to
  se050_t1.h, which is deliberately shared: the SE050 has one command sequence,
  one selected applet and one set of sequence counters, so a second independent
  block layer on the same bus would corrupt the first one's idea of both.

  Five commands, and nothing else:

    begin                 bring the link up and confirm the applet answers
    objectExists          does our key object already exist
    generateEd25519Pair   create an Ed25519 key pair inside the part
    readEd25519PublicKey  read the public half out, 32 raw bytes
    signEd25519           EdDSA over a message, 64 raw signature bytes
    deleteObject          throw the key away, for Settings > regenerate

  Ed25519 and not P-256, because a P-256 key would be perfectly secure and
  completely useless: Solana verifies Ed25519, and so does the broker. If the
  part will not do Ed25519 this module fails and identity.cpp falls back to
  software rather than quietly producing a key nothing can check.

  Everything here fails closed. There is no error this module reports as
  success and no state left behind that would make the next call behave
  differently. See the block comment at the top of the .cpp for what is
  verified and what is not - the short version is that none of it has ever run
  against a real part.
*/
#pragma once

#include <Arduino.h>

#include "../config.h"

namespace se050_apdu {

// Brings up the shared T=1 link. False means every other call in this header
// will also fail; the caller should stop trying and say so.
bool begin();

bool ready();

// Last SW1SW2 seen, 0 when the failure happened below the APDU layer (no
// answer, bad CRC, a block that never came). 0x9000 is success.
uint16_t lastStatusWord();

// The stage the last failure happened at - "select", "exists", "generate",
// "read", "sign", "delete", "t1". Empty when nothing has failed.
const char *lastStage();

bool objectExists(uint32_t objectId, bool &exists);
bool deleteObject(uint32_t objectId);

// Creates an Ed25519 key pair under `objectId`. Slow - the part does a scalar
// multiplication and an NVM write - and slower than the shared transport's own
// budget, so this retries by asking whether the object appeared anyway. See the
// .cpp; it is the one place here that has to work around a bound set elsewhere.
bool generateEd25519Pair(uint32_t objectId);

// The public key in RFC 8032 order, which is the reverse of the order the part
// returns it in.
bool readEd25519PublicKey(uint32_t objectId, uint8_t publicKey[32]);

// EdDSA (pure Ed25519, SHA-512) over `message`, in RFC 8032 order. Pure
// Ed25519 hashes inside the part, so the message travels whole rather than as a
// digest - hence a ceiling, set well above the ~90-byte registration challenge
// that is the only thing the badge actually signs.
constexpr size_t MAX_SIGN_MESSAGE_BYTES = 180;
bool signEd25519(uint32_t objectId, const uint8_t *message, size_t length, uint8_t signature[64]);

}  // namespace se050_apdu
