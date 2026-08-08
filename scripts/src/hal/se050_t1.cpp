#include "se050_t1.h"

#include <Wire.h>

#include "../badge_log.h"
#include "badge_i2c.h"

namespace se050_t1 {
namespace {

// -- Block layer constants ---------------------------------------------------
// Values from NXP's own middleware; see the SOURCES block in se050_t1.h for the
// repositories and commits. The soft reset is additionally confirmed by the one
// frame this board is already known to answer.

// [NANO] phNxpEseProto7816_3.h: SEND_PACKET_SOF 0x5A, RECIEVE_PACKET_SOF 0xA5.
constexpr uint8_t NAD_HOST_TO_SE = 0x5A;
constexpr uint8_t NAD_SE_TO_HOST = 0xA5;

// PCB, decoded the way [NANO] phNxpEseProto7816_3.c decodes it:
//   I-block  !(pcb & 0x80)              bit 6 = N(S), bit 5 = chaining
//   R-block   (pcb & 0x80) && !(0x40)   bit 4 = N(R), bits 0-1 = error
//   S-block   (pcb & 0x80) &&  (0x40)   0xC0 | code request, 0xE0 | code response
constexpr uint8_t PCB_TYPE_MASK  = 0xC0;
constexpr uint8_t PCB_I_BLOCK    = 0x00;
constexpr uint8_t PCB_I_SEQ      = 0x40;
constexpr uint8_t PCB_I_MORE     = 0x20;
constexpr uint8_t PCB_R_BLOCK    = 0x80;
constexpr uint8_t PCB_R_SEQ      = 0x10;
constexpr uint8_t PCB_R_ERR_MASK = 0x03;
constexpr uint8_t PCB_S_BLOCK    = 0xC0;
constexpr uint8_t PCB_S_RESPONSE = 0x20;
// NXP masks the S-block code with 0x3F, which collides with the request/response
// bit at 0x20. 0x1F is the mask that actually separates the two fields, and it
// agrees with NXP on every code this transport uses.
constexpr uint8_t PCB_S_CODE     = 0x1F;

// [NANO] phNxpEseProto7816_3.h, sFrameTypes and PH_PROTO_7816_S_*.
constexpr uint8_t S_CODE_WTX        = 0x03;  // request 0xC3, response 0xE3
constexpr uint8_t S_CODE_SOFT_RESET = 0x0F;  // request 0xCF, response 0xEF

// NXP's WTX acknowledgement is LEN=1 INF=0x01 - a fixed byte, not an echo of
// the request. [NANO] phNxpEseProto7816_SendSFrame, WTX_RSP case.
constexpr uint8_t WTX_RESPONSE_INF = 0x01;

// A whole block: NAD+PCB+LEN + INF + CRC.
constexpr size_t MAX_BLOCK = 3 + MAX_INF + 2;
static_assert(MAX_BLOCK <= 128, "a block must fit Arduino-ESP32's Wire buffer, "
                                "which clamps over-long transfers silently");

// The SE050 NACKs its own I2C address when it has nothing to send yet. That is
// the part saying "not ready", not a fault, and treating the first NACK as a
// failure is the classic way to make this link look broken when it is fine.
// NXP polls every 1 ms for up to 500 tries ([NANO] ESE_NAD_POLLING_MAX); we poll
// at a similar rate but let the wall-clock budget end it, because a Lua callback
// cannot wait half a second.
constexpr uint8_t  READ_POLL_ATTEMPTS = 48;
constexpr uint32_t READ_POLL_DELAY_MS = 2;

bool sReady = false;
uint8_t sSeqHost = 0;  // our N(S)
uint8_t sSeqSe = 0;    // the SE's N(S), as we last saw it

uint8_t sCip[MAX_INF];
size_t sCipLength = 0;

const char *sError = "ok";
uint32_t sDeadline = 0;

void fail(const char *reason) { sError = reason; }

bool budgetLeft() { return (int32_t)(millis() - sDeadline) < 0; }

// -- Framing -----------------------------------------------------------------

// Writes one block. Returns false on an I2C-level failure.
bool writeBlock(uint8_t pcb, const uint8_t *inf, size_t infLength) {
  if (infLength > MAX_INF) {
    fail("block too long");
    return false;
  }

  static uint8_t frame[MAX_BLOCK];
  frame[0] = NAD_HOST_TO_SE;
  frame[1] = pcb;
  frame[2] = (uint8_t)infLength;
  if (infLength > 0) memcpy(frame + 3, inf, infLength);

  const uint16_t crc = crc16(frame, 3 + infLength);
  frame[3 + infLength] = (uint8_t)(crc & 0xFF);          // low byte first
  frame[4 + infLength] = (uint8_t)((crc >> 8) & 0xFF);

  Wire.setTimeOut(SE050_TIMEOUT_MS);
  Wire.beginTransmission(SE050_ADDR);
  Wire.write(frame, 5 + infLength);
  const uint8_t result = Wire.endTransmission(true);
  Wire.setTimeOut(I2C_TIMEOUT_MS);

  if (result != 0) {
    badge_i2c::noteFailure(result);
    fail("no ack on write");
    return false;
  }
  return true;
}

// Reads one block, header first because LEN is not known until it arrives.
// Verifies NAD and CRC; a block that fails either is discarded.
bool readBlock(uint8_t &pcb, uint8_t *inf, size_t infCapacity, size_t &infLength) {
  uint8_t header[3];

  bool gotHeader = false;
  for (uint8_t attempt = 0; attempt < READ_POLL_ATTEMPTS && budgetLeft(); ++attempt) {
    Wire.setTimeOut(SE050_TIMEOUT_MS);
    const size_t received = Wire.requestFrom((int)SE050_ADDR, 3, true);
    size_t index = 0;
    while (Wire.available() && index < 3) header[index++] = (uint8_t)Wire.read();
    while (Wire.available()) Wire.read();
    Wire.setTimeOut(I2C_TIMEOUT_MS);

    // 0xFF throughout is the part saying "not yet" rather than answering, which
    // is not the same as a NACK and must not be treated as a hard failure.
    if (received == 3 && index == 3 && header[0] == NAD_SE_TO_HOST) {
      gotHeader = true;
      break;
    }
    delay(READ_POLL_DELAY_MS);
  }

  if (!gotHeader) {
    badge_i2c::noteFailure(2);
    fail("no response block");
    return false;
  }

  const size_t length = header[2];
  if (length > infCapacity || length > MAX_INF) {
    // Deliberately a hard failure. Wire would clamp the read to its buffer and
    // hand back a short block, and a short block whose CRC happens to be absent
    // is far more confusing than an explicit refusal.
    fail("response block larger than the I2C buffer");
    return false;
  }

  // Static rather than automatic: this layer only ever runs on the main loop,
  // and ~800 bytes of transient stack inside a Lua binding is not free there.
  static uint8_t tail[MAX_INF + 2];
  Wire.setTimeOut(SE050_TIMEOUT_MS);
  const size_t received = Wire.requestFrom((int)SE050_ADDR, (int)(length + 2), true);
  size_t index = 0;
  while (Wire.available() && index < length + 2) tail[index++] = (uint8_t)Wire.read();
  while (Wire.available()) Wire.read();
  Wire.setTimeOut(I2C_TIMEOUT_MS);

  if (received != length + 2 || index != length + 2) {
    badge_i2c::noteFailure(4);
    fail("truncated response block");
    return false;
  }

  // CRC covers NAD..INF. Rebuild that span contiguously to check it.
  static uint8_t frame[MAX_BLOCK];
  memcpy(frame, header, 3);
  if (length > 0) memcpy(frame + 3, tail, length);
  const uint16_t expected = crc16(frame, 3 + length);
  const uint16_t actual = (uint16_t)tail[length] | ((uint16_t)tail[length + 1] << 8);
  if (expected != actual) {
    fail("bad CRC on response");
    return false;
  }

  badge_i2c::noteSuccess();
  pcb = header[1];
  infLength = length;
  if (length > 0) memcpy(inf, tail, length);
  return true;
}

// One write-then-read round trip.
bool exchange(uint8_t pcb, const uint8_t *inf, size_t infLength, uint8_t &replyPcb,
              uint8_t *reply, size_t replyCapacity, size_t &replyLength) {
  if (!writeBlock(pcb, inf, infLength)) return false;
  delay(TURNAROUND_MS);
  return readBlock(replyPcb, reply, replyCapacity, replyLength);
}

// -- Handshake ---------------------------------------------------------------

bool softReset() {
  uint8_t pcb = 0;
  size_t length = 0;
  if (!exchange((uint8_t)(PCB_S_BLOCK | S_CODE_SOFT_RESET), nullptr, 0, pcb, sCip,
                sizeof(sCip), length)) {
    return false;
  }

  // Expect the S-block *response* to the soft reset, carrying the CIP.
  if ((pcb & PCB_TYPE_MASK) != PCB_S_BLOCK || (pcb & PCB_S_RESPONSE) == 0 ||
      (pcb & PCB_S_CODE) != S_CODE_SOFT_RESET) {
    fail("soft reset not acknowledged");
    return false;
  }

  sCipLength = length;
  sSeqHost = 0;
  sSeqSe = 0;
  return true;
}

/*
  SE05x IoT applet SELECT. VERIFIED.

  The AID is APPLET_NAME in [NANO] lib/apdu/se05x_APDU_impl.c, and AN12413
  section 2.3 states the same 16 bytes in prose: "The instance AID for SE050 IoT
  applet - pre-provisioned by NXP - is A0000003965453000000010300000000."

  The SELECT is ordinary ISO 7816-4 - CLA=00 INS=A4 P1=04 (select by DF name)
  P2=00 (first or only occurrence), Lc, AID, Le=00 - and matches both [NANO]'s
  construction and [AN] Table 51 field for field.

  The whole block, CRC included, was recomputed and matches NXP's:
    5A 00 16 00 A4 04 00 10 A0 00 00 03 96 54 53 00 00 00 01 03 00 00 00 00 00 A8 C8

  The applet answers with a 7-byte VersionInfo (major, minor, patch, 2-byte
  AppletConfig, 2-byte SecureBox version) and 90 00 - raw, not TLV-wrapped
  ([AN] Table 52 and section 4.3.33). We do not currently look at it.
*/
constexpr uint8_t APPLET_AID[] = {0xA0, 0x00, 0x00, 0x03, 0x96, 0x54, 0x53,
                                  0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00,
                                  0x00, 0x00};

bool selectApplet();

}  // namespace

uint16_t crc16(const uint8_t *data, size_t length) {
  // CRC-16/X-25: reflected polynomial 0x8408, init 0xFFFF, final XOR 0xFFFF.
  //
  // Identified, not assumed. The soft-reset block this board is known to answer
  // ends in 37 7F, and X-25 over {5A CF 00} is 0x7F37 - emitted low byte first,
  // that is exactly those two bytes. None of KERMIT, MCRF4XX, CCITT-FALSE or
  // XMODEM produce it. See the CONFIDENCE section in the header.
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
  }
  return (uint16_t)(crc ^ 0xFFFF);
}

