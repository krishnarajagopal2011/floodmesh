/**
 * FloodMesh - on-air frame formats.
 *
 * Fixed-offset binary layouts, hand-packed. No protobufs, no serialisation
 * library: every byte on air is accounted for here, and the sizes are checked
 * at compile time against the offsets below.
 *
 * Multi-byte fields are LITTLE ENDIAN, matching the ESP32-S3, so nothing is
 * swapped on this platform. A port to a big-endian MCU must add explicit
 * conversion - do not memcpy structs onto the air.
 *
 * ---------------------------------------------------------------------------
 * Common prefix - 9 bytes, shared by both frame types
 * ---------------------------------------------------------------------------
 *   off size field       notes
 *   --- ---- ----------- -------------------------------------------------------
 *   0   1    ver+type    high nibble 0xF = FloodMesh magic, low nibble = type.
 *                        0xF1 alarm, 0xF2 voice. A protocol revision changes
 *                        the whole byte (0xE1/0xE2), so version and type cost
 *                        one byte between them rather than two.
 *   1   1    hopLimit    DECREMENTED BY EVERY RELAY. Not authenticated - see below.
 *   2   5    callSign    originator, 'A'-'Z'/'0'-'9', space padded, NO NUL
 *   7   1    channel     0..3
 *   8   1    msgId       per-sender rolling counter
 *   9   1    (type-specific: alarmType for alarms, fragIdx|totalFrags for voice)
 *
 * A one-byte msgId is enough here, and that is worth showing rather than
 * asserting. The regulatory duty cycle caps a node at 2.5% of an hour, which is
 * about 35 voice notes per hour; wrapping 256 message ids therefore takes on the
 * order of seven hours, while the dedup ring forgets an entry after ten minutes.
 * The wrap can never catch up with the memory, so a second byte would buy
 * nothing but airtime.
 *
 * Why hopLimit sits outside the authenticated region
 * --------------------------------------------------
 * A relay must decrement the hop limit, so it cannot be covered by the
 * originator's MAC - the MAC would break at the first hop. This is not a
 * FloodMesh quirk; every authenticated mesh has the same hole. The consequence
 * is bounded and worth stating plainly: an attacker can change how FAR an alarm
 * travels, but cannot forge one, alter which alarm it is, or replay an old one.
 * Everything that decides meaning is authenticated; only reach is malleable.
 *
 * ---------------------------------------------------------------------------
 * Layer 1 alarm - 22 bytes, PSK-authenticated
 * ---------------------------------------------------------------------------
 *   9   1    alarmType   FM_ALARM_* (0..3)
 *   10  4    counter     monotonic per-sender, anti-replay
 *   14  8    mac         HMAC-SHA256(PSK, authenticated region) truncated
 *
 * The authenticated region is deliberately NOT contiguous: bytes [0] + [2..13],
 * skipping byte 1 (hopLimit). fm_auth builds that scratch buffer; nothing else
 * should try to MAC the frame directly.
 *
 * ---------------------------------------------------------------------------
 * Layer 2 voice fragment - 10-byte header + up to 192 bytes of payload
 * ---------------------------------------------------------------------------
 *   9   1    fragIdx:4 | totalFrags:4   (high nibble = index)
 *   10  N    payload     whole Codec2 frames, never a partial one
 *
 * Voice is not authenticated. Eight MACs would cost 64 bytes of airtime on a
 * best-effort stream that yields to alarms anyway, and a forged voice note is a
 * nuisance where a forged EVACUATE is a catastrophe. The PSK protects what
 * matters.
 *
 * ---------------------------------------------------------------------------
 * Fragmentation arithmetic (exact, no rounding anywhere)
 * ---------------------------------------------------------------------------
 *   Codec2 1200 bps = 48 bits = 6 bytes per 40 ms frame
 *   10 s / 40 ms                        = 250 frames
 *   250 frames x 6 B                    = 1500 bytes   <- the whole clip
 *   split 7 x 32 frames + 1 x 26 frames = 250 frames
 *   7 x 192 B + 1 x 156 B               = 1500 bytes   <- 8 fragments
 *
 * Splitting on frame boundaries rather than by byte count means a lost fragment
 * costs only its own 32 frames; the decoder stays in sync and can conceal the
 * gap. A byte-count split (1500/8 = 187.5) would cut a 6-byte codec frame in
 * half at every boundary.
 */
#pragma once
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------- constants
#define FM_PROTO_MAGIC      0xF0   // high nibble of byte 0; low nibble is the type
#define FM_TYPE_ALARM       0x01
#define FM_TYPE_VOICE       0x02

#define FM_CALLSIGN_LEN     5      // on air: no NUL terminator
#define FM_CH_COUNT         4

#define FM_HOP_DEFAULT      3      // enough for a colony; see docs/hardware-notes
#define FM_HOP_MAX          7      // sanity ceiling on anything received

// Codec2 1200 bps geometry
#define FM_C2_FRAME_BYTES   6
#define FM_C2_FRAME_MS      40
#define FM_VOICE_MAX_MS_    10000
#define FM_C2_FRAMES_TOTAL  (FM_VOICE_MAX_MS_ / FM_C2_FRAME_MS)          // 250
#define FM_CLIP_BYTES       (FM_C2_FRAMES_TOTAL * FM_C2_FRAME_BYTES)     // 1500

