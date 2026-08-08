/*
  SE05x T=1' (T=1 over I2C) transport.

  A standalone block-level link layer for the NXP SE050 on the shared I2C bus.
  It knows how to frame a block, checksum it, get it to the part, get a reply
  back, and hand up a bare APDU response plus its status word. It knows nothing
  about what the APDUs mean - GetRandom, key generation and signing all sit on
  top of transceive() and are somebody else's problem.

  Two callers are expected: se050::randomBytes() and the Ed25519 identity in
  src/identity/. Sharing one transport is the point; two independent T=1 stacks
  fighting over the same bus would be a bad time.


  ---------------------------------------------------------------- CONFIDENCE

  Read this before trusting anything below it. The layers are NOT equally well
  established, and the difference matters when you are debugging on hardware.

  SOURCES. Everything below was checked against NXP's own Apache-2.0 host
  middleware and against AN12413, rather than reconstructed from memory:

    [NANO]  github.com/NXPPlugNTrust/nano-package @ 26d55b4 (branch master)
            lib/t1oi2c/phNxpEseProto7816_3.{h,c}, lib/t1oi2c/phNxpEse_Api.c,
            lib/t1oi2c/phNxpEsePal_i2c.{h,c}, lib/apdu/se05x_APDU_impl.c,
            lib/apdu/se05x_types.h, lib/platform/linux/sm_i2c.c
    [PNT]   github.com/NXP/plug-and-trust @ 1ddf8cf (branch master)
            hostlib/hostLib/inc/se05x_enums.h,
            hostlib/hostLib/se05x_03_xx_xx/se05x_APDU_impl.h
    [AN]    AN12413 "SE050 APDU Specification" Rev 2.12,
            nxp.com/docs/en/application-note/AN12413.pdf
    [DS]    SE050 datasheet Rev 3.8

  Note that UM11225, the T=1' spec proper, is NOT public - every nxp.com path
  for it 404s. So the block layer below rests on NXP's implementation of the
  spec rather than the spec text. In practice that is the stronger of the two
  anyway, but it is not the same claim.

  VERIFIED, and confirmed twice over - the block layer.
    The framing was first derived independently from the one frame this board
    is already known to answer:

        5A CF 00 37 7F        (firmware/testkit/testkit.ino, testSe050Link)

    Reading that as NAD=5A PCB=CF LEN=00 CRC=37 7F, the trailing two bytes are
    exactly CRC-16/X-25 over {5A CF 00}, emitted low byte first. All five common
    CRC-16 variants were computed; only X-25 produces 0x7F37. A wrong reading of
    the field layout could not have produced a matching checksum.

    [NANO] phNxpEseProto7816_3.h then confirms the same layout literally, and
    phNxpEseProto7816_ComputeCRC is X-25 with the same low-byte-first emission.
    Nine of the ten frames that source implies were recomputed here and matched,
    including the soft reset above, the SELECT, and GetRandom. (The tenth, an
    R-ACK, disagreed - but that one is quoted from a C array literal whose CRC
    bytes are placeholder zeroes filled in at runtime, so it was never a real
    frame. It is not evidence against anything.) The X-25 check value over
    "123456789" also comes out at the published 0x906E.

    PCB 0xCF decodes as S-block / request / code 0x0F = Interface Soft Reset,
    and the 35-byte reply this board returns is 3 header + 30 INF + 2 CRC: an
    S-block response (PCB 0xEF) carrying the 30-byte Chip Identification
    Pattern. The arithmetic only works with a one-byte LEN and a two-byte CRC.

  VERIFIED - the applet AID and the SELECT.
    AID A0 00 00 03 96 54 53 00 00 00 01 03 00 00 00 00 is APPLET_NAME in
    [NANO] se05x_APDU_impl.c, and AN12413 section 2.3 states the same 16 bytes
    in prose. The SELECT is 00 A4 04 00 10 <AID> 00, matching both [NANO]'s
    construction and [AN] Table 51. The applet answers with a 7-byte VersionInfo
    plus 90 00 ([AN] Table 52, section 4.3.33) - raw, not TLV-wrapped.

  VERIFIED ON PAPER, NEVER SEEN ON A BUS - GetRandom.
    See se050.cpp. The encoding is documented in [AN] Table 271 and matches
    Se05x_API_GetRandom in [PNT] byte for byte, so it is not a guess. But no
    real SE050 transaction log was available to any of this, so "the spec and
    two codebases agree" is the strongest claim on offer. It is not the same as
    "observed working". Watch the first exchange on hardware.

  ONE KNOWN DEVIATION FROM NXP, deliberate.
    [NANO] phNxpEseProto7816_Open does R-Sync (0xC0) then Get ATR (0xC7) on a
    normal open, and uses Interface Soft Reset (0xCF) only for recovery. This
    transport opens with 0xCF instead, because 0xCF is the frame this specific
    board is already known to answer and 0xCF also returns the ATR. If the open
    ever misbehaves, swapping in 0xC0 followed by 0xC7 is the first thing to
    try; both are implemented in terms of the same block layer.


  ------------------------------------------------------------------- FRAMING

    +-----+-----+-----+---------------+--------+--------+
    | NAD | PCB | LEN | INF[LEN]      | CRC lo | CRC hi |
    +-----+-----+-----+---------------+--------+--------+

    NAD    0x5A host -> SE, 0xA5 SE -> host.
    LEN    one byte, so at most 254 INF bytes per block. Longer APDUs are
           chained with the I-block "more data" bit.
    CRC    CRC-16/X-25 over NAD..INF inclusive, appended low byte first.

    PCB, standard ISO 7816-3:
      I-block   0b0nm00000   n = send sequence N(S), m = more-data
      R-block   0b100n00ee   n = receive sequence N(R), ee = error code
      S-block   0b11rccccc   r = 0 request / 1 response, ccccc = code
                             code 0x0F = interface soft reset

  A block is written with one Wire transaction and read back with another. The
  part needs a gap between them - the ATR path has used 15 ms since the test kit
  and that is kept here. Reads are two-phase: three header bytes, then LEN+2,
  because the length is not known until the header is in hand.


  -------------------------------------------------------- TIMING AND FAILURE

  Every entry point is bounded. transceive() gives up after its budget expires
  (retries included) and returns false rather than blocking; begin() is bounded
  the same way. This exists because a Lua callback has 250 ms to return, and a
  transport that can hang is worse than one that can fail.

  The budget is per call, not global. BUDGET_MS is the default and is sized for
  the commands the part answers straight away; key generation and signing pass
  SLOW_BUDGET_MS because the part legitimately stalls those with WTX blocks
  while it does the arithmetic. One number could not serve both: sized for
  GetRandom it fails key generation on principle, and sized for key generation
  it lets a dead bus eat a Lua frame budget several times over.

  Failure is always reported, never papered over. There is no fallback path in
  here and there must not be one: a caller asking the secure element for random
  bytes or a signature has to be able to tell whether it got them.
*/
#pragma once

