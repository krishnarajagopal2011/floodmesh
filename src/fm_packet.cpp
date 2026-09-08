/**
 * FloodMesh - frame header parse/build. The airlock between the antenna and
 * the rest of the firmware.
 *
 * Every byte this file sees arrived over an unauthenticated broadcast medium
 * that anyone with a $20 radio can write to. Nothing downstream re-checks the
 * common prefix, so a field that leaves fmParseHeader() is treated as gospel:
 * the channel indexes arrays, the fragment index picks a 192-byte slot inside
 * a 1500-byte clip, and the call sign is handed to %s. Each of those is a
 * memory-safety bug waiting for a malformed frame. Hence: validate everything
 * up front, fill *out only after every check has passed, and never return
 * "true, but".
 *
 * Reject table - fmParseHeader() returns false and the frame is dropped
 * ---------------------------------------------------------------------------
 *   condition                              why it must not get through
 *   -------------------------------------  ----------------------------------
 *   buf == NULL || out == NULL             caller bug; fail loud-ish, not crash
 *   len < FM_OFF_COMMON_END (11)           prefix truncated; fields would be
 *                                          read past the end of the buffer
 *   (buf[0] & 0xF0) != 0xF0                not FloodMesh, or a version whose
 *                                          layout we do not know
 *   type not ALARM/VOICE                   unknown layer; length rules below
 *                                          would not apply
 *   hopLimit > FM_HOP_MAX (7)              hop count is the only unauthenticated
 *                                          field - an attacker sets it, so it
 *                                          is the one an attacker uses to turn
 *                                          the mesh into an amplifier
 *   channel >= FM_CH_COUNT (4)             goes straight into array subscripts
 *   call sign byte outside A-Z 0-9 space   printed with %s and shown on the
 *                                          OLED; also stops control bytes and
 *                                          UTF-8 lead bytes reaching the screen
 *   ALARM && len < FM_ALARM_LEN (24)       counter or MAC truncated; fm_auth
 *                                          would MAC uninitialised stack
 *   VOICE && len < FM_VOICE_HDR_LEN (12)   fragment info byte missing
 *   VOICE && total != FM_FRAG_COUNT (8)    not our fragmentation scheme
 *   VOICE && idx >= total                  fmFragOffset(idx) would land outside
 *                                          the 1500-byte reassembly buffer
 *   VOICE && len-12 != fmFragBytes(idx)    a short fragment leaves a hole the
 *                                          decoder reads as speech; a long one
 *                                          overruns the slot. Exact or nothing.
 *
 * Deliberately NOT rejected here
 * -----------------------------
 *   hopLimit == 0     A frame that has run out of hops is still a valid frame
 *                     to display and dedup; it simply must not be relayed.
 *                     That decision belongs to the relay logic, not the parser.
 *   trailing bytes    An ALARM longer than 24 bytes is accepted (the spec says
 *                     len >= FM_ALARM_LEN) and the extra bytes are ignored by
 *                     everything downstream, which reads fixed offsets. VOICE
 *                     has no such slack because its length check is exact.
 *   the MAC           Authentication is fm_auth's job and needs the PSK. This
 *                     file only guarantees the frame is well FORMED, never that
 *                     it is genuine. Do not treat a true return as trust.
 *
 * On the parsed call sign: the 5 on-air bytes are copied verbatim and a NUL is
 * appended. Trailing pad spaces are NOT trimmed, so out->callSign compares byte
 * for byte with what was transmitted - dedup keys and display strings stay the
 * same string. Trim at the point of display if it ever looks untidy.
 *
 * fmWriteHeader() normalises by default (FM_WRITE_NORMALISE) so that anything
 * this file emits is something this file would accept. A lower-cased call sign
 * out of NVS or an off-by-one hop count then costs a fixup instead of an
 * invisible frame that every receiver in the colony silently drops.
 *
 * No millis(), no state, no allocation: this module is pure and safe to call
 * from any context, including inside the radio ISR's deferred handler.
 */
#include "fm_packet.h"

#include <stdio.h>

// ---------------------------------------------------------------- tunables
#ifndef FM_WRITE_NORMALISE
#define FM_WRITE_NORMALISE 1   // 1 = clamp hop/channel and fix up the call sign
#endif                         // so fmWriteHeader output always parses.
                               // 0 = write exactly what the caller passed, and
                               // let a bad frame go out to prove the point.

// ---------------------------------------------------------------- checks
static_assert(sizeof(FmHeader::callSign) == FM_CALLSIGN_LEN + 1,
              "FmHeader::callSign must hold the 5 on-air bytes plus a NUL");
static_assert(FM_OFF_MSGID + 1 == FM_OFF_COMMON_END,
              "msgId must be the last field of the common prefix");
