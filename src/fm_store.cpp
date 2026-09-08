/**
 * FloodMesh - LittleFS 3-slot clip ring, implementation.
 * See fm_store.h for the on-disk layout, the rotation rule and the wear budget.
 *
 * Two invariants hold everywhere below:
 *
 *  1. Bytes that came out of flash are treated exactly like bytes that came off
 *     the air. Every length is checked against both the caller's capacity and
 *     FM_CLIP_BYTES before a single byte is copied, and every string is
 *     sanitised before anything can print it. Flash corrupts; a header that
 *     survived a CRC is still not a promise about the fields inside it.
 *
 *  2. A file that fails any check is an EMPTY slot, never a damaged one. The
 *     caller has one less clip, which it already knows how to display.
 */
#include "fm_store.h"

#include <LittleFS.h>

namespace {

// ---------------------------------------------------------------- on-disk map
// Offsets for the 32-byte header documented in fm_store.h. Private on purpose:
// nothing outside this file has any business parsing a clip file.
enum {
  kOffMagic   = 0,   // 4
  kOffFmtVer  = 4,   // 1
  kOffChannel = 5,   // 1
  kOffLen10   = 6,   // 2
  kOffRxMs    = 8,   // 4
  kOffSeq     = 12,  // 4
  kOffPayLen  = 16,  // 2
  kOffPayCrc  = 18,  // 2
  kOffCall    = 20,  // FM_CALLSIGN_LEN
  kOffRsvd    = 25,  // 5
  kOffHdrCrc  = 30,  // 2, covers bytes 0..29
};

/** Everything a validated header yields, before the payload is touched. */
struct SlotHdr {
  FmClipMeta meta;
  uint32_t   seq;
  uint16_t   payLen;
  uint16_t   payCrc;
};

struct Slot {
  FmClipMeta meta;
  uint32_t   seq;    // meaningful only while meta.used
};

Slot     g_slot[FM_STORE_SLOTS];
uint32_t g_nextSeq = 1;      // 0 is reserved so an all-zero slot cannot look valid
bool     g_mounted = false;

// ---------------------------------------------------------------- helpers
/**
 * CRC-16/CCITT-FALSE, bitwise. A table would cost 512 B of flash to save a few
 * microseconds on a 1.5 KB buffer that just waited tens of milliseconds for the
 * sector erase - not a trade worth making here.
 */
uint16_t crc16(const uint8_t *p, size_t n, uint16_t crc = 0xFFFF) {
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)((uint16_t)p[i] << 8);
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021)
                           : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

/** True when a is a later write than b, correct across a uint32 wrap. */
bool seqNewer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

void slotPath(uint8_t slot, char *buf, size_t cap) {
  snprintf(buf, cap, FM_STORE_PATH_FMT, (unsigned)slot);
}

/** Pack a caller's call sign into the 5 raw on-air bytes: padded, no NUL. */
void packCallSign(const char *src, uint8_t *dst) {
  size_t i = 0;
  for (; i < (size_t)FM_CALLSIGN_LEN && src[i] != '\0'; i++) {
    dst[i] = (uint8_t)src[i];
  }
  for (; i < (size_t)FM_CALLSIGN_LEN; i++) {
    dst[i] = (uint8_t)' ';
  }
}

/**
 * Unpack 5 stored bytes into a printable, NUL-terminated call sign.
 *
 * Anything outside the on-air alphabet becomes '?' rather than reaching the
 * OLED or the serial log as a control character - a flipped bit in flash must
 * not be able to move the cursor or truncate a line. Trailing pad is dropped so
 * a round trip through the store returns the string the caller handed in.
 */