#define FM_FRAG_COUNT       8
#define FM_FRAG_FRAMES      32                                   // per fragment
#define FM_FRAG_PAYLOAD_MAX (FM_FRAG_FRAMES * FM_C2_FRAME_BYTES)  // 192
#define FM_FRAG_LAST_FRAMES (FM_C2_FRAMES_TOTAL - (FM_FRAG_COUNT - 1) * FM_FRAG_FRAMES)
#define FM_FRAG_LAST_BYTES  (FM_FRAG_LAST_FRAMES * FM_C2_FRAME_BYTES)    // 156

// ---------------------------------------------------------------- offsets
enum {
  FM_OFF_VER        = 0,   // magic nibble + type nibble
  FM_OFF_HOP        = 1,   // NOT authenticated - relays rewrite this
  FM_OFF_CALLSIGN   = 2,
  FM_OFF_CHANNEL    = 7,
  FM_OFF_MSGID      = 8,
  FM_OFF_COMMON_END = 9,

  // alarm
  FM_OFF_ALARMTYPE  = 9,
  FM_OFF_COUNTER    = 10,  // 4 bytes
  FM_OFF_MAC        = 14,  // 8 bytes
  FM_ALARM_LEN      = 22,

  // voice
  FM_OFF_FRAGINFO   = 9,
  FM_VOICE_HDR_LEN  = 10,
};

#define FM_MAC_LEN          8
#define FM_VOICE_LEN_MAX    (FM_VOICE_HDR_LEN + FM_FRAG_PAYLOAD_MAX)   // 202
#define FM_FRAME_LEN_MAX    FM_VOICE_LEN_MAX

// ---------------------------------------------------------------- checks
static_assert(FM_CLIP_BYTES == 1500, "10 s of Codec2 1200 must be exactly 1500 B");
static_assert((FM_FRAG_COUNT - 1) * FM_FRAG_FRAMES + FM_FRAG_LAST_FRAMES ==
                  FM_C2_FRAMES_TOTAL,
              "fragment frame counts must sum to exactly 250 Codec2 frames");
static_assert((FM_FRAG_COUNT - 1) * FM_FRAG_PAYLOAD_MAX + FM_FRAG_LAST_BYTES ==
                  FM_CLIP_BYTES,
              "fragment payloads must sum to exactly 1500 bytes");
static_assert(FM_FRAG_LAST_BYTES == 156, "last fragment must be 156 B");
static_assert(FM_ALARM_LEN == FM_OFF_MAC + FM_MAC_LEN, "alarm layout inconsistent");
static_assert(FM_OFF_CALLSIGN + FM_CALLSIGN_LEN == FM_OFF_CHANNEL, "callSign field size");
static_assert(FM_OFF_MSGID + 1 == FM_OFF_COMMON_END, "msgId is one byte");
static_assert(FM_FRAME_LEN_MAX <= 255, "SX1262 LoRa payload limit is 255 bytes");
static_assert(FM_FRAG_COUNT <= 15, "totalFrags must fit in 4 bits");
static_assert(FM_HOP_MAX <= 15, "hopLimit must stay a small unsigned value");

// ---------------------------------------------------------------- helpers
/** Common fields lifted out of any frame. callSign is NUL-terminated here. */
struct FmHeader {
  uint8_t ver;        // the raw byte 0, magic and type together
  uint8_t type;       // FM_TYPE_*
  uint8_t hopLimit;
  char    callSign[FM_CALLSIGN_LEN + 1];
  uint8_t channel;
  uint8_t msgId;
};

/** Byte 0 for a frame of this type. */
inline uint8_t fmVerByte(uint8_t type) {
  return (uint8_t)(FM_PROTO_MAGIC | (type & 0x0F));
}
inline uint8_t fmFrameType(const uint8_t *buf) { return (uint8_t)(buf[0] & 0x0F); }
inline bool fmMagicOk(const uint8_t *buf) {
  return (uint8_t)(buf[0] & 0xF0) == FM_PROTO_MAGIC;
}

inline uint16_t fmRd16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
inline void fmWr16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}
inline uint32_t fmRd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
inline void fmWr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

inline uint8_t fmFragIdx(uint8_t fragInfo)   { return (uint8_t)(fragInfo >> 4); }
inline uint8_t fmFragTotal(uint8_t fragInfo) { return (uint8_t)(fragInfo & 0x0F); }
inline uint8_t fmFragInfo(uint8_t idx, uint8_t total) {
  return (uint8_t)((idx << 4) | (total & 0x0F));
}

/** Bytes in fragment i of a full 1500 B clip. */
inline uint16_t fmFragBytes(uint8_t idx) {
  return idx == (FM_FRAG_COUNT - 1) ? (uint16_t)FM_FRAG_LAST_BYTES
                                    : (uint16_t)FM_FRAG_PAYLOAD_MAX;
}
inline uint16_t fmFragOffset(uint8_t idx) {
  return (uint16_t)idx * (uint16_t)FM_FRAG_PAYLOAD_MAX;
}

/**
 * Parse and sanity-check the common prefix. Returns false for anything that is
 * not a well-formed FloodMesh frame - wrong magic, unknown type, absurd hop
 * count, out-of-range channel, or a call sign with bytes outside the permitted
 * alphabet. Everything downstream may assume these hold.
 *
 * The call sign check matters: the raw 5 bytes come off the air with no NUL,
 * and printing them with %s would run off the end of the buffer.
 */
bool fmParseHeader(const uint8_t *buf, size_t len, FmHeader *out);

/** Write the common prefix. Returns bytes written (FM_OFF_COMMON_END). */
size_t fmWriteHeader(uint8_t *buf, uint8_t type, uint8_t hopLimit,
                     const char *callSign, uint8_t channel, uint8_t msgId);
