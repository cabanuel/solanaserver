/*
  base58 and base64, the two ways the badge's key and signatures leave the
  device.

  Split out from identity.cpp because they are pure and therefore the only part
  of the identity module that can be tested off the badge, and because
  broker_client.cpp uses them for its own payloads.

  base58 is the Bitcoin alphabet, which is also Solana's: the public key printed
  on screen is a Solana address, and a badge ID is the first eight characters of
  it (broker/PROTOCOL.md). The leading-zero rule is the part people get wrong -
  the big-number conversion cannot represent a leading zero byte, so each one is
  re-added as a literal '1'. Getting that wrong produces an encoding that is
  correct 99.6% of the time and silently wrong for the one badge in 256 whose
  key starts with a zero byte.
*/
#include <stdlib.h>
#include <string.h>

#include "identity.h"

namespace identity {
namespace {

const char BASE58_ALPHABET[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
const char BASE64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

String base58Encode(const uint8_t *data, size_t length) {
  if (!data || length == 0) return String();

  size_t zeros = 0;
  while (zeros < length && data[zeros] == 0) ++zeros;

  // log(256)/log(58) = 1.3658, so 138/100 rounded up by one covers every input.
  const size_t capacity = (length - zeros) * 138 / 100 + 1;
  uint8_t *digits = (uint8_t *)calloc(capacity, 1);
  if (!digits) return String();

  size_t used = 0;
  for (size_t i = zeros; i < length; ++i) {
    uint32_t carry = data[i];
    size_t written = 0;
    for (size_t j = capacity; j-- > 0 && (carry != 0 || written < used);) {
      carry += 256u * digits[j];
      digits[j] = (uint8_t)(carry % 58);
      carry /= 58;
      ++written;
    }
    used = written;
  }

  char *out = (char *)malloc(zeros + used + 1);
  if (!out) {
    free(digits);
    return String();
  }
  size_t at = 0;
  for (size_t i = 0; i < zeros; ++i) out[at++] = BASE58_ALPHABET[0];
  for (size_t i = capacity - used; i < capacity; ++i) out[at++] = BASE58_ALPHABET[digits[i]];
  out[at] = '\0';

  String encoded(out);
  free(out);
  free(digits);
  return encoded;
}

String base64Encode(const uint8_t *data, size_t length) {
  if (!data || length == 0) return String();

  const size_t outLength = ((length + 2) / 3) * 4;
  char *out = (char *)malloc(outLength + 1);
  if (!out) return String();

  size_t at = 0;
  for (size_t i = 0; i < length; i += 3) {
    const uint32_t remaining = length - i;
    const uint32_t triple = ((uint32_t)data[i] << 16) | ((remaining > 1 ? (uint32_t)data[i + 1] : 0) << 8) |
                            (remaining > 2 ? (uint32_t)data[i + 2] : 0);
    out[at++] = BASE64_ALPHABET[(triple >> 18) & 0x3F];
    out[at++] = BASE64_ALPHABET[(triple >> 12) & 0x3F];
    out[at++] = remaining > 1 ? BASE64_ALPHABET[(triple >> 6) & 0x3F] : '=';
    out[at++] = remaining > 2 ? BASE64_ALPHABET[triple & 0x3F] : '=';
  }
  out[at] = '\0';

  String encoded(out);
  free(out);
  return encoded;
}

}  // namespace identity