void unpackCallSign(const uint8_t *src, char *dst) {
  uint8_t last = 0;  // one past the final non-space character
  for (uint8_t i = 0; i < FM_CALLSIGN_LEN; i++) {
    const char c = (char)src[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ';
    dst[i] = ok ? c : '?';
    if (dst[i] != ' ') last = (uint8_t)(i + 1);
  }
  dst[last] = '\0';
}

void clearMeta(FmClipMeta *meta) {
  if (!meta) return;
  memset(meta, 0, sizeof(*meta));
  meta->callSign[0] = '\0';
  meta->used = false;
}

/**
 * Open a slot and validate its header. On success the File is left open and
 * positioned at the first payload byte, and the caller must close it. On any
 * failure the File is closed here and the slot counts as empty.
 */
bool openValid(uint8_t slot, fs::File &f, SlotHdr &h) {
  char path[32];
  slotPath(slot, path, sizeof(path));

  f = LittleFS.open(path, FILE_READ);
  if (!f) return false;  // no file yet: an empty slot, not an error

  uint8_t hdr[FM_STORE_HDR_BYTES];
  const size_t fileSize = f.size();
  if (fileSize < sizeof(hdr) || fileSize > (size_t)FM_STORE_FILE_BYTES ||
      f.read(hdr, sizeof(hdr)) != sizeof(hdr)) {
    f.close();
    return false;
  }

  if (fmRd32(hdr + kOffMagic) != (uint32_t)FM_STORE_MAGIC ||
      hdr[kOffFmtVer] != (uint8_t)FM_STORE_FMT_VER ||
      fmRd16(hdr + kOffHdrCrc) != crc16(hdr, FM_STORE_HDR_BYTES - 2)) {
    f.close();
    return false;
  }

  const uint16_t payLen = fmRd16(hdr + kOffPayLen);
  // The length is checked against the file it claims to describe as well as
  // against the protocol ceiling: a truncated write leaves a short file with a
  // perfectly good header, and only this comparison catches it.
  if (payLen == 0 || payLen > FM_CLIP_BYTES ||
      fileSize != sizeof(hdr) + (size_t)payLen ||
      hdr[kOffChannel] >= FM_CH_COUNT) {
    f.close();
    return false;
  }

  h.seq    = fmRd32(hdr + kOffSeq);
  h.payLen = payLen;
  h.payCrc = fmRd16(hdr + kOffPayCrc);

  clearMeta(&h.meta);
  unpackCallSign(hdr + kOffCall, h.meta.callSign);
  h.meta.channel    = hdr[kOffChannel];
  h.meta.lengthMs10 = fmRd16(hdr + kOffLen10);
  h.meta.rxMillis   = fmRd32(hdr + kOffRxMs);
  h.meta.used       = true;
  return true;
}

/** CRC the payload of an already-validated, still-open file. */
bool payloadOk(fs::File &f, const SlotHdr &h) {
  uint8_t buf[128];  // streamed so no 1.5 KB temporary is needed on the stack
  uint16_t left = h.payLen;
  uint16_t crc = 0xFFFF;
  while (left > 0) {
    const size_t want = (size_t)left < sizeof(buf) ? (size_t)left : sizeof(buf);
    if (f.read(buf, want) != want) return false;
    crc = crc16(buf, want, crc);
    left = (uint16_t)(left - want);
  }
  return crc == h.payCrc;
}

/** Rebuild one RAM slot entry from flash. An unreadable slot becomes empty. */
void loadSlot(uint8_t slot) {
  clearMeta(&g_slot[slot].meta);
  g_slot[slot].seq = 0;

  fs::File f;
  SlotHdr h;
  if (!openValid(slot, f, h)) return;

  const bool ok = FM_STORE_VERIFY_ON_MOUNT ? payloadOk(f, h) : true;
  f.close();
  if (!ok) {
    Serial.printf("[WARN] clip slot %u failed its payload CRC - treating it as "
                  "empty.\n",
                  (unsigned)slot);
    return;
  }

  g_slot[slot].meta = h.meta;
  g_slot[slot].seq  = h.seq;
}

/** Empty slots first, in index order; otherwise the lowest seq wins. */
uint8_t pickVictim() {
  for (uint8_t i = 0; i < FM_STORE_SLOTS; i++) {
    if (!g_slot[i].meta.used) return i;
  }
  uint8_t oldest = 0;
  for (uint8_t i = 1; i < FM_STORE_SLOTS; i++) {
    if (seqNewer(g_slot[oldest].seq, g_slot[i].seq)) oldest = i;
  }
  return oldest;
}

}  // namespace

