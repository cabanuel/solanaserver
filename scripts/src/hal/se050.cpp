#include "se050.h"

#include <Wire.h>

#include "../badge_log.h"
#include "badge_i2c.h"
#include "se050_t1.h"

namespace se050 {
namespace {

bool sPresent = false;
uint8_t sAtr[SE050_ATR_LENGTH] = {0};
uint8_t sAtrLength = 0;
uint8_t sLastError = 0;

}  // namespace

void begin() {
  // ENA/RST is an active-high enable on this board. Left floating the part can
  // sit in reset and never answer, so drive it explicitly and give it time.
  pinMode(PIN_SE050_ISO_RST, OUTPUT);
  digitalWrite(PIN_SE050_ISO_RST, SE050_ENABLE_ACTIVE_HIGH ? HIGH : LOW);
  delay(5);
}

bool test() {
  sPresent = false;
  sAtrLength = 0;
  sLastError = 0;

  // T=1-over-I2C soft reset (SE05x host interface spec).
  static const uint8_t SOFT_RESET[5] = {0x5A, 0xCF, 0x00, 0x37, 0x7F};

  Wire.setTimeOut(SE050_TIMEOUT_MS);
  Wire.beginTransmission(SE050_ADDR);
  Wire.write(SOFT_RESET, sizeof(SOFT_RESET));
  const uint8_t txResult = Wire.endTransmission(true);
  if (txResult != 0) {
    sLastError = txResult;
    badge_i2c::noteFailure(txResult);
    Wire.setTimeOut(I2C_TIMEOUT_MS);
    return false;
  }

  delay(15);  // the part needs time to produce its ATR
  const size_t received = Wire.requestFrom((int)SE050_ADDR, (int)SE050_ATR_LENGTH, true);
  while (Wire.available() && sAtrLength < SE050_ATR_LENGTH) {
    sAtr[sAtrLength++] = (uint8_t)Wire.read();
  }
  while (Wire.available()) Wire.read();
  Wire.setTimeOut(I2C_TIMEOUT_MS);

  if (received != SE050_ATR_LENGTH || sAtrLength != SE050_ATR_LENGTH) {
    sLastError = (sAtrLength == 0) ? 2 : 4;
    badge_i2c::noteFailure(sLastError);
    return false;
  }

  badge_i2c::noteSuccess();
  sPresent = true;
  badge_log::tagf("se050", "ATR ok (%u bytes)", sAtrLength);
  return true;
}

bool present() { return sPresent; }
const uint8_t *atr() { return sAtr; }
uint8_t atrLength() { return sAtrLength; }
uint8_t lastError() { return sLastError; }

// -- GetRandom ---------------------------------------------------------------
//
// The SE05x command layer. Everything here is from AN12413 and NXP's own host
// middleware rather than reconstructed - see the SOURCES block in se050_t1.h -
// but unlike the block layer below it, none of it has been seen working against
// real silicon. If the applet selects but this returns nothing, the encoding
// here is the place to look, and a bus capture is worth more than any amount of
// re-reading.
//
// [AN] Table 277 lists GetRandom as CLA 0x80, INS 0x04, P1 0x00, P2 0x49.
// [PNT] se05x_APDU_impl.h Se05x_API_GetRandom builds exactly that header from
// kSE05x_CLA / kSE05x_INS_MGMT / kSE05x_P1_DEFAULT / kSE05x_P2_RANDOM, and
// writes the requested size as TLVSET_U16(kSE05x_TAG_1, size) - tag 0x41,
// length 0x02, then the count big-endian. [AN] Table 271 agrees.

namespace {

constexpr uint8_t SE05X_CLA        = 0x80;
constexpr uint8_t SE05X_INS_MGMT   = 0x04;
constexpr uint8_t SE05X_P1_DEFAULT = 0x00;
constexpr uint8_t SE05X_P2_RANDOM  = 0x49;
constexpr uint8_t SE05X_TAG_1      = 0x41;

// One exchange's worth. Kept well inside a single T=1 block so a request never
// has to chain, and inside one Lua callback's time budget.
constexpr size_t MAX_PER_EXCHANGE = 64;

bool getRandomChunk(uint8_t *out, size_t length) {
  // Short-form APDU: 80 04 00 49 | Lc=04 | 41 02 <size hi> <size lo> | Le=00.
  //
  // NXP's middleware actually emits the *extended* form (3-byte Lc, 2-byte Le)
  // because its generic transmit path sets hasle=1. Both are legal - [AN]
  // section 4.1 says the applet accepts standard and extended length - and the
  // short form is what [AN] Table 271 documents and what independent drivers
  // send. It is also 3 bytes shorter, which matters not at all, but it is the
  // simpler thing to get right by hand.
  uint8_t apdu[10];
  apdu[0] = SE05X_CLA;
  apdu[1] = SE05X_INS_MGMT;
  apdu[2] = SE05X_P1_DEFAULT;
  apdu[3] = SE05X_P2_RANDOM;
  apdu[4] = 0x04;  // Lc: the TLV below is four bytes
  apdu[5] = SE05X_TAG_1;
  apdu[6] = 0x02;  // TLV length: the size field is two bytes
  apdu[7] = (uint8_t)((length >> 8) & 0xFF);
  apdu[8] = (uint8_t)(length & 0xFF);
  apdu[9] = 0x00;  // Le

  static uint8_t body[se050_t1::MAX_RESPONSE];
  size_t bodyLength = 0;
  uint16_t status = 0;

  if (!se050_t1::transceive(apdu, sizeof(apdu), body, sizeof(body), bodyLength, status)) {
    badge_log::tagf("se050", "GetRandom transport failed: %s", se050_t1::lastError());
    return false;
  }
  if (status != 0x9000) {
    badge_log::tagf("se050", "GetRandom refused: SW=%04X", status);
    return false;
  }

  // Response body is TLV[TAG_1] = <tag> <length> <random>. The length field is
  // ISO 7816-4 Annex D.3 as restricted by [AN] 4.1.3.2: one byte up to 0x7F,
  // else 0x81 + one byte, else 0x82 + two. Only the first form can occur at
  // these sizes, but parsing the others costs four lines and removes a cliff.
  if (bodyLength < 2 || body[0] != SE05X_TAG_1) {
    badge_log::tagf("se050", "GetRandom: unexpected response tag");
    return false;
  }

  size_t offset = 0;
  size_t tlvLength = 0;
  if (body[1] <= 0x7F) {
    tlvLength = body[1];
    offset = 2;
  } else if (body[1] == 0x81 && bodyLength >= 3) {
    tlvLength = body[2];
    offset = 3;
  } else if (body[1] == 0x82 && bodyLength >= 4) {
    tlvLength = ((size_t)body[2] << 8) | (size_t)body[3];
    offset = 4;
  } else {
    badge_log::tagf("se050", "GetRandom: bad TLV length encoding");
    return false;
  }

  // Insist on exactly what was asked for, and on nothing trailing it. A short
  // or padded answer is a sign the encoding is wrong somewhere, and handing the
  // caller whatever arrived would hide that behind numbers that look random.
  if (tlvLength != length || offset + tlvLength != bodyLength) {
    badge_log::tagf("se050", "GetRandom: wanted %u bytes, TLV says %u of %u",
                    (unsigned)length, (unsigned)tlvLength, (unsigned)bodyLength);
    return false;
  }

  memcpy(out, body + offset, tlvLength);
  return true;
}

}  // namespace

bool randomBytes(uint8_t *out, size_t length) {
  if (out == nullptr || length == 0) return false;

  size_t done = 0;
  while (done < length) {
    const size_t remaining = length - done;
    const size_t chunk = remaining > MAX_PER_EXCHANGE ? MAX_PER_EXCHANGE : remaining;
    if (!getRandomChunk(out + done, chunk)) {
      // Zero the whole buffer, including the chunks that did succeed. A caller
      // that ignores the return value then gets an obvious nothing rather than
      // a half-filled buffer that passes a glance.
      memset(out, 0, length);
      return false;
    }
    done += chunk;
  }
  return true;
}

String atrHex() {
  String out;
  out.reserve(sAtrLength * 2);
  char pair[3];
  for (uint8_t i = 0; i < sAtrLength; ++i) {
    snprintf(pair, sizeof(pair), "%02x", sAtr[i]);
    out += pair;
  }
  return out;
}

}  // namespace se050
