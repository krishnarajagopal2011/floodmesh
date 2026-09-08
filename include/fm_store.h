/**
 * FloodMesh - LittleFS ring buffer holding the three most recent voice clips.
 *
 * A received voice note cost eight LoRa fragments and several seconds of shared
 * airtime to arrive, and in a flood there is no way to ask for it again. Losing
 * one to a battery swap or a brownout wastes the only resource this device
 * cannot make more of, so clips live in flash rather than RAM.
 *
 * Three slots is a deliberate ceiling, not a memory limit. The inbox is a
 * 128x64 OLED with room for three rows; a queue longer than the screen is a
 * queue nobody scrolls through while standing in water.
 *
 * One file per slot, metadata and audio together
 * ----------------------------------------------
 * clip_0.bin, clip_1.bin, clip_2.bin each hold a 32-byte header followed by up
 * to FM_CLIP_BYTES (1500) bytes of Codec2. The metadata is deliberately NOT in
 * a separate index file: two files cannot be updated atomically, and the
 * failure mode of trying is the worst one on offer - a crash between the two
 * writes pairs clip A's audio with clip B's call sign, so the screen names one
 * sender and the speaker plays another. One file per clip makes that state
 * impossible to express.
 *
 * A write lands in a scratch file and is then renamed over the slot. littlefs
 * commits a rename as a single metadata transaction, so a slot holds either
 * entirely the old clip or entirely the new one, never a blend. On top of that
 * the header carries a magic, a format version, a payload length and two CRCs:
 * a torn, truncated or bit-rotted file fails validation and the slot is
 * reported EMPTY. Nothing corrupt is ever handed upward - there is deliberately
 * no "damaged clip" state for the UI to render or for the decoder to chew on.
 *
 * On-disk header, 32 bytes, LITTLE ENDIAN (same convention as fm_packet.h)
 * -----------------------------------------------------------------------
 *   offset size field       notes
 *   ------ ---- ----------- ------------------------------------------------
 *   0      4    magic       FM_STORE_MAGIC ("FCM1"); anything else = empty
 *   4      1    fmtVer      FM_STORE_FMT_VER; a mismatch reads as empty, so
 *                           bumping it retires old clips rather than
 *                           misinterpreting them
 *   5      1    channel     0..FM_CH_COUNT-1, else the record is rejected
 *   6      2    lengthMs10  clip length in tenths of a second (inbox display)
 *   8      4    rxMillis    millis() at reception. DISPLAY ONLY - see below
 *   12     4    seq         monotonic write counter; this orders the slots
 *   16     2    payloadLen  1..FM_CLIP_BYTES, must equal fileSize - 32
 *   18     2    payloadCrc  CRC-16/CCITT-FALSE over the payload bytes
 *   20     5    callSign    'A'-'Z' / '0'-'9' / space, space padded, NO NUL,
 *                           exactly as it appeared on air
 *   25     5    reserved    written as zero, covered by headerCrc
 *   30     2    headerCrc   CRC-16/CCITT-FALSE over bytes 0..29
 *
 * Why rotation is ordered by seq and not by rxMillis
 * --------------------------------------------------
 * This board has no RTC. rxMillis is millis(), which restarts at zero on every
 * boot, so a clip received two minutes after a reboot carries a SMALLER
 * timestamp than one received an hour before it. Recycling "the oldest
 * timestamp" would therefore destroy the newest clip first after every reboot -
 * precisely backwards, and worst exactly when a node has just been power-cycled
 * in the field.
 *
 * seq is a write counter stored in the header, so it survives the reboot that
 * millis() does not. fmStoreBegin() scans the slots, takes the highest seq it
 * finds and continues from there; the next write claims the slot with the
 * lowest seq. Two valid slots can never tie, because no two writes share a seq.
 * The only tie is between empty slots, and those are filled in index order
 * (clip_0 first) before any occupied slot is recycled. Comparisons use signed
 * differences of unsigned counters, so even a seq wrap orders correctly - at
 * one clip per second that wrap is 136 years out.
 *
 * rxMillis is still stored because the inbox shows relative age ("2m ago"),
 * which stays correct for every clip received since the current boot. A clip
 * that predates the last reboot will misreport its age; that is a cosmetic wart
 * on a device with no clock, and inventing a plausible-looking wall time would
 * be a worse lie than an obviously stale one.
 *
 * Flash wear
 * ----------
 * The data partition is 1.5 MB (default_8MB.csv), i.e. 384 erase blocks of
 * 4 KB, on NOR flash rated for roughly 100k erase cycles per sector. One clip
 * write dirties about two data blocks plus a metadata commit, and littlefs
 * spreads both dynamically - CONFIG_LITTLEFS_BLOCK_CYCLES is 512 in this SDK,
 * so the directory metadata pair relocates instead of becoming a hot spot.
 * That puts wear-out on the order of ten million clip writes. A node receiving
 * a clip every ten seconds without pause would need about three years of
 * continuous traffic to reach it, and a flood deployment is measured in days.
 * Wear is not a design constraint on this product; battery and airtime are.
 *
 * What this module does NOT do
 * ----------------------------
 *  - It allocates nothing itself. The Arduino FS layer beneath it does (a path
 *    buffer per call, a ~512 B littlefs cache per open file), which is why
 *    exactly one file is ever open at a time and no File handle is kept
 *    between calls.
 *  - It is not reentrant, not thread-safe, and not interrupt-safe. Writing
 *    1.5 KB erases flash and can block for tens of milliseconds with the
 *    instruction cache disabled: call these from loop(), never from an ISR, an
 *    I2S callback, or while the radio is mid-frame.
 *  - It never reports partial data. Every failure path yields "empty slot",
 *    which the UI already knows how to draw.
 */