// ---------------------------------------------------------------- API
bool fmStoreBegin() {
  g_mounted = false;
  g_nextSeq = 1;
  for (uint8_t i = 0; i < FM_STORE_SLOTS; i++) {
    clearMeta(&g_slot[i].meta);
    g_slot[i].seq = 0;
  }

  // Try a plain mount first. Formatting is a last resort, not the default:
  // a mount that fails once because of a transient error would otherwise
  // silently eat every clip already on the device.
  if (!LittleFS.begin(false, FM_STORE_BASE_PATH, FM_STORE_MAX_OPEN,
                      FM_STORE_PART_LABEL)) {
    Serial.println("[WARN] clip partition would not mount - formatting it.");
    if (!LittleFS.begin(true, FM_STORE_BASE_PATH, FM_STORE_MAX_OPEN,
                        FM_STORE_PART_LABEL)) {
      Serial.println("[ERR ] clip partition unusable; voice notes will not be "
                     "stored this session.");
      return false;
    }
  }
  g_mounted = true;

  // A power cut mid-write leaves the scratch file behind. It is never read -
  // only ever overwritten - but clearing it keeps the partition honest about
  // how much space is free.
  if (LittleFS.exists(FM_STORE_TMP_PATH)) LittleFS.remove(FM_STORE_TMP_PATH);

  uint8_t used = 0;
  for (uint8_t i = 0; i < FM_STORE_SLOTS; i++) {
    loadSlot(i);
    if (!g_slot[i].meta.used) continue;
    used++;
    // Resume the write counter above the highest one that survived, which is
    // what makes rotation correct across a reboot; millis() cannot do this.
    if (seqNewer(g_slot[i].seq + 1, g_nextSeq)) g_nextSeq = g_slot[i].seq + 1;
  }
  if (g_nextSeq == 0) g_nextSeq = 1;

  Serial.printf("[INFO] clip store mounted: %u/%u slots in use, next seq %lu\n",
                (unsigned)used, (unsigned)FM_STORE_SLOTS,
                (unsigned long)g_nextSeq);
  return true;
}

bool fmStoreWrite(const FmClipMeta *meta, const uint8_t *clip, size_t len) {
  if (!g_mounted || !meta || !clip) return false;
  if (len == 0 || len > (size_t)FM_CLIP_BYTES) return false;
  if (meta->channel >= FM_CH_COUNT) {
    // Writing it would produce a file the read path rejects, i.e. a slot that
    // silently reads back empty. Refuse loudly instead.
    Serial.printf("[WARN] refusing to store a clip on channel %u.\n",
                  (unsigned)meta->channel);
    return false;
  }

  const uint8_t slot = pickVictim();
  const uint32_t seq = g_nextSeq;

  uint8_t hdr[FM_STORE_HDR_BYTES];
  memset(hdr, 0, sizeof(hdr));
  fmWr32(hdr + kOffMagic, (uint32_t)FM_STORE_MAGIC);
  hdr[kOffFmtVer]  = (uint8_t)FM_STORE_FMT_VER;
  hdr[kOffChannel] = meta->channel;
  fmWr16(hdr + kOffLen10, meta->lengthMs10);
  fmWr32(hdr + kOffRxMs, meta->rxMillis);
  fmWr32(hdr + kOffSeq, seq);
  fmWr16(hdr + kOffPayLen, (uint16_t)len);
  fmWr16(hdr + kOffPayCrc, crc16(clip, len));
  packCallSign(meta->callSign, hdr + kOffCall);
  fmWr16(hdr + kOffHdrCrc, crc16(hdr, FM_STORE_HDR_BYTES - 2));

  // Scratch file first. create=false on purpose: these paths live in the root,
  // so nothing needs the FS layer's directory-creating branch.
  fs::File f = LittleFS.open(FM_STORE_TMP_PATH, FILE_WRITE, false);
  if (!f) {
    Serial.println("[WARN] could not open the clip scratch file.");
    return false;
  }
  const bool written =
      (f.write(hdr, sizeof(hdr)) == sizeof(hdr)) && (f.write(clip, len) == len);
  f.close();
  if (!written) {
    Serial.println("[WARN] clip write failed - the partition is probably full.");
    LittleFS.remove(FM_STORE_TMP_PATH);
    return false;  // the target slot still holds its previous clip, untouched
  }

  char path[32];
  slotPath(slot, path, sizeof(path));

  // littlefs replaces an existing destination inside the rename's own metadata
  // commit, so the slot is never briefly absent. If a future FS layer refuses
  // that, fall back to remove-then-rename: the slot is momentarily empty rather
  // than stale, which is the safe direction to fail in - an absent file reads
  // as an empty slot and never as somebody else's audio.
  if (!LittleFS.rename(FM_STORE_TMP_PATH, path)) {
    clearMeta(&g_slot[slot].meta);
    g_slot[slot].seq = 0;
    LittleFS.remove(path);
    if (!LittleFS.rename(FM_STORE_TMP_PATH, path)) {
      Serial.printf("[WARN] could not commit a clip into slot %u.\n",
                    (unsigned)slot);
      LittleFS.remove(FM_STORE_TMP_PATH);
      return false;
    }
  }

  // Mirror what actually reached flash, not what the caller passed: the stored
  // call sign has been padded to five bytes and is read back trimmed, so
  // round-tripping it here keeps RAM and flash telling the same story.
  clearMeta(&g_slot[slot].meta);
  unpackCallSign(hdr + kOffCall, g_slot[slot].meta.callSign);
  g_slot[slot].meta.channel    = meta->channel;
  g_slot[slot].meta.lengthMs10 = meta->lengthMs10;
  g_slot[slot].meta.rxMillis   = meta->rxMillis;
  g_slot[slot].meta.used       = true;
  g_slot[slot].seq             = seq;

  g_nextSeq = seq + 1;
  if (g_nextSeq == 0) g_nextSeq = 1;  // 0 means "empty" in the RAM table
  return true;
}

