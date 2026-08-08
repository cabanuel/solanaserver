/*
  ============================================================================
  THIS FILE HAS NEVER RUN AGAINST A REAL SE050.
  ============================================================================

  The encodings below were taken from, and are cited inline against:

    [AN]    AN12413 "SE050 APDU Specification" Rev 2.12,
            nxp.com/docs/en/application-note/AN12413.pdf
    [PNT]   github.com/NXP/plug-and-trust @ master (read 2026-08):
            hostlib/hostLib/inc/se05x_enums.h            - CLA/INS/P1/P2, tags
            hostlib/hostLib/inc/se05x_tlv.h              - TLV length rules
            hostlib/hostLib/se05x/src/se05x_tlv.c        - TLV writers
            hostlib/hostLib/se05x_03_xx_xx/se05x_APDU_impl.h
                                                         - each command's tags
            sss/src/se05x/fsl_sss_se05x_apis.c           - the byte-order rule

  The block layer underneath is se050_t1, whose own header documents what it
  checked and how. This file adds no framing of its own.

  What is most likely to be wrong here, in order:

    1. SCP03. Some SE050 configurations require a Platform SCP03 secure channel
       before any command is accepted. We do not have this part's SCP03 keys
       and this module deliberately does not implement SCP03 - if that is what
       is happening, the honest outcome is the software identity, not a
       half-secure one. The tell is a clean applet SELECT followed by 0x6982
       (security status not satisfied) on CheckObjectExists.
    2. Applet variant. SE050 ships as A1/A2/B1/B2/C1/C2/E with different
       feature sets, and Ed25519 is only on the ones with the full EC feature
       set. A part without it answers WriteECKey with 0x6A80 (wrong data) or
       0x6A81 (function not supported). The SELECT response carries the variant
       bitmap; se050_t1 logs the link coming up, and AN12436 decodes the bitmap.
    3. Byte order. The part stores and returns Ed25519 public keys and
       signatures with each 32-byte quantity reversed relative to RFC 8032.
       This is not stated in the prose spec; it is visible in [PNT]
       fsl_sss_se05x_apis.c, which reverses the public key in both
       key_store_get_key and key_store_set_key, and reverses each half of the
       signature independently around EdDSASign and EdDSAVerify. We do the same.
       identity.cpp's self test catches this one for free: it verifies an SE050
       signature with the software verifier, which fails outright if the
       reversal is wrong in either place.
    4. Key generation time versus se050_t1::BUDGET_MS. The shared transport
       bounds one exchange at 120 ms because a Lua callback has 250 ms to
       return. An on-chip Ed25519 key generation plus its NVM write can take
       longer than that even with WTX. generateEd25519Pair() below therefore
       treats a transport timeout as "maybe" and re-asks whether the object
       appeared, rather than as "no".

  How to debug with a badge and a serial cable: every failure logs its stage
  under the "se050" tag, and identity.cpp logs the stage and status word it
  fell back at under "id". A stage of "t1" with SW 0x0000 means the block layer
  never got a well-formed answer - a bus or reset problem, not an applet one. A
  stage with a non-zero SW means the applet answered and refused; look the
  status word up in AN12413 section 4.4.
*/
#include "se050_apdu.h"

#include <string.h>

#include "../badge_log.h"
#include "se050_t1.h"