static_assert(FM_OFF_CALLSIGN + FM_CALLSIGN_LEN == FM_OFF_CHANNEL,
              "call sign must run right up to the channel byte");
static_assert(FM_CALLSIGN_LEN == 5,
              "the \"%-5.5s\" pad format in fmWriteHeader hardcodes 5");

namespace {

/** The on-air call sign alphabet. Everything else is a malformed frame. */
bool callSignByteOk(uint8_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ';
}

}  // namespace

// ---------------------------------------------------------------- API
bool fmParseHeader(const uint8_t *buf, size_t len, FmHeader *out) {
  if (buf == nullptr || out == nullptr) return false;
  if (len < (size_t)FM_OFF_COMMON_END) return false;

  // Byte 0 carries the magic in the high nibble and the type in the low one,
  // so a single read rejects both foreign traffic and unknown frame types.
  if (!fmMagicOk(buf)) return false;
  const uint8_t ver = buf[FM_OFF_VER];

  const uint8_t type = fmFrameType(buf);
  if (type != FM_TYPE_ALARM && type != FM_TYPE_VOICE) return false;

  const uint8_t hopLimit = buf[FM_OFF_HOP];
  if (hopLimit > FM_HOP_MAX) return false;

  const uint8_t channel = buf[FM_OFF_CHANNEL];
  if (channel >= FM_CH_COUNT) return false;

  for (uint8_t i = 0; i < FM_CALLSIGN_LEN; i++) {
    if (!callSignByteOk(buf[FM_OFF_CALLSIGN + i])) return false;
  }

  // Per-type length rules. Both are checked before a single byte reaches the
  // caller, because a half-populated FmHeader is exactly the thing a caller
  // forgets to guard against after a false return.
  if (type == FM_TYPE_ALARM) {
    if (len < (size_t)FM_ALARM_LEN) return false;
  } else {
    if (len < (size_t)FM_VOICE_HDR_LEN) return false;

    const uint8_t info  = buf[FM_OFF_FRAGINFO];
    const uint8_t idx   = fmFragIdx(info);
    const uint8_t total = fmFragTotal(info);
    if (total != FM_FRAG_COUNT) return false;
    if (idx >= total) return false;

    // Exact, not "at least": fmFragOffset(idx) + fmFragBytes(idx) is the only
    // window this fragment may write into the reassembly buffer, and a sender
    // that disagrees about the size is either broken or probing.
    if ((len - (size_t)FM_VOICE_HDR_LEN) != (size_t)fmFragBytes(idx)) {
      return false;
    }
  }

  out->ver      = ver;
  out->type     = type;
  out->hopLimit = hopLimit;
  memcpy(out->callSign, buf + FM_OFF_CALLSIGN, FM_CALLSIGN_LEN);
  out->callSign[FM_CALLSIGN_LEN] = '\0';
  out->channel  = channel;
  out->msgId    = buf[FM_OFF_MSGID];
  return true;
}

size_t fmWriteHeader(uint8_t *buf, uint8_t type, uint8_t hopLimit,
                     const char *callSign, uint8_t channel, uint8_t msgId) {
  if (buf == nullptr) return 0;

  // "%-5.5s" is the entire padding rule: left justified, space filled up to 5,
  // hard truncated at 5. It is built in a local because snprintf always appends
  // a NUL and the frame has no room for one - only the first FM_CALLSIGN_LEN
  // bytes are ever copied out. The width is a literal rather than "%-*.*s" so
  // that -Wformat can check it and so it stays on newlib-nano's common path;
  // the static_assert above keeps it in step with FM_CALLSIGN_LEN.
  char cs[FM_CALLSIGN_LEN + 1];
  snprintf(cs, sizeof(cs), "%-5.5s", callSign == nullptr ? "" : callSign);

  uint8_t hop = hopLimit;
  uint8_t ch  = channel;

#if FM_WRITE_NORMALISE
  if (hop > FM_HOP_MAX) hop = FM_HOP_MAX;
  if (ch >= FM_CH_COUNT) ch = (uint8_t)(FM_CH_COUNT - 1);
  for (uint8_t i = 0; i < FM_CALLSIGN_LEN; i++) {
    uint8_t c = (uint8_t)cs[i];
    if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
    // A space is the only safe substitute: it is in the alphabet, so the frame
    // still parses, and it reads on the OLED as a gap rather than as a
    // different station.
    cs[i] = callSignByteOk(c) ? (char)c : ' ';
  }
#endif

  buf[FM_OFF_VER] = fmVerByte(type);
  buf[FM_OFF_HOP] = hop;
  memcpy(buf + FM_OFF_CALLSIGN, cs, FM_CALLSIGN_LEN);
  buf[FM_OFF_CHANNEL] = ch;
  buf[FM_OFF_MSGID] = msgId;
  return (size_t)FM_OFF_COMMON_END;
}