bool transceive(const uint8_t *apdu, size_t apduLen, uint8_t *response,
                size_t responseCapacity, size_t &responseLen, uint16_t &statusWord,
                uint32_t budgetMs) {
  responseLen = 0;
  statusWord = 0;

  // begin() always runs on the short budget - the handshake is quick even when
  // the command that follows is not - and the caller's budget starts afterwards
  // so a slow command gets its full allowance rather than what the handshake
  // left over.
  sDeadline = millis() + BUDGET_MS;
  if (!sReady && !begin()) return false;
  sDeadline = millis() + budgetMs;

  static uint8_t assembled[MAX_RESPONSE];
  size_t assembledLength = 0;

  // -- Send, chaining if the APDU does not fit one block --------------------
  size_t offset = 0;
  while (offset < apduLen) {
    const size_t chunk = (apduLen - offset > MAX_INF) ? MAX_INF : apduLen - offset;
    const bool more = (offset + chunk) < apduLen;
    const uint8_t pcb =
        (uint8_t)(PCB_I_BLOCK | (sSeqHost ? PCB_I_SEQ : 0) | (more ? PCB_I_MORE : 0));

    if (!writeBlock(pcb, apdu + offset, chunk)) {
      reset();
      return false;
    }
    sSeqHost ^= 1;
    offset += chunk;

    if (more) {
      // Every chained block but the last is answered with an R-block ack.
      delay(TURNAROUND_MS);
      uint8_t pcbIn = 0;
      size_t lengthIn = 0;
      static uint8_t scratch[MAX_INF];
      if (!readBlock(pcbIn, scratch, sizeof(scratch), lengthIn)) {
        reset();
        return false;
      }
      if ((pcbIn & 0xC0) != PCB_R_BLOCK || (pcbIn & PCB_R_ERR_MASK) != 0) {
        fail("chaining not acknowledged");
        reset();
        return false;
      }
    }
  }

  // -- Receive, following chaining and WTX ----------------------------------
  //
  // The wall-clock budget is the real bound here; the counter only stops a
  // pathological peer that answers WTX forever inside the budget. Key
  // generation on a SLOW_BUDGET_MS call can legitimately take many WTX rounds,
  // so it has to be generous.
  for (uint16_t guard = 0; guard < 512; ++guard) {
    if (!budgetLeft()) {
      fail("timed out");
      reset();
      return false;
    }

    delay(TURNAROUND_MS);
    uint8_t pcb = 0;
    size_t length = 0;
    static uint8_t inf[MAX_INF];
    if (!readBlock(pcb, inf, sizeof(inf), length)) {
      reset();
      return false;
    }

    if ((pcb & 0x80) == 0) {
      // I-block: response data.
      if (assembledLength + length > sizeof(assembled)) {
        fail("response too long");
        reset();
        return false;
      }
      memcpy(assembled + assembledLength, inf, length);
      assembledLength += length;
      sSeqSe = (pcb & PCB_I_SEQ) ? 1 : 0;

      if (pcb & PCB_I_MORE) {
        // Acknowledge and keep reading.
        const uint8_t ack = (uint8_t)(PCB_R_BLOCK | ((sSeqSe ^ 1) ? PCB_R_SEQ : 0));
        if (!writeBlock(ack, nullptr, 0)) {
          reset();
          return false;
        }
        continue;
      }
      break;
    }

    if ((pcb & PCB_TYPE_MASK) == PCB_S_BLOCK && (pcb & PCB_S_RESPONSE) == 0 &&
        (pcb & PCB_S_CODE) == S_CODE_WTX) {
      // Waiting time extension: the part is busy and is asking for more time.
      // The answer is the fixed byte 0x01, not an echo of the request - see
      // WTX_RESPONSE_INF. Granting it costs nothing, because our own wall-clock
      // budget still bounds the whole exchange; NXP instead waits up to 40 s
      // here, which no Lua callback could survive.
      const uint8_t grant = WTX_RESPONSE_INF;
      if (!writeBlock((uint8_t)(PCB_S_BLOCK | PCB_S_RESPONSE | S_CODE_WTX), &grant, 1)) {
        reset();
        return false;
      }
      continue;
    }

    fail("unexpected block type");
    reset();
    return false;
  }

  // -- Split off the status word --------------------------------------------
  if (assembledLength < 2) {
    fail("response has no status word");
    reset();
    return false;
  }
  statusWord = (uint16_t)((assembled[assembledLength - 2] << 8) |
                          assembled[assembledLength - 1]);
  const size_t bodyLength = assembledLength - 2;
  if (bodyLength > responseCapacity) {
    fail("response does not fit");
    return false;
  }
  if (bodyLength > 0) memcpy(response, assembled, bodyLength);
  responseLen = bodyLength;
  sError = "ok";
  return true;
}