namespace se050_apdu {
namespace {

// -- SE05x command encodings, [AN] section 4 / [PNT] se05x_enums.h ------------
constexpr uint8_t SE05X_CLA = 0x80;
constexpr uint8_t INS_WRITE = 0x01;
constexpr uint8_t INS_READ = 0x02;
constexpr uint8_t INS_CRYPTO = 0x03;
constexpr uint8_t INS_MGMT = 0x04;

constexpr uint8_t P1_DEFAULT = 0x00;
constexpr uint8_t P1_EC = 0x01;
constexpr uint8_t P1_SIGNATURE = 0x0C;
constexpr uint8_t P1_KEY_PAIR = 0x60;

constexpr uint8_t P2_DEFAULT = 0x00;
constexpr uint8_t P2_SIGN = 0x09;
constexpr uint8_t P2_EXIST = 0x27;
constexpr uint8_t P2_DELETE_OBJECT = 0x28;

constexpr uint8_t TAG_1 = 0x41;
constexpr uint8_t TAG_2 = 0x42;
constexpr uint8_t TAG_3 = 0x43;

constexpr uint8_t ECCURVE_ED25519 = 0x40;               // kSE05x_ECCurve_ECC_ED_25519
constexpr uint8_t EDSIG_ED25519PURE_SHA512 = 0xA3;      // kSE05x_EDSignatureAlgo_ED25519PURE_SHA_512
constexpr uint8_t RESULT_SUCCESS = 0x01;                // kSE05x_Result_SUCCESS

constexpr uint16_t SW_OK = 0x9000;

// Longest command APDU we build: the sign command with a maximum-length
// message, plus its header and TLV overhead.
constexpr size_t MAX_APDU = MAX_SIGN_MESSAGE_BYTES + 24;

// Backstop for a generation that outruns even se050_t1::SLOW_BUDGET_MS. The
// part may well have finished anyway, so ask again rather than declaring
// failure - each ask re-runs the T=1 handshake, which is also the right
// recovery from an exchange that was abandoned mid-command.
constexpr uint8_t GENERATE_RECHECKS = 6;
constexpr uint32_t GENERATE_RECHECK_MS = 250;

uint16_t sStatusWord = 0;
const char *sStage = "";

void fail(const char *stage, uint16_t statusWord) {
  sStage = stage;
  sStatusWord = statusWord;
}

// -- APDU and TLV construction -----------------------------------------------
// [PNT] sss_se05x_channel_txnRaw builds a plain (no secure channel) command as
// header, then a one-byte Lc when there is data and a lone 0x00 when there is
// not, and no Le at all - the applet returns its response data regardless. Our
// commands never exceed a one-byte Lc, which is checked below.

size_t buildApdu(uint8_t *out, uint8_t ins, uint8_t p1, uint8_t p2, const uint8_t *data, size_t dataLength) {
  out[0] = SE05X_CLA;
  out[1] = ins;
  out[2] = p1;
  out[3] = p2;
  if (dataLength == 0) {
    out[4] = 0x00;
    return 5;
  }
  out[4] = (uint8_t)dataLength;
  memcpy(out + 5, data, dataLength);
  return 5 + dataLength;
}

size_t tlvU8(uint8_t *out, uint8_t tag, uint8_t value) {
  out[0] = tag;
  out[1] = 1;
  out[2] = value;
  return 3;
}

size_t tlvU32(uint8_t *out, uint8_t tag, uint32_t value) {
  out[0] = tag;
  out[1] = 4;
  out[2] = (uint8_t)(value >> 24);
  out[3] = (uint8_t)(value >> 16);
  out[4] = (uint8_t)(value >> 8);
  out[5] = (uint8_t)value;
  return 6;
}

// [PNT] tlvSet_u8buf: one length byte below 0x80, then 0x81 + one byte, then
// 0x82 + two. Only the first two forms can occur at our sizes.
size_t tlvBuf(uint8_t *out, uint8_t tag, const uint8_t *data, size_t length) {
  out[0] = tag;
  size_t at = 1;
  if (length <= 0x7F) {
    out[at++] = (uint8_t)length;
  } else {
    out[at++] = 0x81;
    out[at++] = (uint8_t)length;
  }
  memcpy(out + at, data, length);
  return at + length;
}

// Every response we care about is a single TLV, and se050_t1 has already split
// off the status word.
bool tlvFirst(const uint8_t *body, size_t bodyLength, uint8_t tag, const uint8_t **value, size_t *valueLength) {
  if (bodyLength < 2 || body[0] != tag) return false;
  size_t at = 1;
  size_t declared = body[at++];
  if (declared == 0x81) {
    if (at >= bodyLength) return false;
    declared = body[at++];
  } else if (declared == 0x82) {
    if (at + 1 >= bodyLength) return false;
    declared = ((size_t)body[at] << 8) | body[at + 1];
    at += 2;
  } else if (declared > 0x7F) {
    return false;
  }
  if (at + declared > bodyLength) return false;
  *value = body + at;
  *valueLength = declared;
  return true;
}

// The part works in the opposite byte order to RFC 8032 for every 32-byte
// Ed25519 quantity - see note 3 in the file header.
void reverse32(uint8_t *data) {
  for (size_t i = 0; i < 16; ++i) {
    const uint8_t swap = data[i];
    data[i] = data[31 - i];
    data[31 - i] = swap;
  }
}

// One command, one response body. `stage` names this command for the log.
//
// `budgetMs` is how long the transport may wait for this particular command.
// The default suits the ones the part answers immediately - an object lookup,
// a read, a delete. The two that do public-key arithmetic pass
// se050_t1::SLOW_BUDGET_MS instead: the part stalls those with WTX blocks while
// it works, and abandoning an exchange part-way is precisely what desynchronises
// the T=1 sequence counters and poisons every command after it.
bool command(const char *stage,
             const uint8_t *apdu,
             size_t apduLength,
             uint8_t *body,
             size_t bodyCapacity,
             size_t &bodyLength,
             uint32_t budgetMs = se050_t1::BUDGET_MS) {
  uint16_t statusWord = 0;
  if (!se050_t1::transceive(apdu, apduLength, body, bodyCapacity, bodyLength, statusWord,
                            budgetMs)) {
    badge_log::tagf("se050", "%s: transport failed (%s)", stage, se050_t1::lastError());
    fail("t1", 0);
    return false;
  }
  sStatusWord = statusWord;
  if (statusWord != SW_OK) {
    badge_log::tagf("se050", "%s refused, sw %04x", stage, statusWord);
    fail(stage, statusWord);
    return false;
  }
  return true;
}

}  // namespace

bool begin() {
  sStatusWord = 0;
  sStage = "";
  if (!se050_t1::begin()) {
    badge_log::tagf("se050", "link/select failed (%s)", se050_t1::lastError());
    fail("select", 0);
    return false;
  }
  return true;
}

bool ready() { return se050_t1::ready(); }
uint16_t lastStatusWord() { return sStatusWord; }
const char *lastStage() { return sStage; }

bool objectExists(uint32_t objectId, bool &exists) {
  exists = false;

  // [AN] CheckObjectExists, [PNT] Se05x_API_CheckObjectExists:
  // 80 04 00 27, TLV_1 = object id. Answers TLV_1 = 0x01 present, 0x02 absent.
  uint8_t data[6];
  const size_t dataLength = tlvU32(data, TAG_1, objectId);
  uint8_t apdu[16];
  const size_t apduLength = buildApdu(apdu, INS_MGMT, P1_DEFAULT, P2_EXIST, data, dataLength);

  uint8_t body[16];
  size_t bodyLength = 0;
  if (!command("exists", apdu, apduLength, body, sizeof(body), bodyLength)) return false;

  const uint8_t *value = nullptr;
  size_t valueLength = 0;
  if (!tlvFirst(body, bodyLength, TAG_1, &value, &valueLength) || valueLength != 1) {
    badge_log::tagf("se050", "exists: unparseable answer, %u bytes", (unsigned)bodyLength);
    fail("exists", sStatusWord);
    return false;
  }
  exists = (value[0] == RESULT_SUCCESS);
  return true;
}

bool deleteObject(uint32_t objectId) {
  // [AN] DeleteSecureObject, [PNT] Se05x_API_DeleteSecureObject:
  // 80 04 00 28, TLV_1 = object id.
  uint8_t data[6];
  const size_t dataLength = tlvU32(data, TAG_1, objectId);
  uint8_t apdu[16];
  const size_t apduLength = buildApdu(apdu, INS_MGMT, P1_DEFAULT, P2_DELETE_OBJECT, data, dataLength);

  uint8_t body[16];
  size_t bodyLength = 0;
  return command("delete", apdu, apduLength, body, sizeof(body), bodyLength);
}

bool generateEd25519Pair(uint32_t objectId) {
  // [AN] WriteECKey, [PNT] Se05x_API_WriteECKey with key_part = KeyPart_Pair:
  // 80 01 61 00, TLV_1 = object id, TLV_2 = curve. Leaving out TLV_3 (private
  // key) and TLV_4 (public key) is what turns "import this key" into "generate
  // one". Ed25519 is a curve the applet always has, so unlike the Weierstrass
  // curves there is no CreateECCurve to do first ([PNT]
  // sss_se05x_create_curve_if_needed returns early for it).
  uint8_t data[16];
  size_t dataLength = tlvU32(data, TAG_1, objectId);
  dataLength += tlvU8(data + dataLength, TAG_2, ECCURVE_ED25519);

  uint8_t apdu[32];
  const size_t apduLength = buildApdu(apdu, INS_WRITE, (uint8_t)(P1_EC | P1_KEY_PAIR), P2_DEFAULT, data, dataLength);

  uint8_t body[16];
  size_t bodyLength = 0;
  if (command("generate", apdu, apduLength, body, sizeof(body), bodyLength,
              se050_t1::SLOW_BUDGET_MS)) {
    return true;
  }

  // A refusal is final; a transport timeout is not. SLOW_BUDGET_MS above should
  // already cover generation, so reaching here means the estimate was wrong -
  // but the key may still have landed in the part, and asking is cheaper than
  // discarding a working secure-element identity over a timing guess. This is
  // the safety net, not the mechanism; if the log below ever appears on real
  // hardware, raise SLOW_BUDGET_MS rather than leaning on these rechecks.
  if (sStatusWord != 0) return false;

  for (uint8_t attempt = 0; attempt < GENERATE_RECHECKS; ++attempt) {
    delay(GENERATE_RECHECK_MS);
    bool exists = false;
    if (objectExists(objectId, exists) && exists) {
      badge_log::tagf("se050", "generate outran the transport budget but landed");
      return true;
    }
  }
  fail("generate", 0);
  return false;
}

bool readEd25519PublicKey(uint32_t objectId, uint8_t publicKey[32]) {
  // [AN] ReadObject, [PNT] Se05x_API_ReadObject: 80 02 00 00, TLV_1 = object
  // id, offset and length omitted. On an EC key pair this returns the public
  // half only - the private one is not readable by anyone, which is the whole
  // point of the part.
  uint8_t data[6];
  const size_t dataLength = tlvU32(data, TAG_1, objectId);
  uint8_t apdu[16];
  const size_t apduLength = buildApdu(apdu, INS_READ, P1_DEFAULT, P2_DEFAULT, data, dataLength);

  uint8_t body[80];
  size_t bodyLength = 0;
  if (!command("read", apdu, apduLength, body, sizeof(body), bodyLength)) return false;

  const uint8_t *value = nullptr;
  size_t valueLength = 0;
  if (!tlvFirst(body, bodyLength, TAG_1, &value, &valueLength) || valueLength != 32) {
    badge_log::tagf("se050", "public key was %u bytes, wanted 32", (unsigned)valueLength);
    fail("read", sStatusWord);
    return false;
  }

  memcpy(publicKey, value, 32);
  reverse32(publicKey);
  return true;
}

bool signEd25519(uint32_t objectId, const uint8_t *message, size_t length, uint8_t signature[64]) {
  if (!message || length == 0 || length > MAX_SIGN_MESSAGE_BYTES) {
    fail("sign", 0);
    return false;
  }

  // [AN] EdDSASign, [PNT] Se05x_API_EdDSASign: 80 03 0C 09, TLV_1 = object id,
  // TLV_2 = algorithm, TLV_3 = the message itself. Not a digest - Ed25519 is
  // not prehashed, and the part does the SHA-512 internally.
  uint8_t data[MAX_APDU];
  size_t dataLength = tlvU32(data, TAG_1, objectId);
  dataLength += tlvU8(data + dataLength, TAG_2, EDSIG_ED25519PURE_SHA512);
  dataLength += tlvBuf(data + dataLength, TAG_3, message, length);
  if (dataLength > 0xFE) {  // would need a three-byte Lc, which we do not build
    fail("sign", 0);
    return false;
  }

  uint8_t apdu[MAX_APDU + 8];
  const size_t apduLength = buildApdu(apdu, INS_CRYPTO, P1_SIGNATURE, P2_SIGN, data, dataLength);

  uint8_t body[96];
  size_t bodyLength = 0;
  if (!command("sign", apdu, apduLength, body, sizeof(body), bodyLength,
               se050_t1::SLOW_BUDGET_MS)) {
    return false;
  }

  const uint8_t *value = nullptr;
  size_t valueLength = 0;
  if (!tlvFirst(body, bodyLength, TAG_1, &value, &valueLength) || valueLength != 64) {
    badge_log::tagf("se050", "signature was %u bytes, wanted 64", (unsigned)valueLength);
    fail("sign", sStatusWord);
    return false;
  }

  memcpy(signature, value, 64);
  reverse32(signature);       // R
  reverse32(signature + 32);  // S
  return true;
}

}  // namespace se050_apdu
