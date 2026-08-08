/*
  Ed25519 on the ESP32, wrapped around the vendored TweetNaCl.

  Two things this adds to tweetnacl.c, which is left untouched (see
  TWEETNACL-README):

    randombytes()  TweetNaCl declares it and refuses to supply it, on the
                   grounds that only the platform knows where entropy comes
                   from. Ours is the ESP32 hardware RNG.
    detached form  TweetNaCl only offers the combined form, where the
                   signature and the message come back in one buffer. The
                   broker wants the 64 signature bytes on their own.

  The keypair is handled as a 32-byte seed rather than TweetNaCl's 64-byte
  secret key, because the second half of that secret key is just the public
  key - which identity.cpp already stores separately for every source. So the
  seed is the only thing that has to be kept secret, and it is the only thing
  written to NVS.
*/
#pragma once

#include <Arduino.h>

namespace ed25519 {

constexpr size_t SEED_BYTES = 32;
constexpr size_t PUBLIC_KEY_BYTES = 32;
constexpr size_t SIGNATURE_BYTES = 64;

// Optional defense-in-depth hook: XOR up to 32 bytes of caller-supplied entropy
// into the seed that the very next generate() draws, then forget it. Because it
// is mixed in before crypto_sign_keypair derives the public key, the resulting
// seed/public-key pair stays consistent - unlike XORing a finished seed, which
// would desync it from its key (there is deliberately no seed-to-key function).
// Call it right before generate(); a length of 0 or a null pointer clears it.
void mixEntropy(const uint8_t *data, size_t length);

// A fresh seed from the hardware RNG, and the public key it implies. There is
// no seed-to-public-key function here on purpose: TweetNaCl keeps the Edwards
// base point multiplication private, and the only reason to want one would be
// to check a stored pair, which verify() below does more convincingly anyway.
void generate(uint8_t seed[32], uint8_t publicKey[32]);

// Detached signature. `publicKey` must be the one that belongs to `seed` -
// TweetNaCl copies it into the signed message rather than deriving it, so a
// mismatched pair produces a signature that verifies against nothing.
bool sign(const uint8_t *message,
          size_t length,
          const uint8_t seed[32],
          const uint8_t publicKey[32],
          uint8_t signature[64]);

// Only used by the self-test at first-key creation: a key that cannot verify
// its own signature is a key that would fail silently at the broker instead.
bool verify(const uint8_t *message,
            size_t length,
            const uint8_t signature[64],
            const uint8_t publicKey[32]);

}  // namespace ed25519