#include <Arduino.h>

#include "../config.h"

namespace se050_t1 {

// Largest INF payload this transport puts in one block.
//
// The protocol allows 254 (LEN is a single byte), but Arduino-ESP32's Wire has
// a 128-byte buffer and its requestFrom() *silently clamps* an over-long read
// to that size rather than failing - which would hand us a truncated block that
// still looked plausible. So blocks are kept small enough that a whole one
// (3 header + INF + 2 CRC) fits, and longer APDUs are chained, which is exactly
// what T=1 chaining is for. Nothing this firmware sends comes close anyway.
//
// A response block bigger than the buffer is rejected outright rather than
// truncated - see readBlock(). If a future caller genuinely needs bigger single
// blocks, call Wire.setBufferSize() before badge_i2c::begin() and raise this.
constexpr size_t MAX_INF = 120;

// Largest APDU response this layer will assemble across chained blocks. An
// Ed25519 public key or signature is well under this; so is a GetRandom reply.
constexpr size_t MAX_RESPONSE = 512;

// Default wall-clock ceiling for one begin() or transceive(), retries included.
// Chosen against the runtime's 250 ms callback budget: a binding can extend the
// deadline by this much and still leave room for the rest of the frame.
//
// It is a default, not a limit - see the `budgetMs` argument on transceive().
// Commands that run a scalar multiply or write NVM inside the part take far
// longer than any interactive command, and holding them to a budget sized for
// GetRandom would fail them on principle rather than on evidence.
constexpr uint32_t BUDGET_MS = 120;

// Ceiling for a command the part answers with WTX while it does public-key
// arithmetic - key generation above all, which does a scalar multiply and then
// commits the result to NVM. NXP's own host stack waits far longer than this
// (tens of seconds); this is bounded much tighter because it runs from setup()
// and a badge that appears dead at boot is worse than one that falls back to a
// software key.
constexpr uint32_t SLOW_BUDGET_MS = 3000;

// Settle time between writing a block and reading the answer. 15 ms is what the
// ATR path in the test kit uses on this board.
constexpr uint32_t TURNAROUND_MS = 15;

// How many times a block exchange is retried before the transport gives up.
constexpr uint8_t MAX_RETRIES = 2;

// Soft-resets the part, reads its ATR/CIP, and selects the IoT applet.
//
// Safe to call repeatedly; a second call on a ready link is a no-op and costs
// nothing. Returns false and leaves ready() false if any step fails - including
// the applet select, which is the step most likely to fail on a part whose
// applet is absent or locked.
bool begin();

// True once begin() has completed and the applet answered. Everything above
// this layer should gate on it.
bool ready();

// Drops the cached state so the next begin() redoes the whole handshake. Called
// after an exchange fails badly enough that the sequence counters are suspect.
void reset();

// Sends one command APDU and returns its response.
//
//   apdu/apduLen        the complete command APDU, CLA INS P1 P2 [Lc data] [Le]
//   response            receives the response body WITHOUT the status word
//   responseCapacity    size of `response`
//   responseLen         set to the number of body bytes written
//   statusWord          set to SW1SW2, e.g. 0x9000
//   budgetMs            wall-clock ceiling for this one exchange. Callers that
//                       run inside a Lua callback must leave it at the default;
//                       key generation and signing pass SLOW_BUDGET_MS, because
//                       the part answers those with WTX for as long as the
//                       arithmetic takes and cutting it off mid-command is what
//                       leaves the sequence counters desynchronised.
//
// Returns true only when a well-formed response block came back and its CRC
// checked out. A card error (SW != 0x9000) still returns true with the status
// word set - that is a valid answer to a question, and only the caller knows
// whether it is acceptable. False means the transport failed: no reply, a bad
// checksum, a truncated frame, or the budget ran out. On false, nothing has
// been written to `response` that callers may rely on.
//
// Calls begin() implicitly if the link is not ready.
bool transceive(const uint8_t *apdu, size_t apduLen, uint8_t *response,
                size_t responseCapacity, size_t &responseLen, uint16_t &statusWord,
                uint32_t budgetMs = BUDGET_MS);

// The Chip Identification Pattern from the last soft reset - what the rest of
// the firmware has always called "the ATR". Valid while ready(); length is
// atrLength() and is 30 on this part.
const uint8_t *atr();
size_t atrLength();

// A short human-readable reason for the most recent failure, for the log and
// the Settings screen. Never null; "ok" when nothing has gone wrong.
const char *lastError();

// CRC-16/X-25 over `length` bytes. Exposed because it is independently
// checkable against the known-good soft-reset frame, which is the only reason
// the framing above can be called verified. See se050_t1.cpp for that check.
uint16_t crc16(const uint8_t *data, size_t length);

}  // namespace se050_t1