namespace {

bool selectApplet() {
  uint8_t apdu[6 + sizeof(APPLET_AID)];
  apdu[0] = 0x00;                       // CLA
  apdu[1] = 0xA4;                       // INS SELECT
  apdu[2] = 0x04;                       // P1: select by DF name (AID)
  apdu[3] = 0x00;                       // P2: first or only occurrence
  apdu[4] = (uint8_t)sizeof(APPLET_AID);  // Lc
  memcpy(apdu + 5, APPLET_AID, sizeof(APPLET_AID));
  apdu[5 + sizeof(APPLET_AID)] = 0x00;  // Le: return whatever the applet offers

  static uint8_t response[MAX_RESPONSE];
  size_t responseLength = 0;
  uint16_t status = 0;

  // The link is up at this point; transceive() must not recurse into begin().
  sReady = true;
  const bool ok = transceive(apdu, sizeof(apdu), response, sizeof(response),
                             responseLength, status);
  sReady = false;

  if (!ok) return false;
  if (status != 0x9000) {
    // 0x6A82 here means the AID is wrong or the applet is not installed. That
    // is the expected symptom if the UNVERIFIED AID above is incorrect.
    badge_log::tagf("se050", "applet select refused: SW=%04X", status);
    fail("applet select refused");
    return false;
  }
  return true;
}

}  // namespace

bool begin() {
  if (sReady) return true;

  sDeadline = millis() + BUDGET_MS;
  sError = "ok";

  if (!softReset()) return false;
  if (!selectApplet()) return false;

  sReady = true;
  badge_log::tagf("se050", "T=1 link up, applet selected (CIP %u bytes)",
                  (unsigned)sCipLength);
  return true;
}

bool ready() { return sReady; }

void reset() {
  sReady = false;
  sSeqHost = 0;
  sSeqSe = 0;
}

const uint8_t *atr() { return sCip; }
size_t atrLength() { return sCipLength; }
const char *lastError() { return sError; }

}  // namespace se050_t1
