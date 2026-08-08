#include "ed25519.h"

#include <esp_random.h>
#include <string.h>

#include "../badge_log.h"

// Optional extra entropy XORed into the very next seed draw, for defense in
// depth against a weak platform RNG. It is one-shot: consumed and cleared the
// first time randombytes() runs after it is set, so unrelated later draws are
// never correlated with it. See ed25519::mixEntropy(). File scope because the
// C hook below has to reach it.
static uint8_t sMixEntropy[32];
static bool sHaveMixEntropy = false;

extern "C" {
#include "tweetnacl.h"

// TweetNaCl's one platform hook. esp_fill_random() draws from the SHA/AES
// entropy pool, which is only a true random number generator while an RF
// subsystem is running - with Wi-Fi and BLE both off the ESP32 falls back to a
// pseudo-random source seeded at boot. setup() works around this by holding
// bootloader_random_enable() across identity::begin() (see solana-os.ino), and
// identity.cpp mixes in SE050 hardware entropy via mixEntropy() when the part
// is present. The SE050 path does not have this problem at all, which is one
// more reason to prefer it.
void randombytes(unsigned char *out, unsigned long long length) {
  esp_fill_random(out, (size_t)length);
  if (sHaveMixEntropy) {
    const size_t n = (length < sizeof(sMixEntropy)) ? (size_t)length : sizeof(sMixEntropy);
    for (size_t i = 0; i < n; ++i) out[i] ^= sMixEntropy[i];
    memset(sMixEntropy, 0, sizeof(sMixEntropy));
    sHaveMixEntropy = false;
  }
}
}

namespace ed25519 {
namespace {

// TweetNaCl only signs into a buffer that holds the signature and the message
// together, so signing and verifying both need scratch of message + 64 bytes.
// Everything the badge signs is a short challenge string, and a request for
// megabytes here is a bug rather than a workload.
constexpr size_t MAX_MESSAGE_BYTES = 4096;

}  // namespace

void mixEntropy(const uint8_t *data, size_t length) {
  // Setting fresh entropy replaces (does not accumulate) any pending block, so
  // the software path always mixes exactly one source into one keypair.
  memset(sMixEntropy, 0, sizeof(sMixEntropy));
  if (data && length) {
    const size_t n = (length < sizeof(sMixEntropy)) ? length : sizeof(sMixEntropy);
    memcpy(sMixEntropy, data, n);
    sHaveMixEntropy = true;
  } else {
    sHaveMixEntropy = false;
  }
}

void generate(uint8_t seed[32], uint8_t publicKey[32]) {
  uint8_t secret[64];
  crypto_sign_keypair(publicKey, secret);
  memcpy(seed, secret, 32);  // secret[32..63] is publicKey, already returned
  memset(secret, 0, sizeof(secret));
}

bool sign(const uint8_t *message,
          size_t length,
          const uint8_t seed[32],
          const uint8_t publicKey[32],
          uint8_t signature[64]) {
  if (!message && length) return false;
  if (length > MAX_MESSAGE_BYTES) {
    badge_log::tagf("id", "sign refused, %u bytes over limit", (unsigned)length);
    return false;
  }

  uint8_t secret[64];
  memcpy(secret, seed, 32);
  memcpy(secret + 32, publicKey, 32);

  uint8_t *signed_ = (uint8_t *)malloc(length + 64);
  if (!signed_) {
    memset(secret, 0, sizeof(secret));
    return false;
  }

  unsigned long long signedLength = 0;
  const int result = crypto_sign(signed_, &signedLength, message, (unsigned long long)length, secret);
  memset(secret, 0, sizeof(secret));

  const bool ok = (result == 0) && (signedLength == length + 64);
  if (ok) memcpy(signature, signed_, 64);
  free(signed_);
  return ok;
}

bool verify(const uint8_t *message,
            size_t length,
            const uint8_t signature[64],
            const uint8_t publicKey[32]) {
  if (!message && length) return false;
  if (length > MAX_MESSAGE_BYTES) return false;

  uint8_t *signed_ = (uint8_t *)malloc(length + 64);
  uint8_t *recovered = (uint8_t *)malloc(length + 64);
  if (!signed_ || !recovered) {
    free(signed_);
    free(recovered);
    return false;
  }

  memcpy(signed_, signature, 64);
  memcpy(signed_ + 64, message, length);

  unsigned long long recoveredLength = 0;
  const int result =
      crypto_sign_open(recovered, &recoveredLength, signed_, (unsigned long long)(length + 64), publicKey);

  const bool ok = (result == 0) && (recoveredLength == length) &&
                  (length == 0 || memcmp(recovered, message, length) == 0);
  free(signed_);
  free(recovered);
  return ok;
}

}  // namespace ed25519