#pragma once
#include <Arduino.h>

#include "fm_packet.h"

// ---------------------------------------------------------------- tunables
#ifndef FM_STORE_SLOTS
#define FM_STORE_SLOTS 3           // one per inbox row on the 128x64 OLED
#endif
#ifndef FM_STORE_PATH_FMT
#define FM_STORE_PATH_FMT "/clip_%u.bin"
#endif
#ifndef FM_STORE_TMP_PATH
#define FM_STORE_TMP_PATH "/clip_tmp.bin"   // scratch file, renamed over a slot
#endif
#ifndef FM_STORE_PART_LABEL
#define FM_STORE_PART_LABEL "spiffs"        // data partition in default_8MB.csv
#endif
#ifndef FM_STORE_BASE_PATH
#define FM_STORE_BASE_PATH "/littlefs"
#endif
#ifndef FM_STORE_MAX_OPEN
#define FM_STORE_MAX_OPEN 4        // this module needs 1; the rest is headroom
#endif
#ifndef FM_STORE_VERIFY_ON_MOUNT
#define FM_STORE_VERIFY_ON_MOUNT 1 // 1 = CRC every stored payload at boot, so a
#endif                             // rotted clip is never listed and then fails
                                   // on play. Costs a few ms of boot time.

// ---------------------------------------------------------------- constants
#define FM_STORE_NONE       0xFF               // "no such slot" from fmStoreNewest
#define FM_STORE_MAGIC      0x314D4346UL       // 'F','C','M','1' on disk
#define FM_STORE_FMT_VER    1
#define FM_STORE_HDR_BYTES  32
#define FM_STORE_FILE_BYTES (FM_STORE_HDR_BYTES + FM_CLIP_BYTES)   // 1532

static_assert(FM_STORE_SLOTS >= 1 && FM_STORE_SLOTS <= 9,
              "slot index must stay a single digit for FM_STORE_PATH_FMT");
static_assert(FM_CLIP_BYTES <= 0xFFFF, "payloadLen is a 16-bit header field");
static_assert(FM_STORE_HDR_BYTES == 32, "the on-disk header table says 32 bytes");

// ---------------------------------------------------------------- types
/**
 * What the inbox needs to know about a stored clip, without touching the audio.
 *
 * callSign is NUL-terminated here (unlike the 5 raw bytes on air) with the
 * trailing padding removed. Bytes read back from flash are sanitised to the
 * on-air alphabet before they land in this struct, because a corrupt sector is
 * exactly as untrusted as a corrupt packet - and this string gets printed
 * with %s.
 *
 * used is derived, never stored: a slot whose file validates is used, and one
 * that does not - missing, truncated, wrong magic, bad CRC - is not.
 */
struct FmClipMeta {
  char     callSign[FM_CALLSIGN_LEN + 1];
  uint8_t  channel;
  uint16_t lengthMs10;      // clip length in tenths of a second
  uint32_t rxMillis;
  bool     used;
};

// ---------------------------------------------------------------- API
/**
 * Mount the clip partition and rebuild the in-RAM slot table from flash.
 * Formats and retries once if the mount fails, because a device that cannot
 * store clips is still a device that must boot. Returns false only if the
 * partition is unusable even after that format; every other call then fails
 * safely and the radio, alarm and UI paths carry on without a store.
 */
bool fmStoreBegin();

/**
 * Store a clip, replacing the oldest slot. len must be 1..FM_CLIP_BYTES and
 * meta->channel must be a real channel. meta->used is ignored (a written clip
 * is used by definition) and meta->callSign may be shorter than 5 characters -
 * it is space padded on disk, matching the on-air form.
 *
 * len and meta->lengthMs10 are NOT cross-checked: a clip reassembled with
 * fragments missing is shorter than its nominal duration and is still worth
 * keeping. Returns false without disturbing any slot if the arguments are bad
 * or the write fails.
 */
bool fmStoreWrite(const FmClipMeta *meta, const uint8_t *clip, size_t len);

/**
 * Read a clip into out (cap bytes; pass at least FM_CLIP_BYTES). On success
 * *outLen holds the payload size and *meta, when given, describes it. On any
 * failure - bad slot, empty slot, cap too small, failed CRC - this returns
 * false, sets *outLen to 0, clears *meta, and the contents of out are
 * undefined. A slot that fails its CRC here is dropped from the inbox so it
 * cannot be selected again.
 */
bool fmStoreRead(uint8_t slot, uint8_t *out, size_t cap, size_t *outLen,
                 FmClipMeta *meta);

/**
 * Metadata only, answered from the RAM table without touching flash. Returns
 * false for an out-of-range or empty slot, and clears *meta when it does.
 */
bool fmStoreMeta(uint8_t slot, FmClipMeta *meta);

/** Slot holding the most recently written clip, or FM_STORE_NONE if empty. */
uint8_t fmStoreNewest();

/** How many slots currently hold a valid clip (0..FM_STORE_SLOTS). */
uint8_t fmStoreCount();

/** Erase every clip. Blocking - a full littlefs format takes a moment. */
void fmStoreFormat();