bool fmStoreRead(uint8_t slot, uint8_t *out, size_t cap, size_t *outLen,
                 FmClipMeta *meta) {
  if (outLen) *outLen = 0;
  clearMeta(meta);
  if (!g_mounted || slot >= FM_STORE_SLOTS || !out || !outLen) return false;

  fs::File f;
  SlotHdr h;
  if (!openValid(slot, f, h)) return false;

  // Capacity is checked before anything is read, so a small buffer can never be
  // half-filled. cap is a caller-side promise; payLen came off flash. Neither
  // is trusted without the other.
  if ((size_t)h.payLen > cap) {
    f.close();
    Serial.printf("[WARN] clip in slot %u is %u B, caller offered %u B.\n",
                  (unsigned)slot, (unsigned)h.payLen, (unsigned)cap);
    return false;
  }

  const size_t got = f.read(out, (size_t)h.payLen);
  f.close();
  if (got != (size_t)h.payLen || crc16(out, got) != h.payCrc) {
    // Bit rot, or a file that changed under us. Drop the slot from the RAM
    // table so the inbox stops offering a clip that cannot be played; the file
    // itself is left alone and will be recycled by the next write.
    Serial.printf("[WARN] clip slot %u failed verification on read.\n",
                  (unsigned)slot);
    clearMeta(&g_slot[slot].meta);
    g_slot[slot].seq = 0;
    return false;
  }

  *outLen = got;
  if (meta) *meta = h.meta;
  return true;
}

bool fmStoreMeta(uint8_t slot, FmClipMeta *meta) {
  clearMeta(meta);
  if (!g_mounted || slot >= FM_STORE_SLOTS || !g_slot[slot].meta.used) {
    return false;
  }
  if (meta) *meta = g_slot[slot].meta;
  return true;
}

uint8_t fmStoreNewest() {
  uint8_t best = FM_STORE_NONE;
  for (uint8_t i = 0; i < FM_STORE_SLOTS; i++) {
    if (!g_slot[i].meta.used) continue;
    if (best == FM_STORE_NONE || seqNewer(g_slot[i].seq, g_slot[best].seq)) {
      best = i;
    }
  }
  return best;
}

uint8_t fmStoreCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < FM_STORE_SLOTS; i++) {
    if (g_slot[i].meta.used) n++;
  }
  return n;
}

void fmStoreFormat() {
  if (g_mounted && !LittleFS.format()) {
    // Nothing else to try: the RAM table is cleared regardless, so the device
    // behaves as if the store were empty even if stale files survive. They
    // cannot be surfaced, only overwritten.
    Serial.println("[ERR ] clip partition format failed.");
  }
  for (uint8_t i = 0; i < FM_STORE_SLOTS; i++) {
    clearMeta(&g_slot[i].meta);
    g_slot[i].seq = 0;
  }
  // The seq counter restarts too. Nothing readable survives a format, so there
  // is no older clip left for a low seq to be misordered against.
  g_nextSeq = 1;
}
