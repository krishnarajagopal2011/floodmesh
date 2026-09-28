/**
 * FloodMesh V4 - field logger. See fm_log.h for the design and
 * docs/field-logger-protocol.md for the contract with the web app.
 *
 * Layout of this file: the on-flash record, the store (segment files, cursor,
 * dropping), the record hooks (loop task), JSON and the uploader task, then
 * fmLogBegin / fmLogLoop and the serial commands.
 */
#include "fm_log.h"

#if FM_LOG_ENABLE

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>

#include "fm_alarm.h"
#include "fm_ids.h"
#include "fm_mesh.h"
#include "fm_ota.h"
#include "fm_packet.h"
#include "fm_role.h"
#include "fm_text.h"

#ifndef FLOODMESH_VERSION
#define FLOODMESH_VERSION "4.2.0"
#endif

#if !FM_LOG_TLS_INSECURE
// The Mozilla root bundle already inside the Arduino core's prebuilt mbedTLS
// (x509_crt_bundle.S.obj in libmbedtls.a, CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_
// DEFAULT_FULL, 136 roots, ~62 KB of flash). Nothing else in the core links it
// in; referencing it here does. WiFiClientSecure::setCACertBundle() takes this
// exact format (a 2-byte count, then name/key entries sorted by subject).
extern const uint8_t kFmCaBundle[] asm("_binary_x509_crt_bundle_start");
#endif

namespace {

// ================================================================ the record
// 128 bytes on flash, fixed size so a damaged one can be stepped over and the
// rest stay aligned. Little-endian, native layout: only this firmware reads it.
constexpr uint8_t kMagic = 0xF1;   // "record format 1"

enum Kind : uint8_t { K_BOOT = 1, K_RX, K_ECHO, K_TX, K_RELAY, K_SOS, K_STATUS, K_END };

// Which optional fields a record carries (protocol §3: "absent when not applicable").
enum : uint8_t {
  FL_ID = 0x01,
  FL_HOPS = 0x02,
  FL_SIG = 0x04,    // rssi and snr
  FL_CH = 0x08,
  FL_TEXT = 0x10,
  FL_OK = 0x20,     // tx: `ok` present, value in FL_OKV
  FL_OKV = 0x40,
  FL_FROM = 0x80,
};

struct BootInfo {
  char reason[12];
  char fw[24];
  char image[9];
};

struct StatusInfo {
  uint16_t battMv;
  uint8_t  battPct;
  uint8_t  powered;
  uint16_t airPm;
  uint8_t  heard;
  uint8_t  sleepPct;
  uint8_t  responder;
};

struct Rec {
  uint8_t  magic;
  uint8_t  kind;
  uint8_t  t;          // FmLogType
  uint8_t  flags;
  uint32_t seq;
  uint32_t boot;
  uint32_t ms;
  uint32_t epoch;
  int16_t  rssi;       // dBm, rounded as RXLOG prints it
  int16_t  snr10;      // dB x 10, rounded as RXLOG prints it
  uint16_t id;
  uint8_t  hops;
  uint8_t  ch;
  uint8_t  state;      // FmLogSosState
  uint8_t  n;
  char     from[FM_CALLSIGN_LEN + 1];
  union {
    char       text[FM_TEXT_MAX_CHARS + 1];
    BootInfo   boot;
    StatusInfo status;
  } u;
  uint8_t  pad[26];
  uint32_t crc;        // CRC-32 of everything above
};
static_assert(sizeof(Rec) == 128, "a record is 128 bytes: 32 per 4 KB flash block");
static_assert(offsetof(Rec, crc) == 124, "crc must be the last 4 bytes");

constexpr size_t kRec = sizeof(Rec);

// ================================================================ state
constexpr char kBase[] = "/littlefs";         // Arduino LittleFS default mount point
constexpr char kDir[] = "/littlefs/fmlog";
constexpr char kPart[] = "spiffs";            // the data partition of default_8MB.csv

// NVS: the logger's own counters in "fmlog"; the settings named by the
// protocol (§6) next to the WiFi update keys in FM_NVS_NAMESPACE.
constexpr char kNs[] = "fmlog";
constexpr char kKeyBoot[] = "boot";
constexpr char kKeySeq[] = "seqhw";
constexpr char kKeyAck[] = "ackseq";
constexpr char kKeyDropped[] = "dropped";
constexpr char kKeyUrl[] = "logurl";
constexpr char kKeySsid2[] = "wifissid2";
constexpr char kKeyPass2[] = "wifipass2";

constexpr size_t kUrlMax = 120;
constexpr uint8_t kScanMax = 64;   // segment files looked at during the boot scan

struct Seg {
  uint32_t num;        // file name; only ever grows
  uint16_t nrec;       // whole records in the file (damaged ones included)
  bool     sealed;     // never append again (torn tail or a failed write)
  uint32_t firstSeq;
  uint32_t lastSeq;
};

// ---- store: shared with the uploader task, under g_mx
SemaphoreHandle_t g_mx = nullptr;
bool     g_fsOk = false;
Seg      g_segs[FM_LOG_SEGMENTS + 1];   // oldest first
uint8_t  g_nsegs = 0;
uint16_t g_curSlot = 0;                 // g_segs[0] slots before this are sent (or damaged)
uint32_t g_nextNum = 1;
uint32_t g_ackSeq = 0;                  // highest seq the server has acknowledged
bool     g_ackDirty = false;                // g_ackSeq not in NVS yet (fmLogLoop saves it)
// Segment files the uploader is done with. It only takes them out of the
// table; fmLogLoop deletes them while the mesh is idle (see idleFlashChores).
uint32_t g_unlinkQ[FM_LOG_SEGMENTS + 1];
uint8_t  g_unlinkN = 0;

struct Stats {
  char     result[160];
  bool     tried;
  uint32_t tryMs;
  uint32_t sentBoot;
  uint32_t windowsOk;
  uint32_t stackFree;
  uint32_t heapMin;
};
Stats g_st = {};

// ---- read by the status screen without the mutex
std::atomic<bool>     g_busy{false};
std::atomic<uint32_t> g_waiting{0};
std::atomic<uint32_t> g_dropped{0};
std::atomic<uint32_t> g_lastOkMs{0};
std::atomic<bool>     g_everOk{false};
// The uploader left flash work for the loop: queued deletes or an unsaved ack.
std::atomic<bool>     g_chores{false};

// ---- the logger's clock anchor: Unix ms = g_timeOffsetMs + esp_timer ms
portMUX_TYPE g_timeMux = portMUX_INITIALIZER_UNLOCKED;
bool     g_timeOk = false;
int64_t  g_timeOffsetMs = 0;
uint32_t g_timeSyncMs = 0;
char     g_timeSrc[24] = "";   // "SNTP time.google.com" is 20 characters

// ---- the V4 clock (fmTimeSet), uploader task only: see feedV4Clock
uint32_t g_v4SlewUsedS = 0;   // seconds one time source alone has moved it this boot
bool     g_v4UnsetSaid = false;

// ---- loop task only
TaskHandle_t  g_loopTask = nullptr;
TaskHandle_t  g_task = nullptr;
FmLogStatusFn g_statusFn = nullptr;
bool     g_ready = false;
Rec      g_q[FM_LOG_RAMQ];
uint8_t  g_qn = 0;
bool     g_droppedDirty = false;
bool     g_foreignWarned = false;
uint32_t g_boot = 0;
uint32_t g_seqNext = 1;
uint32_t g_seqLimit = 1;
uint32_t g_nextStatus = 0;
uint32_t g_nextUpload = 0;
char     g_url[kUrlMax + 1] = "";
bool     g_urlFromNvs = false;
char     g_ssid2[33] = "";
bool     g_hasPass2 = false;
uint8_t  g_key[32];
size_t   g_keyLen = 0;   // 0 = no valid FM_LOG_KEY: record, never upload
char     g_mac[18] = "";
char     g_image[9] = "";

// Own frames recently sent, so an echo can say PING rather than TEXT, HELP
// rather than ACK.
struct Sent {
  uint8_t frameType;
  uint8_t id;
  uint8_t t;
};
Sent    g_sent[16];
uint8_t g_sentHead = 0;

// Written by the loop just before it wakes the task; untouched while g_busy.
struct Job {
  char url[kUrlMax + 1];
  char unit[FM_CALLSIGN_LEN + 1];
  char host[16];
};
Job g_job;

// ================================================================ small helpers
const char *kindName(uint8_t k) {
  switch (k) {
    case K_BOOT:   return "boot";
    case K_RX:     return "rx";
    case K_ECHO:   return "echo";
    case K_TX:     return "tx";
    case K_RELAY:  return "relay";
    case K_SOS:    return "sos";
    case K_STATUS: return "status";
    default:       return "?";
  }
}

const char *typeName(uint8_t t) {
  switch (t) {
    case FM_LOG_T_SOS:       return "SOS";
    case FM_LOG_T_ALARM:     return "ALARM";
    case FM_LOG_T_TEXT:      return "TEXT";
    case FM_LOG_T_PING:      return "PING";
    case FM_LOG_T_ACK:       return "ACK";
    case FM_LOG_T_HEARTBEAT: return "HEARTBEAT";
    case FM_LOG_T_HELP:      return "HELP";
    default:                 return nullptr;
  }
}

const char *stateName(uint8_t s) {
  switch (s) {
    case FM_LOG_SOS_SENT:          return "SENT";
    case FM_LOG_SOS_RETRY:         return "RETRY";
    case FM_LOG_SOS_DELIVERED:     return "DELIVERED";
    case FM_LOG_SOS_HELP:          return "HELP";
    case FM_LOG_SOS_NOT_DELIVERED: return "NOT_DELIVERED";
    case FM_LOG_SOS_STOPPED:       return "STOPPED";
    default:                       return "?";
  }
}

const char *resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

/** The frame type an own-transmission type goes out as, for echo matching. */
uint8_t frameTypeOf(uint8_t t) {
  switch (t) {
    case FM_LOG_T_SOS:
    case FM_LOG_T_ALARM:     return FM_TYPE_ALARM;
    case FM_LOG_T_TEXT:
    case FM_LOG_T_PING:      return FM_TYPE_TEXT;
    case FM_LOG_T_ACK:
    case FM_LOG_T_HELP:      return FM_TYPE_ACK;
    case FM_LOG_T_HEARTBEAT: return FM_TYPE_HEARTBEAT;
    default:                 return 0;
  }
}

/**
 * Copy for JSON: printable ASCII only. Bytes off the air are authenticated but
 * the body must stay valid UTF-8, since the HMAC is over its exact bytes and a
 * server that re-encodes a bad byte would no longer match it.
 */
void copyPrintable(char *dst, size_t n, const char *src, bool trimRight) {
  if (!n) return;
  size_t i = 0;
  for (; src && src[i] && i + 1 < n; i++) {
    const char c = src[i];
    dst[i] = (c >= 0x20 && c <= 0x7E) ? c : '?';
  }
  dst[i] = '\0';
  if (trimRight) {
    while (i > 0 && dst[i - 1] == ' ') dst[--i] = '\0';
  }
}

bool printable(const char *s) {
  for (; *s; s++) {
    if (*s < 0x20 || *s > 0x7E) return false;
  }
  return true;
}

/** The value RXLOG prints with "%.0f", as an int. */
int16_t rssiAsLogged(float rssi) {
  char b[16];
  snprintf(b, sizeof(b), "%.0f", rssi);
  return (int16_t)atoi(b);
}

/** The value RXLOG prints with "%.1f", in tenths. */
int16_t snr10AsLogged(float snr) {
  char b[16];
  snprintf(b, sizeof(b), "%.1f", snr);
  const bool neg = b[0] == '-';
  char *end = nullptr;
  const long whole = strtol(b + (neg ? 1 : 0), &end, 10);
  const long tenth = (end && *end == '.' && end[1] >= '0' && end[1] <= '9') ? end[1] - '0' : 0;
  const long v = whole * 10 + tenth;
  return (int16_t)(neg ? -v : v);
}

void fmtAge(uint32_t ms, char *out, size_t n) {
  const uint32_t s = ms / 1000;
  if (s < 60)        snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else               snprintf(out, n, "%luh", (unsigned long)(s / 3600));
}

int64_t nowMs64() { return esp_timer_get_time() / 1000; }   // the clock millis() reads

/** Unix seconds from the logger's anchor, or 0 until it has synced this boot. */
uint32_t epochNow() {
  portENTER_CRITICAL(&g_timeMux);
  const bool ok = g_timeOk;
  const int64_t off = g_timeOffsetMs;
  portEXIT_CRITICAL(&g_timeMux);
  if (!ok) return 0;
  return (uint32_t)((off + nowMs64()) / 1000);
}

/**
 * New anchor for the records' epoch, from SNTP or the server. The V4 clock is
 * not touched here: feedV4Clock() decides that, more strictly, at the end of
 * the window. Returns the offset (Unix ms - nowMs64()) it stored.
 */
int64_t setAnchor(int64_t unixMsNow, const char *src) {
  const int64_t off = unixMsNow - nowMs64();
  portENTER_CRITICAL(&g_timeMux);
  const bool had = g_timeOk;
  const int64_t old = g_timeOffsetMs;
  g_timeOk = true;
  g_timeOffsetMs = off;
  g_timeSyncMs = millis();
  snprintf(g_timeSrc, sizeof(g_timeSrc), "%s", src);
  portEXIT_CRITICAL(&g_timeMux);
  if (had) {
    // The drift since the last sync: the RC-clocked RTC keeps time in light
    // sleep, so this is worth watching during the first field tests.
    Serial.printf("[LOG] clock from %s, %+ld ms against the previous sync\n", src,
                  (long)(off - old));
  } else {
    Serial.printf("[LOG] clock from %s: records now carry epoch (%lu)\n", src,
                  (unsigned long)(unixMsNow / 1000));
  }
  return off;
}

bool plausibleUnixMs(int64_t ms) {
  return ms >= (int64_t)FM_TIME_MIN_VALID * 1000 && ms < 4102444800000LL;   // before 2100
}

/** The times one upload window heard, as offsets (Unix ms - nowMs64()). */
struct ClockVotes {
  bool    sntp;
  int64_t sntpOff;
  char    sntpSrc[24];
  bool    https;      // server_epoch over HTTPS with a verified certificate
  int64_t httpsOff;
};

/**
 * Offer one window's times to the V4 clock (fm_role's, uploader task).
 *
 * That clock only ever moves forward, is saved to NVS hourly, and a responder
 * whose grant it passes loses its key for good. One reading years ahead would
 * expire every grant, and every new one too, since provisioning cannot move
 * the clock back. Neither source here is authenticated on its own: SNTP is
 * plain UDP (the nonce stops off-path answers, not a bad pool server or
 * someone on the hotspot), and server_epoch over http:// or unverified TLS is
 * whatever the path says. So:
 *  - SNTP and a server_epoch over verified HTTPS that agree within
 *    FM_LOG_CLOCK_AGREE_MS: the clock goes there, however far;
 *  - one of them alone (or two that disagree: the earlier one): forward by at
 *    most FM_LOG_CLOCK_SLEW_S plus FM_LOG_CLOCK_SLEW_PCT of the uptime, in all,
 *    per boot. That covers clock drift, and an on-path liar gains no more;
 *  - a clock never set (no provisioning yet) is set only by two that agree;
 *  - server_epoch over http:// or unverified TLS: never (ClockVotes::https).
 * A clock held back by this is the case fm_role.h already accepts after a
 * power-off: a responder grant runs a little long, never short.
 */
void feedV4Clock(const ClockVotes &v) {
  if (!v.sntp && !v.https) return;
  const int64_t nowMs = nowMs64();
  const uint32_t cur = fmTimeNow();
  if (v.sntp && v.https) {
    const int64_t d = v.sntpOff - v.httpsOff;
    if ((d < 0 ? -d : d) <= (int64_t)FM_LOG_CLOCK_AGREE_MS) {
      const uint32_t t = (uint32_t)((v.sntpOff + nowMs) / 1000);
      if (t > cur + 1) {   // a second or less is fmTimeSet() dropping the fraction
        fmTimeSet(t);
        if (cur) {
          Serial.printf("[LOG] V4 clock +%lu s: %s and the HTTPS server agree\n",
                        (unsigned long)(t - cur), v.sntpSrc);
        } else {
          Serial.printf("[LOG] V4 clock set to %lu: %s and the HTTPS server agree\n",
                        (unsigned long)t, v.sntpSrc);
        }
      }
      return;
    }
    Serial.printf("[LOG] !! %s and the HTTPS server differ by %ld s - V4 clock: drift only\n",
                  v.sntpSrc, (long)(d / 1000));
  }
  const bool useSntp = v.sntp && (!v.https || v.sntpOff <= v.httpsOff);
  const int64_t off = useSntp ? v.sntpOff : v.httpsOff;
  const char *src = useSntp ? v.sntpSrc : "HTTPS server";
  if (cur == 0) {
    if (!g_v4UnsetSaid) {
      g_v4UnsetSaid = true;
      Serial.printf("[LOG] V4 clock not set: %s alone does not set it (provisioning does, or "
                    "SNTP and the HTTPS server together)\n", src);
    }
    return;
  }
  const int64_t want = (off + nowMs) / 1000;
  if (want <= (int64_t)cur + 1) return;
  const uint64_t budget =
      FM_LOG_CLOCK_SLEW_S + (uint64_t)(nowMs / 1000) * FM_LOG_CLOCK_SLEW_PCT / 100;
  const uint64_t left = budget > g_v4SlewUsedS ? budget - g_v4SlewUsedS : 0;
  const uint64_t gap = (uint64_t)(want - (int64_t)cur);
  const uint32_t step = (uint32_t)(gap < left ? gap : left);
  if (step) {
    fmTimeSet(cur + step);
    g_v4SlewUsedS += step;
  }
  if (step < gap) {
    Serial.printf("[LOG] V4 clock %lu s behind %s: moved %lu s, the rest needs SNTP and the "
                  "HTTPS server to agree (responder expiry)\n",
                  (unsigned long)gap, src, (unsigned long)step);   // < 2^32: plausibleUnixMs
  } else {
    Serial.printf("[LOG] V4 clock +%lu s from %s\n", (unsigned long)step, src);
  }
}

bool inLoopTask() {
  if (xTaskGetCurrentTaskHandle() == g_loopTask) return true;
  if (!g_foreignWarned) {
    g_foreignWarned = true;
    Serial.println("[LOG] !! record hook called outside the loop task - ignored (fm_log.h)");
  }
  return false;
}

uint32_t crcOf(const Rec &r) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&r), offsetof(Rec, crc));
}

bool recValid(const Rec &r) {
  return r.magic == kMagic && r.kind >= K_BOOT && r.kind < K_END && r.crc == crcOf(r);
}

// ================================================================ NVS
uint32_t nvsGet(const char *key, uint32_t def) {
  Preferences p;
  // Read-write: a read-only open of a namespace that does not exist yet (first
  // boot) fails with an error line on the console.
  if (!p.begin(kNs, false)) return def;
  const uint32_t v = p.isKey(key) ? p.getUInt(key, def) : def;
  p.end();
  return v;
}

bool nvsPut(const char *key, uint32_t v) {
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  const bool ok = p.putUInt(key, v) == sizeof(uint32_t);
  p.end();
  return ok;
}

/** One WiFi network from NVS; false unless both the name and a password (maybe empty) are stored. */
bool loadNet(const char *kSsid, const char *kPass, char ssid[33], char pass[64]) {
  ssid[0] = '\0';
  pass[0] = '\0';
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, true)) return false;
  if (p.isKey(kSsid)) p.getString(kSsid, ssid, 33);
  const bool hasPass = p.isKey(kPass);
  if (hasPass) p.getString(kPass, pass, 64);
  p.end();
  return ssid[0] != '\0' && hasPass;
}

/**
 * Promise the next FM_LOG_SEQ_BLOCK seq values: one NVS write. Same block
 * reservation as fmNextAuthCounter() (fm_ids.cpp): the NVS value is the first
 * seq NOT yet promised. A failed write still moves the limit (a record now
 * beats a perfect seq after the next reboot) but says so.
 */
void reserveSeq() {
  const uint32_t limit = (g_seqLimit > g_seqNext ? g_seqLimit : g_seqNext) + FM_LOG_SEQ_BLOCK;
  if (!nvsPut(kKeySeq, limit)) {
    Serial.printf("[LOG] !! seq high-water mark %lu not saved - seq may repeat after a reboot\n",
                  (unsigned long)limit);
  }
  g_seqLimit = limit;
}

/**
 * Next seq. newRec() runs inside the rx and relay hooks, which must not write
 * flash (fm_log.h), so fmLogLoop keeps at least half a block reserved ahead
 * while the mesh is idle. The write here is only the fallback for a loop that
 * made that many records without one idle pass: a correct seq beats timing.
 */
uint32_t nextSeq() {
  if (g_seqNext >= g_seqLimit) reserveSeq();
  return g_seqNext++;
}

// ================================================================ the store (caller holds g_mx)
void segPath(uint32_t num, char *out, size_t n) {
  snprintf(out, n, "%s/%08lu.rec", kDir, (unsigned long)num);
}

/** Reads up to `count` whole records from slot `slot`; returns how many it got. */
uint16_t readSlots(uint32_t num, uint16_t slot, Rec *out, uint16_t count) {
  char path[40];
  segPath(num, path, sizeof(path));
  const int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  uint16_t got = 0;
  if (lseek(fd, (off_t)slot * (off_t)kRec, SEEK_SET) >= 0) {
    const ssize_t r = read(fd, out, (size_t)count * kRec);
    got = r > 0 ? (uint16_t)((size_t)r / kRec) : 0;
  }
  close(fd);
  return got;
}

void updateWaiting() {
  uint32_t w = 0;
  for (uint8_t i = 0; i < g_nsegs; i++) w += g_segs[i].nrec;
  w = w > g_curSlot ? w - g_curSlot : 0;
  g_waiting.store(w);
}

void unlinkSeg(uint32_t num) {
  char path[40];
  segPath(num, path, sizeof(path));
  if (unlink(path) != 0 && errno != ENOENT) {
    Serial.printf("[LOG] !! could not delete %s (errno %d)\n", path, errno);
  }
}

/** Delete the files the uploader queued (caller holds g_mx). */
void unlinkQueued() {
  for (uint8_t i = 0; i < g_unlinkN; i++) unlinkSeg(g_unlinkQ[i]);
  g_unlinkN = 0;
}

/**
 * Take g_segs[0] out of the table and delete its file. defer (the uploader
 * task): the file is only queued, and fmLogLoop deletes it while the mesh is
 * idle. A LittleFS delete is a flash write, which stalls both cores for tens
 * of ms (longer when it erases), and the uploader cannot know whether a relay
 * or ACK is counting down on the other core. A queued file is out of the
 * table, so nothing reads it again; after a reboot the scan finds it and
 * either deletes it as acknowledged or, if the ack was not saved yet, sends
 * it again (the server keeps each seq once).
 */
void removeFirst(bool defer) {
  if (!g_nsegs) return;
  if (defer && g_unlinkN < sizeof(g_unlinkQ) / sizeof(g_unlinkQ[0])) {
    g_unlinkQ[g_unlinkN++] = g_segs[0].num;
    g_chores.store(true);
  } else {
    unlinkSeg(g_segs[0].num);
  }
  memmove(&g_segs[0], &g_segs[1], sizeof(Seg) * (g_nsegs - 1));
  g_nsegs--;
  g_curSlot = 0;
}

/** Store full: the oldest file goes, and its unsent records count as dropped. */
void dropOldest(const char *why) {
  if (!g_nsegs) return;
  const uint32_t lost = g_segs[0].nrec > g_curSlot ? g_segs[0].nrec - g_curSlot : 0;
  removeFirst(false);
  if (lost) {
    g_dropped.fetch_add(lost);
    g_droppedDirty = true;
    Serial.printf("[LOG] %s: the oldest %lu unsent records dropped (%lu lost in all)\n", why,
                  (unsigned long)lost, (unsigned long)g_dropped.load());
  }
}

/** Fully sent files at the front go, except the one still being appended to. */
void normalize(bool defer) {
  while (g_nsegs && g_curSlot >= g_segs[0].nrec &&
         (g_segs[0].nrec >= FM_LOG_SEG_RECS || g_segs[0].sealed || g_nsegs > 1)) {
    removeFirst(defer);
  }
}

/**
 * Move the send cursor to (file `num`, slot `slot`); files before it are done.
 * Uploader task only: the files it finishes are queued for the loop to delete.
 */
void advanceTo(uint32_t num, uint16_t slot) {
  while (g_nsegs && g_segs[0].num < num) removeFirst(true);
  if (g_nsegs && g_segs[0].num == num && slot > g_curSlot) g_curSlot = slot;
  normalize(true);
  updateWaiting();
}

/**
 * First valid record of a file from its start (forward) or end (backward).
 * Slots that cannot be read (a read error, e.g. a flash bit error in one
 * block) are stepped over like damaged records, so a bad chunk never hides
 * the valid records around it. *unreadable says whether any slot was skipped
 * that way.
 */
bool edgeSeq(const Seg &s, bool fromEnd, uint32_t *seq, bool *unreadable) {
  Rec chunk[8];
  uint16_t pos = fromEnd ? s.nrec : 0;
  while (fromEnd ? pos > 0 : pos < s.nrec) {
    const uint16_t k = fromEnd ? (pos >= 8 ? 8 : pos) : (s.nrec - pos >= 8 ? 8 : s.nrec - pos);
    const uint16_t start = fromEnd ? (uint16_t)(pos - k) : pos;
    const uint16_t got = readSlots(s.num, start, chunk, k);
    if (got < k) *unreadable = true;   // chunk[got..k-1] were not read
    for (uint16_t i = 0; i < got; i++) {
      const Rec &r = chunk[fromEnd ? got - 1 - i : i];
      if (recValid(r)) {
        *seq = r.seq;
        return true;
      }
    }
    pos = fromEnd ? start : (uint16_t)(pos + k);
  }
  return false;
}

/**
 * How many records of a run of `nrec` deleted records (files with no valid
 * record, so no readable seq) had not been acknowledged yet. They lie between
 * the kept files around them: after the one ending at prevLast, before the one
 * starting at nextFirst (0 = none after). Within one boot seq has no gaps, so
 * counting from the nearer known end is exact then; across a reboot's gap it
 * is an estimate.
 */
uint32_t unsentOf(uint32_t nrec, uint32_t prevLast, uint32_t nextFirst) {
  if (nextFirst) {   // the run ends just below nextFirst
    const uint32_t floorSeq = g_ackSeq > prevLast ? g_ackSeq : prevLast;
    const uint32_t above = nextFirst - 1 > floorSeq ? nextFirst - 1 - floorSeq : 0;
    return above < nrec ? above : nrec;
  }
  // The run starts just above prevLast.
  const uint32_t acked = g_ackSeq > prevLast ? g_ackSeq - prevLast : 0;
  return acked < nrec ? nrec - acked : 0;
}

void countScanLost(uint32_t lost) {
  if (!lost) return;
  g_dropped.fetch_add(lost);
  g_droppedDirty = true;
  Serial.printf("[LOG] %lu unsent records were in the deleted file(s) - counted as dropped\n",
                (unsigned long)lost);
}

/** Boot: list the segment files, rebuild the table and the send cursor. */
uint32_t scanStore() {
  if (mkdir(kDir, 0775) != 0 && errno != EEXIST) {
    Serial.printf("[LOG] !! cannot create %s (errno %d)\n", kDir, errno);
  }
  uint32_t nums[kScanMax];
  uint8_t n = 0;
  DIR *d = opendir(kDir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
      char *end = nullptr;
      const unsigned long v = strtoul(e->d_name, &end, 10);
      if (!end || strcmp(end, ".rec") != 0 || end == e->d_name) continue;
      if (n < kScanMax) nums[n++] = (uint32_t)v;
      else Serial.printf("[LOG] !! more than %u log files - %s ignored\n", (unsigned)kScanMax, e->d_name);
    }
    closedir(d);
  }
  for (uint8_t i = 1; i < n; i++) {   // oldest first
    for (uint8_t j = i; j > 0 && nums[j] < nums[j - 1]; j--) {
      const uint32_t t = nums[j];
      nums[j] = nums[j - 1];
      nums[j - 1] = t;
    }
  }

  g_nsegs = 0;
  g_curSlot = 0;
  uint32_t lastSeq = 0;
  uint32_t goneRecs = 0;   // records of deleted files since the last kept one
  uint32_t goneAfter = 0;  // ... which all come after this seq
  for (uint8_t i = 0; i < n; i++) {
    char path[40];
    segPath(nums[i], path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) continue;
    Seg s = {};
    s.num = nums[i];
    s.nrec = (uint16_t)(st.st_size / (off_t)kRec);
    // A length that is not a whole number of records means a write stopped
    // part-way. Keep what is whole; never append behind the torn tail.
    s.sealed = (st.st_size % (off_t)kRec) != 0;
    if (s.sealed) Serial.printf("[LOG] %s has a torn tail - sealed\n", path);
    bool unreadable = false;
    if (!edgeSeq(s, false, &s.firstSeq, &unreadable) ||
        !edgeSeq(s, true, &s.lastSeq, &unreadable)) {
      // Not one valid record in any slot that could be read. The file's seqs
      // are unknown, so the table cannot place it; readBatch would step over
      // the same slots anyway. Its unsent records count as dropped.
      Serial.printf("[LOG] %s holds no valid record%s - deleted\n", path,
                    unreadable ? " (part of it unreadable)" : "");
      unlink(path);
      if (s.nrec) {
        if (!goneRecs) goneAfter = lastSeq;
        goneRecs += s.nrec;
      }
      continue;
    }
    if (unreadable) Serial.printf("[LOG] %s has unreadable records - they will be skipped\n", path);
    if (goneRecs) {   // deleted files just before this one: now their seq range is known
      countScanLost(unsentOf(goneRecs, goneAfter, s.firstSeq));
      goneRecs = 0;
    }
    if (g_nsegs >= FM_LOG_SEGMENTS) {   // FM_LOG_SEGMENTS was lowered since
      dropOldest("store over capacity");
    }
    g_segs[g_nsegs++] = s;
    if (s.lastSeq > lastSeq) lastSeq = s.lastSeq;
    g_nextNum = s.num + 1;
  }
  if (goneRecs) countScanLost(unsentOf(goneRecs, goneAfter, 0));
  // Only the newest file is ever appended to; the older ones are complete.
  for (uint8_t i = 0; i + 1 < g_nsegs; i++) g_segs[i].sealed = true;

  // The cursor: skip what the server acknowledged before the reboot.
  while (g_nsegs && g_segs[0].lastSeq <= g_ackSeq) removeFirst(false);
  if (g_nsegs && g_segs[0].firstSeq <= g_ackSeq) {
    Rec chunk[8];
    const Seg &s = g_segs[0];
    uint16_t slot = 0;
    bool found = false;
    while (slot < s.nrec && !found) {
      const uint16_t k = s.nrec - slot >= 8 ? 8 : s.nrec - slot;
      const uint16_t got = readSlots(s.num, slot, chunk, k);
      uint16_t i = 0;
      while (i < got && !(recValid(chunk[i]) && chunk[i].seq > g_ackSeq)) i++;
      found = i < got;
      // Not found: the whole chunk is behind the cursor, its unread part too
      // (edgeSeq and readBatch step over unreadable slots the same way).
      slot = (uint16_t)(slot + (found ? i : k));
    }
    g_curSlot = slot;
  }
  normalize(false);
  updateWaiting();
  return lastSeq;
}

/**
 * Append records to the newest file, opening a new one when it is full or
 * sealed (dropping the oldest when the store is full). Returns how many landed.
 */
uint8_t appendRecs(const Rec *recs, uint8_t n) {
  uint8_t done = 0;
  bool retried = false;
  while (done < n) {
    Seg *s = g_nsegs ? &g_segs[g_nsegs - 1] : nullptr;
    if (!s || s->sealed || s->nrec >= FM_LOG_SEG_RECS) {
      if (g_nsegs >= FM_LOG_SEGMENTS) dropOldest("log full");
      s = &g_segs[g_nsegs++];
      *s = Seg{g_nextNum++, 0, false, 0, 0};
    }
    const uint16_t room = (uint16_t)(FM_LOG_SEG_RECS - s->nrec);
    const uint8_t k = (uint8_t)((n - done) < room ? (n - done) : room);
    char path[40];
    segPath(s->num, path, sizeof(path));
    bool ok = false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
      const ssize_t w = write(fd, &recs[done], (size_t)k * kRec);
      // close() is the LittleFS commit: only after it is the append durable.
      const int c = close(fd);
      ok = w == (ssize_t)((size_t)k * kRec) && c == 0;
    }
    uint8_t landed = k;
    if (!ok) {
      // Learn what really reached the file, and never append behind it again.
      struct stat st;
      const uint16_t have = stat(path, &st) == 0 ? (uint16_t)(st.st_size / (off_t)kRec) : s->nrec;
      landed = have > s->nrec ? (uint8_t)(have - s->nrec < k ? have - s->nrec : k) : 0;
      s->sealed = true;
      Serial.printf("[LOG] !! write to %s failed (errno %d), %u of %u records kept\n", path, errno,
                    (unsigned)landed, (unsigned)k);
    }
    if (landed) {
      if (s->nrec == 0) s->firstSeq = recs[done].seq;
      s->lastSeq = recs[done + landed - 1].seq;
      s->nrec = (uint16_t)(s->nrec + landed);
      done = (uint8_t)(done + landed);
    }
    if (!ok) {
      if (s->nrec == 0) {   // an empty file that failed: forget it altogether
        unlink(path);
        g_nsegs--;
      }
      if (retried) break;
      // Most likely the partition is full: make room once, then try again.
      // Files already sent (still queued for deletion) go before unsent ones.
      retried = true;
      if (g_unlinkN) unlinkQueued();
      else if (g_nsegs > 1) dropOldest("flash full");
    }
  }
  return done;
}

/** Delete every log file. Returns how many unsent records went with them. */
uint32_t clearStore() {
  const uint32_t unsent = g_waiting.load();
  while (g_nsegs) removeFirst(false);
  g_unlinkN = 0;   // queued files are in the directory: the sweep below takes them
  // Files the table did not know about (a failed scan) go too.
  DIR *d = opendir(kDir);
  if (d) {
    struct dirent *e;
    char path[300];
    while ((e = readdir(d)) != nullptr) {
      snprintf(path, sizeof(path), "%s/%s", kDir, e->d_name);
      unlink(path);
    }
    closedir(d);
  }
  g_curSlot = 0;
  updateWaiting();
  return unsent;
}

// ================================================================ records (loop task)
void flushQueue(TickType_t wait) {
  if (!g_qn) return;
  if (!g_fsOk) {
    g_dropped.fetch_add(g_qn);
    g_droppedDirty = true;
    g_qn = 0;
    return;
  }
  // The uploader holds the mutex only while it reads files or moves the
  // cursor. If it is in there now, the records wait in RAM for the next pass.
  if (xSemaphoreTake(g_mx, wait) != pdTRUE) return;
  const uint8_t landed = appendRecs(g_q, g_qn);
  updateWaiting();
  xSemaphoreGive(g_mx);
  if (landed < g_qn) {
    g_dropped.fetch_add((uint32_t)(g_qn - landed));
    g_droppedDirty = true;
    Serial.printf("[LOG] !! %u records could not be written to flash - counted as dropped\n",
                  (unsigned)(g_qn - landed));
  }
  g_qn = 0;
}

/** A zeroed record in the RAM queue with the common fields set; commit() it. */
Rec *newRec(uint8_t kind) {
  if (!g_ready || !inLoopTask()) return nullptr;
  if (g_qn >= FM_LOG_RAMQ) flushQueue(pdMS_TO_TICKS(200));   // only if the loop stalled
  if (g_qn >= FM_LOG_RAMQ) {
    g_dropped.fetch_add(1);
    g_droppedDirty = true;
    return nullptr;
  }
  Rec *r = &g_q[g_qn];
  memset(r, 0, sizeof(*r));
  r->magic = kMagic;
  r->kind = kind;
  r->seq = nextSeq();
  r->boot = g_boot;
  r->ms = millis();
  r->epoch = epochNow();
  return r;
}

void commit(Rec *r) {
  r->crc = crcOf(*r);
  g_qn++;
}

void setText(Rec *r, const char *text) {
  if (!text || !text[0]) return;
  copyPrintable(r->u.text, sizeof(r->u.text), text, false);
  r->flags |= FL_TEXT;
}

void setFrom(Rec *r, const char *from) {
  if (!from) return;
  copyPrintable(r->from, sizeof(r->from), from, true);
  if (r->from[0]) r->flags |= FL_FROM;
}

void makeBootRecord() {
  Rec *r = newRec(K_BOOT);
  if (!r) return;
  copyPrintable(r->u.boot.reason, sizeof(r->u.boot.reason), resetName(esp_reset_reason()), false);
  copyPrintable(r->u.boot.fw, sizeof(r->u.boot.fw), FLOODMESH_VERSION, false);
  copyPrintable(r->u.boot.image, sizeof(r->u.boot.image), g_image, false);
  commit(r);
}

void makeStatusRecord() {
  if (!g_statusFn) return;
  FmLogStatus s = {};
  g_statusFn(&s);
  Rec *r = newRec(K_STATUS);
  if (!r) return;
  const float mv = s.battV * 1000.0f + 0.5f;
  r->u.status.battMv = (uint16_t)(mv < 0 ? 0 : (mv > 65535.0f ? 65535.0f : mv));
  r->u.status.battPct = s.battPct;
  r->u.status.powered = s.powered ? 1 : 0;
  r->u.status.airPm = s.airPermille;
  r->u.status.heard = s.heard;
  r->u.status.sleepPct = s.sleepPct;
  r->u.status.responder = s.responder ? 1 : 0;
  commit(r);
}

void persistDropped() {
  if (!g_droppedDirty) return;
  g_droppedDirty = false;
  nvsPut(kKeyDropped, g_dropped.load());
}

/**
 * Every flash write the logger makes outside flushQueue(), in one place:
 * the dropped counter, the next seq block, and what the uploader left
 * behind (the files it finished, and the acknowledged seq once its window is
 * over: once per window, not per request, since after a crash the server
 * just sees a few records again). fmLogLoop calls it only while the mesh is
 * idle, for the same reason it holds the records back: a flash write stalls
 * both cores, and a relay or ACK counting down would go out late.
 */
void idleFlashChores() {
  persistDropped();
  if (g_seqLimit - g_seqNext < FM_LOG_SEQ_BLOCK / 2) reserveSeq();
  if (!g_chores.load()) return;
  if (xSemaphoreTake(g_mx, 0) != pdTRUE) return;   // the uploader is reading: next pass
  unlinkQueued();
  bool saveAck = false;
  const uint32_t ack = g_ackSeq;
  if (g_ackDirty && !g_busy.load()) {
    saveAck = true;
    g_ackDirty = false;
  }
  g_chores.store(g_ackDirty);   // an ack of a window still running: look again
  xSemaphoreGive(g_mx);
  if (saveAck && !nvsPut(kKeyAck, ack)) {
    Serial.println("[LOG] !! acknowledged seq not saved - a reboot re-sends some records");
  }
}

// ================================================================ JSON
/** Tenths as a JSON number, e.g. -125 -> -12.5, exactly what RXLOG printed. */
float tenths(int16_t v) { return (float)v / 10.0f; }

void recToJson(const Rec &r, JsonObject o) {
  // Casts to const char*: ArduinoJson stores a `const char (&)[N]` by address
  // as if it were a string literal. These buffers are freed before the body is
  // built, so every string from a record must be copied.
  o["seq"] = r.seq;
  o["boot"] = r.boot;
  o["ms"] = r.ms;
  o["epoch"] = r.epoch;
  o["k"] = kindName(r.kind);
  const char *t = typeName(r.t);
  switch (r.kind) {
    case K_BOOT:
      o["reason"] = (const char *)r.u.boot.reason;
      o["fw"] = (const char *)r.u.boot.fw;
      o["image"] = (const char *)r.u.boot.image;
      return;
    case K_STATUS:
      o["batt_v"] = (float)r.u.status.battMv / 1000.0f;
      o["batt_pct"] = r.u.status.battPct;
      o["powered"] = r.u.status.powered != 0;
      o["air_pm"] = r.u.status.airPm;
      o["heard"] = r.u.status.heard;
      o["sleep_pct"] = r.u.status.sleepPct;
      o["role"] = r.u.status.responder ? "responder" : "civilian";
      return;
    case K_SOS:
      o["state"] = stateName(r.state);
      if (r.flags & FL_ID) o["id"] = r.id;
      o["ch"] = r.ch;
      o["n"] = r.n;
      return;
    default:
      break;
  }
  if (t) o["t"] = t;
  if (r.flags & FL_FROM) o["from"] = (const char *)r.from;
  if (r.flags & FL_ID) o["id"] = r.id;
  if (r.flags & FL_HOPS) o["hops"] = r.hops;
  if (r.flags & FL_SIG) {
    o["rssi"] = r.rssi;
    o["snr"] = tenths(r.snr10);
  }
  if (r.flags & FL_OK) o["ok"] = (r.flags & FL_OKV) != 0;
  if (r.flags & FL_TEXT) o["text"] = (const char *)r.u.text;
  if (r.flags & FL_CH) o["ch"] = r.ch;
}

// ================================================================ uploader task
struct Batch {
  Rec      recs[FM_LOG_MAX_RECORDS];
  uint32_t afterNum[FM_LOG_MAX_RECORDS];    // position just after each record
  uint16_t afterSlot[FM_LOG_MAX_RECORDS];
  uint16_t n;
  uint16_t damaged;
  uint32_t endNum;                          // just after the last slot looked at
  uint16_t endSlot;
};

/** Up to FM_LOG_MAX_RECORDS valid records from the cursor on (caller holds g_mx). */
void readBatch(Batch &b) {
  b.n = 0;
  b.damaged = 0;
  if (!g_nsegs) return;
  uint8_t si = 0;
  uint16_t slot = g_curSlot;
  b.endNum = g_segs[0].num;
  b.endSlot = slot;
  Rec chunk[8];
  while (b.n < FM_LOG_MAX_RECORDS) {
    const Seg &s = g_segs[si];
    if (slot >= s.nrec) {
      if (si + 1 >= g_nsegs) break;
      si++;
      slot = 0;
      continue;
    }
    const uint16_t k = s.nrec - slot >= 8 ? 8 : s.nrec - slot;
    const uint16_t got = readSlots(s.num, slot, chunk, k);
    if (got == 0) {   // unreadable: step over this chunk, as the boot scan does
      b.damaged = (uint16_t)(b.damaged + k);
      slot = (uint16_t)(slot + k);
      b.endNum = s.num;
      b.endSlot = slot;
      continue;
    }
    for (uint16_t i = 0; i < got && b.n < FM_LOG_MAX_RECORDS; i++) {
      slot++;
      if (recValid(chunk[i])) {
        b.recs[b.n] = chunk[i];
        b.afterNum[b.n] = s.num;
        b.afterSlot[b.n] = slot;
        b.n++;
      } else {
        b.damaged++;
      }
      b.endNum = s.num;
      b.endSlot = slot;
    }
  }
}

/**
 * The request body for the first records of `b` that fit 16 KB. Returns a
 * malloc'd, NUL-terminated buffer (caller frees) or nullptr; *len and
 * *included are set.
 */
char *buildBody(const Batch &b, size_t *len, uint16_t *included) {
  *len = 0;
  *included = 0;
  JsonDocument doc;
  doc["v"] = 1;
  doc["unit"] = (const char *)g_job.unit;
  doc["mac"] = (const char *)g_mac;
  doc["fw"] = FLOODMESH_VERSION;
  doc["image"] = (const char *)g_image;
  doc["boot"] = g_boot;
  // up_ms is "when the request was built" (§2): the server subtracts each
  // record's ms from it, so it is taken right before serializing.
  doc["up_ms"] = (uint32_t)millis();
  doc["epoch"] = epochNow();
  doc["dropped"] = g_dropped.load();
  JsonArray arr = doc["records"].to<JsonArray>();
  size_t size = measureJson(doc);
  uint16_t n = 0;
  for (uint16_t i = 0; i < b.n; i++) {
    JsonObject o = arr.add<JsonObject>();
    recToJson(b.recs[i], o);
    const size_t add = measureJson(o) + (n ? 1 : 0);   // plus the comma
    if (size + add > FM_LOG_MAX_BODY) {
      arr.remove(n);
      break;
    }
    size += add;
    n++;
  }
  if (doc.overflowed() || n == 0) return nullptr;
  char *buf = (char *)malloc(size + 1);
  if (!buf) return nullptr;
  const size_t w = serializeJson(doc, buf, size + 1);
  if (w != size) {   // measure and serialize disagree: never send a cut body
    free(buf);
    return nullptr;
  }
  *len = w;
  *included = n;
  return buf;
}

bool parseUrl(const char *url, bool *secure, char *host, size_t hn, uint16_t *port) {
  const char *p;
  if (strncmp(url, "https://", 8) == 0) {
    *secure = true;
    *port = 443;
    p = url + 8;
  } else if (strncmp(url, "http://", 7) == 0) {
    *secure = false;
    *port = 80;
    p = url + 7;
  } else {
    return false;
  }
  size_t i = 0;
  while (p[i] && p[i] != ':' && p[i] != '/') {
    if (i + 1 >= hn) return false;
    host[i] = p[i];
    i++;
  }
  host[i] = '\0';
  if (i == 0) return false;
  if (p[i] == ':') {
    char *end = nullptr;
    const long v = strtol(p + i + 1, &end, 10);
    if (v <= 0 || v > 65535 || (end && *end && *end != '/')) return false;
    *port = (uint16_t)v;
  }
  return true;
}

uint32_t rd32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/**
 * One SNTP request (RFC 4330) over UDP. Not configTime(): the lwIP client
 * would set the system clock itself, possibly backwards (fm_role moves it
 * forward only, for responder expiry), and keep polling after WiFi is off.
 * Returns the server's Unix ms at the moment the reply arrived.
 */
bool sntpQuery(const char *server, int64_t *unixMsNow) {
  IPAddress ip;
  if (!WiFi.hostByName(server, ip)) return false;
  WiFiUDP udp;
  if (!udp.begin((uint16_t)(40000 + esp_random() % 20000))) return false;
  uint8_t pkt[48] = {};
  pkt[0] = 0x23;   // LI 0, version 4, mode 3 (client)
  uint8_t nonce[8];
  esp_fill_random(nonce, sizeof(nonce));
  memcpy(pkt + 40, nonce, sizeof(nonce));   // echoed back as "originate": proves it answers us
  const int64_t t0 = esp_timer_get_time();
  bool got = false;
  if (udp.beginPacket(ip, 123) && udp.write(pkt, sizeof(pkt)) == sizeof(pkt) && udp.endPacket()) {
    while (esp_timer_get_time() - t0 < (int64_t)FM_LOG_NTP_MS * 1000) {
      if (udp.parsePacket() >= (int)sizeof(pkt) && udp.read(pkt, sizeof(pkt)) == (int)sizeof(pkt)) {
        got = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  const int64_t t1 = esp_timer_get_time();
  udp.stop();
  if (!got) return false;
  const uint8_t mode = pkt[0] & 0x07, stratum = pkt[1];
  if ((mode != 4 && mode != 5) || stratum == 0 || stratum > 15) return false;
  if (memcmp(pkt + 24, nonce, sizeof(nonce)) != 0) return false;
  uint64_t secs = rd32be(pkt + 40);
  if (secs < 2208988800ULL) secs += 0x100000000ULL;   // NTP era 1, from 2036
  const uint64_t frac = rd32be(pkt + 44);
  int64_t ms = (int64_t)(secs - 2208988800ULL) * 1000 + (int64_t)((frac * 1000ULL) >> 32);
  ms += (t1 - t0) / 2000;   // the reply was stamped about half a round trip ago
  if (!plausibleUnixMs(ms)) return false;
  *unixMsNow = ms;
  return true;
}

/** SNTP: re-anchors the records' clock and, if it answers, votes for the V4 clock. */
void syncSntp(ClockVotes *votes) {
  static const char *const kServers[] = {"pool.ntp.org", "time.google.com"};
  for (const char *s : kServers) {
    int64_t ms = 0;
    if (sntpQuery(s, &ms)) {
      snprintf(votes->sntpSrc, sizeof(votes->sntpSrc), "SNTP %s", s);
      votes->sntpOff = setAnchor(ms, votes->sntpSrc);
      votes->sntp = true;
      return;
    }
    Serial.printf("[LOG] no SNTP answer from %s\n", s);
  }
}

bool joinNetwork(const char *ssid, const char *pass) {
  WiFi.begin(ssid, pass[0] ? pass : nullptr);
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < FM_LOG_JOIN_MS) {
    if (WiFi.status() == WL_CONNECTED) return true;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  WiFi.disconnect();
  vTaskDelay(pdMS_TO_TICKS(200));
  return false;
}

void noteHeap(uint32_t *minFree) {
  const uint32_t f = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (f < *minFree) *minFree = f;
}

/**
 * WiFiClientSecure, except that stop() leaves the socket field at -1. In core
 * 2.0.17 stop() ends by zeroing its whole context (stop_ssl_socket), so the
 * socket reads 0 afterwards and the next stop() runs close(0). Every window
 * has such a second stop (HTTPClient's destructor, then the client's own).
 * fd 0 is the console's stdin until the first of them closes it; from then on
 * it is whatever LittleFS opens next, possibly a segment file the loop task is
 * appending to on the other core at that moment.
 */
class FmTlsClient : public WiFiClientSecure {
 public:
  void stop() override {
    WiFiClientSecure::stop();
    sslclient->socket = -1;
  }
};

/**
 * Open the connection to the server; on failure *result says why. No stop()
 * first: a TLS client that reports "not connected" has already stopped, and
 * a plain one closes its old socket when connect() replaces it.
 */
bool connectServer(WiFiClient *cli, bool secure, const char *host, uint16_t port, char *result,
                   size_t rn) {
  if (!cli->connect(host, port, (int32_t)FM_LOG_HTTP_MS)) {
    snprintf(result, rn, "could not connect to %s:%u%s", host, (unsigned)port,
             secure ? " (TLS: network, or the certificate did not verify)" : "");
    return false;
  }
  // HTTPClient only sets this when it connects itself; it bounds every read.
  cli->setTimeout(FM_LOG_HTTP_MS / 1000);
  return true;
}

/**
 * Send batches until nothing is left or something fails. Returns true if at
 * least one request was accepted. *result says what happened last; *votes
 * gets the server's time if it came over verified HTTPS.
 */
bool uploadAll(bool secure, const char *host, uint16_t port, ClockVotes *votes, char *result,
               size_t rn, uint32_t *sentOut, uint16_t *reqOut, uint32_t *heapMin) {
  char endpoint[kUrlMax + 16];
  snprintf(endpoint, sizeof(endpoint), "%s/api/ingest", g_job.url);
  // Only a server whose certificate was checked may vote for the V4 clock.
  const bool verified = secure && !FM_LOG_TLS_INSECURE;

  WiFiClient plain;
  FmTlsClient tls;
  WiFiClient *cli = &plain;
  if (secure) {
#if FM_LOG_TLS_INSECURE
    tls.setInsecure();
#else
    tls.setCACertBundle(kFmCaBundle);
#endif
    tls.setHandshakeTimeout(FM_LOG_HTTP_MS / 1000);
    cli = &tls;
  }

  Batch *b = (Batch *)malloc(sizeof(Batch));
  if (!b) {
    snprintf(result, rn, "no memory for a batch (%u bytes)", (unsigned)sizeof(Batch));
    return false;
  }
  // Declared after the clients: its destructor calls stop() on the one it holds.
  HTTPClient http;
  http.setReuse(true);
  http.setConnectTimeout((int32_t)FM_LOG_HTTP_MS);
  http.setTimeout((uint16_t)FM_LOG_HTTP_MS);
  // begin() before the first connect. In core 2.0.17 begin() closes a connected
  // client whenever the host differs from the last one it saw (beginInternal:
  // "switching host"), and that starts out empty: a begin() after our connect
  // would close the TLS session just opened, and POST() would open a second
  // one, after up_ms was stamped. With the host already set, the begin() of
  // each request keeps the connection.
  if (!http.begin(*cli, endpoint)) {
    free(b);
    snprintf(result, rn, "bad server address '%s'", g_job.url);
    return false;
  }

  bool anyOk = false;
  bool serverTimeSeen = false;
  uint32_t sent = 0;
  uint16_t req = 0;
  snprintf(result, rn, "nothing to send");
  while (req < FM_LOG_MAX_REQUESTS) {
    xSemaphoreTake(g_mx, portMAX_DELAY);
    readBatch(*b);
    if (b->n == 0 && b->damaged) advanceTo(b->endNum, b->endSlot);   // only damaged ones left
    xSemaphoreGive(g_mx);
    if (b->damaged) {
      Serial.printf("[LOG] %u damaged record(s) skipped (CRC)\n", (unsigned)b->damaged);
    }
    if (b->n == 0) break;

    // (Re)connect before building the body, so the TLS handshake never sits
    // between up_ms and the server's receive time (§5 derives record times
    // from both). The first request connects here; later ones only if the
    // server closed the kept-alive connection, which HTTPClient would
    // otherwise reopen inside POST(), after the body was stamped.
    if (!cli->connected()) {
      if (!connectServer(cli, secure, host, port, result, rn)) break;
      noteHeap(heapMin);
    }

    size_t len = 0;
    uint16_t inc = 0;
    char *body = buildBody(*b, &len, &inc);
    if (!body) {
      snprintf(result, rn, "could not build the request (heap %u)",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      break;
    }
    uint8_t mac[32];
    fmHmacSha256(g_key, g_keyLen, reinterpret_cast<const uint8_t *>(body), len, mac);
    char sig[65];
    fmToHex(mac, sizeof(mac), sig);

    http.begin(*cli, endpoint);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-FM-Unit", g_job.unit);
    http.addHeader("X-FM-Signature", sig);
    const int code = http.POST(reinterpret_cast<uint8_t *>(body), len);
    free(body);
    noteHeap(heapMin);
    req++;
    const uint32_t firstSeq = b->recs[0].seq, lastSeq = b->recs[inc - 1].seq;
    if (code != 200) {
      if (code > 0) {
        snprintf(result, rn, "HTTP %d%s", code,
                 code == 401   ? " (bad signature, or unit not allowed)"
                 : code == 413 ? " (body over 16 KB)"
                 : code == 400 ? " (server rejected the body)"
                               : "");
      } else {
        snprintf(result, rn, "HTTP failed: %s", HTTPClient::errorToString(code).c_str());
      }
      Serial.printf("[LOG] POST %u records (seq %lu-%lu, %u B) -> %s\n", (unsigned)inc,
                    (unsigned long)firstSeq, (unsigned long)lastSeq, (unsigned)len, result);
      break;
    }
    const String resp = http.getString();
    JsonDocument rd;
    const DeserializationError err = deserializeJson(rd, resp);
    const bool okFlag = !err && (rd["ok"] | false);
    if (!okFlag || !rd["acked_seq"].is<uint32_t>()) {
      snprintf(result, rn, "HTTP 200 but not {\"ok\":true,\"acked_seq\":...}");
      Serial.printf("[LOG] POST %u records -> %s\n", (unsigned)inc, result);
      break;
    }
    const uint32_t acked = rd["acked_seq"].as<uint32_t>();
    if (!serverTimeSeen && rd["server_epoch"].is<double>()) {
      const int64_t ms = (int64_t)(rd["server_epoch"].as<double>() * 1000.0);
      if (plausibleUnixMs(ms)) {
        serverTimeSeen = true;
        // The records' anchor takes it only if SNTP failed (SNTP is finer than
        // 1 s); the V4 clock only hears it from a verified server.
        if (!votes->sntp) setAnchor(ms, "server_epoch");
        if (verified) {
          votes->https = true;
          votes->httpsOff = ms - nowMs64();
        }
      }
    }

    // Only records that were in THIS request are ever deleted, whatever
    // acked_seq says.
    int last = -1;
    for (uint16_t i = 0; i < inc; i++) {
      if (b->recs[i].seq <= acked) last = i;
    }
    if (last < 0) {
      snprintf(result, rn, "server acknowledged none of seq %lu-%lu (acked_seq %lu)",
               (unsigned long)firstSeq, (unsigned long)lastSeq, (unsigned long)acked);
      Serial.printf("[LOG] %s\n", result);
      break;
    }
    const bool all = last == inc - 1;
    xSemaphoreTake(g_mx, portMAX_DELAY);
    if (all && inc == b->n) advanceTo(b->endNum, b->endSlot);
    else advanceTo(b->afterNum[last], b->afterSlot[last]);
    const uint32_t ackedSeq = b->recs[last].seq;
    if (ackedSeq > g_ackSeq) {
      g_ackSeq = ackedSeq;
      g_ackDirty = true;
    }
    g_st.sentBoot += (uint32_t)(last + 1);
    xSemaphoreGive(g_mx);
    sent += (uint32_t)(last + 1);
    anyOk = true;
    Serial.printf("[LOG] POST %u records (seq %lu-%lu, %u B) -> 200, acked %lu, %lu waiting\n",
                  (unsigned)inc, (unsigned long)firstSeq, (unsigned long)lastSeq, (unsigned)len,
                  (unsigned long)acked, (unsigned long)g_waiting.load());
    snprintf(result, rn, "OK: %u request(s), %lu records, acked seq %lu", (unsigned)req,
             (unsigned long)sent, (unsigned long)ackedSeq);
    if (!all) {   // the server kept only part of it: try the rest next window
      snprintf(result, rn, "server kept %d of %u records (acked_seq %lu)", last + 1,
               (unsigned)inc, (unsigned long)acked);
      break;
    }
  }
  http.end();
  cli->stop();
  free(b);
  *sentOut = sent;
  *reqOut = req;
  return anyOk;
}

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

/** One upload window, in the uploader task. */
void runWindow() {
  const uint32_t t0 = millis();
  char result[160] = "";
  uint32_t heapMin = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t sent = 0;
  uint16_t req = 0;
  bool ok = false;

  bool secure = false;
  char host[72];
  uint16_t port = 0;
  if (!parseUrl(g_job.url, &secure, host, sizeof(host), &port)) {
    snprintf(result, sizeof(result), "bad server address '%s'", g_job.url);
  } else if (heapMin < FM_LOG_MIN_HEAP) {
    snprintf(result, sizeof(result), "skipped: %lu B of heap free, WiFi + TLS need %lu",
             (unsigned long)heapMin, (unsigned long)FM_LOG_MIN_HEAP);
  } else {
    char ssid[2][33], pass[2][64];
    const bool have[2] = {loadNet(FM_NVS_KEY_WIFI_SSID, FM_NVS_KEY_WIFI_PASS, ssid[0], pass[0]),
                          loadNet(kKeySsid2, kKeyPass2, ssid[1], pass[1])};
    WiFi.persistent(false);            // the credentials live in our NVS keys only
    WiFi.setHostname(g_job.host);      // before mode(), as in fm_ota
    WiFi.mode(WIFI_STA);
    int joined = -1;
    for (int i = 0; i < 2 && joined < 0; i++) {
      if (!have[i]) continue;
      Serial.printf("[LOG] upload window: joining network %d '%s'\n", i + 1, ssid[i]);
      if (joinNetwork(ssid[i], pass[i])) joined = i;
      else Serial.printf("[LOG] could not join '%s' (WiFi status %d)\n", ssid[i], (int)WiFi.status());
    }
    memset(pass, 0, sizeof(pass));
    noteHeap(&heapMin);
    if (joined < 0) {
      if (have[0] && have[1]) {
        snprintf(result, sizeof(result), "WiFi: could not join '%s' or '%s'", ssid[0], ssid[1]);
      } else if (have[0] || have[1]) {
        snprintf(result, sizeof(result), "WiFi: could not join '%s'", ssid[have[0] ? 0 : 1]);
      } else {
        snprintf(result, sizeof(result), "WiFi: no network stored");
      }
    } else {
      Serial.printf("[LOG] on '%s' as %s, %d dBm\n", ssid[joined],
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      ClockVotes votes = {};
      syncSntp(&votes);
      ok = uploadAll(secure, host, port, &votes, result, sizeof(result), &sent, &req, &heapMin);
      feedV4Clock(votes);
    }
    wifiOff();
  }

  const uint32_t stackFree = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);   // bytes on ESP-IDF
  xSemaphoreTake(g_mx, portMAX_DELAY);
  snprintf(g_st.result, sizeof(g_st.result), "%s", result);
  g_st.tried = true;
  g_st.tryMs = millis();
  if (ok) g_st.windowsOk++;
  g_st.stackFree = stackFree;
  if (g_st.heapMin == 0 || heapMin < g_st.heapMin) g_st.heapMin = heapMin;
  // The acknowledged seq goes to NVS from fmLogLoop, while the mesh is idle
  // (idleFlashChores), not from this task.
  if (g_ackDirty) g_chores.store(true);
  xSemaphoreGive(g_mx);
  if (ok) {
    g_lastOkMs.store(millis());
    g_everOk.store(true);
  }
  Serial.printf("[LOG] upload window %s in %lu s: %s. WiFi off\n", ok ? "done" : "FAILED",
                (unsigned long)((millis() - t0) / 1000), result);
}

void uploaderTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    runWindow();
    g_busy.store(false);
  }
}

// ================================================================ scheduling (loop task)
bool net1Set() { return fmOtaHasSsid() && fmOtaHasPass(); }
bool net2Set() { return g_ssid2[0] != '\0' && g_hasPass2; }

/** Why no window can run, or nullptr if one can. */
const char *blocker() {
  if (!g_fsOk) return "the flash store did not mount";
  if (!g_task) return "the uploader task did not start";
  if (!g_keyLen) return "this build has no FM_LOG_KEY";
  if (!g_url[0]) return "no server address: 'log url <base>' or FM_LOG_URL";
  if (!net1Set() && !net2Set()) return "no WiFi network: 'wifi ssid/pass' or 'wifi2 ssid/pass'";
  return nullptr;
}

void startWindow(uint32_t now) {
  snprintf(g_job.url, sizeof(g_job.url), "%s", g_url);
  copyPrintable(g_job.unit, sizeof(g_job.unit), fmCallSign(), true);
  snprintf(g_job.host, sizeof(g_job.host), "fm-%s", g_job.unit);
  for (char *p = g_job.host; *p; p++) *p = (char)tolower((unsigned char)*p);
  g_nextUpload = now + FM_LOG_PERIOD_MS;
  g_busy.store(true);   // before the notify: canSleep() must see it this pass
  xTaskNotifyGive(g_task);
}

bool setUrl(const char *url) {
  char u[kUrlMax + 1];
  const size_t n = strlen(url);
  if (n == 0 || n > kUrlMax || !printable(url) || strchr(url, ' ') || strchr(url, '@') ||
      strchr(url, '?') || strchr(url, '#')) {
    return false;
  }
  snprintf(u, sizeof(u), "%s", url);
  for (size_t i = strlen(u); i > 0 && u[i - 1] == '/'; i--) u[i - 1] = '\0';
  bool secure;
  char host[72];
  uint16_t port;
  if (!parseUrl(u, &secure, host, sizeof(host), &port)) return false;
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, false)) return false;
  const bool ok = p.putString(kKeyUrl, u) == strlen(u);
  p.end();
  if (!ok) return false;
  snprintf(g_url, sizeof(g_url), "%s", u);
  g_urlFromNvs = true;
  return true;
}

void loadSettings() {
  Preferences p;
  g_url[0] = '\0';
  g_urlFromNvs = false;
  if (p.begin(FM_NVS_NAMESPACE, true)) {
    if (p.isKey(kKeyUrl)) {
      p.getString(kKeyUrl, g_url, sizeof(g_url));
      g_urlFromNvs = g_url[0] != '\0';
    }
    g_ssid2[0] = '\0';
    if (p.isKey(kKeySsid2)) p.getString(kKeySsid2, g_ssid2, sizeof(g_ssid2));
    g_hasPass2 = p.isKey(kKeyPass2);
    p.end();
  }
  if (!g_urlFromNvs) {
    char u[kUrlMax + 1];
    snprintf(u, sizeof(u), "%s", FM_LOG_URL);
    for (size_t i = strlen(u); i > 0 && u[i - 1] == '/'; i--) u[i - 1] = '\0';
    snprintf(g_url, sizeof(g_url), "%s", u);
  }
}

void parseKey() {
  const char *h = FM_LOG_KEY;
  const size_t n = strlen(h);
  g_keyLen = 0;
  if (n == 0) return;
  if (n < 32 || n > 64 || (n % 2) != 0 || !fmFromHex(h, g_key, n / 2)) {
    Serial.println("[LOG] !! FM_LOG_KEY is malformed (need 32-64 hex characters) - uploads off");
    return;
  }
  g_keyLen = n / 2;
}

void printStatus() {
  Stats st = {};
  uint32_t stored = 0;
  uint8_t files = 0;
  if (g_mx && xSemaphoreTake(g_mx, pdMS_TO_TICKS(500)) == pdTRUE) {
    st = g_st;
    for (uint8_t i = 0; i < g_nsegs; i++) stored += g_segs[i].nrec;
    files = g_nsegs;
    xSemaphoreGive(g_mx);
  }
  const uint32_t now = millis();
  Serial.printf("[LOG] %lu records in flash (%u files, room for %lu), %lu waiting to upload, "
                "%lu dropped in all; boot %lu, next seq %lu\n",
                (unsigned long)(stored + g_qn), (unsigned)files,
                (unsigned long)(FM_LOG_SEGMENTS * FM_LOG_SEG_RECS),
                (unsigned long)(g_waiting.load() + g_qn), (unsigned long)g_dropped.load(),
                (unsigned long)g_boot, (unsigned long)g_seqNext);
  char age[8], age2[8];
  fmtAge(now - g_lastOkMs.load(), age, sizeof(age));
  fmtAge(now - st.tryMs, age2, sizeof(age2));
  Serial.printf("[LOG] %lu sent since boot; last good upload %s%s; last try %s%s%s%s\n",
                (unsigned long)st.sentBoot, g_everOk.load() ? age : "never",
                g_everOk.load() ? " ago" : "", st.tried ? age2 : "never", st.tried ? " ago: " : "",
                st.tried ? st.result : "", g_busy.load() ? " (uploading now)" : "");
  char next[8];
  fmtAge((int32_t)(g_nextUpload - now) > 0 ? g_nextUpload - now : 0, next, sizeof(next));
  const char *why = blocker();
  Serial.printf("[LOG] server %s (%s), key %s, every %lu s; %s%s\n",
                g_url[0] ? g_url : "not set", g_urlFromNvs ? "set by 'log url'" : "build default",
                g_keyLen ? "set" : "NOT SET", (unsigned long)(FM_LOG_PERIOD_MS / 1000),
                why ? "no uploads: " : "next window in ", why ? why : next);
  portENTER_CRITICAL(&g_timeMux);
  const bool tOk = g_timeOk;
  const uint32_t tAt = g_timeSyncMs;
  char src[20];
  memcpy(src, g_timeSrc, sizeof(src));
  portEXIT_CRITICAL(&g_timeMux);
  if (tOk) {
    fmtAge(now - tAt, age, sizeof(age));
    Serial.printf("[LOG] clock from %s %s ago, now %lu\n", src, age, (unsigned long)epochNow());
  } else {
    Serial.println("[LOG] clock not synced this boot: records carry epoch 0 (the server "
                   "dates them from the uptime)");
  }
  Serial.printf("[LOG] flash %u of %u KB used; uploader stack margin %lu B, lowest heap in a "
                "window %lu B%s\n",
                (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024),
                (unsigned long)st.stackFree, (unsigned long)st.heapMin,
                FM_LOG_TLS_INSECURE ? "; TLS NOT VERIFIED (FM_LOG_TLS_INSECURE)" : "");
}

}  // namespace

// ================================================================ API
void fmLogBegin(FmLogStatusFn status) {
  g_loopTask = xTaskGetCurrentTaskHandle();
  g_statusFn = status;
  g_mx = xSemaphoreCreateMutex();

  uint8_t m[6] = {};
  esp_efuse_mac_get_default(m);
  snprintf(g_mac, sizeof(g_mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4],
           m[5]);
  const esp_app_desc_t *d = esp_ota_get_app_description();
  snprintf(g_image, sizeof(g_image), "%02x%02x%02x%02x", d->app_elf_sha256[0],
           d->app_elf_sha256[1], d->app_elf_sha256[2], d->app_elf_sha256[3]);
  parseKey();
  loadSettings();

  g_boot = nvsGet(kKeyBoot, 0) + 1;
  if (!nvsPut(kKeyBoot, g_boot)) Serial.println("[LOG] !! boot counter not saved");
  g_ackSeq = nvsGet(kKeyAck, 0);
  g_dropped.store(nvsGet(kKeyDropped, 0));

  // Mount without formatting first, so a format (which erases the partition)
  // is never silent. It happens on first use, or when the partition held
  // something else (SPIFFS from another firmware).
  g_fsOk = LittleFS.begin(false, kBase, 4, kPart);
  if (!g_fsOk) {
    Serial.println("[LOG] flash store not mountable - formatting the 'spiffs' partition");
    g_fsOk = LittleFS.begin(true, kBase, 4, kPart);
  }
  uint32_t lastSeq = 0;
  if (g_fsOk && g_mx) {
    xSemaphoreTake(g_mx, portMAX_DELAY);
    lastSeq = scanStore();
    xSemaphoreGive(g_mx);
  } else {
    g_fsOk = false;
    Serial.println("[LOG] !! flash store unavailable - records are counted as dropped, not kept");
  }

  // seq never goes backwards: above the saved mark AND above anything in flash.
  g_seqNext = nvsGet(kKeySeq, 1);
  if (lastSeq + 1 > g_seqNext) g_seqNext = lastSeq + 1;
  if (g_seqNext == 0) g_seqNext = 1;
  g_seqLimit = g_seqNext;
  reserveSeq();   // now, while nothing is on the air; fmLogLoop keeps it topped up

  if (g_mx && xTaskCreatePinnedToCore(uploaderTask, "fmlog", FM_LOG_TASK_STACK, nullptr, 1,
                                      &g_task, 0) != pdPASS) {
    g_task = nullptr;
    Serial.println("[LOG] !! uploader task not started (heap?) - recording only");
  }
  g_ready = true;
  makeBootRecord();
  g_nextStatus = millis();                       // first status record on the first loop pass
  g_nextUpload = millis() + FM_LOG_FIRST_MS;

  Serial.printf("[LOG] field logger: boot %lu, %lu records waiting, %lu dropped in all, "
                "next seq %lu, mac %s, image %s\n",
                (unsigned long)g_boot, (unsigned long)g_waiting.load(),
                (unsigned long)g_dropped.load(), (unsigned long)g_seqNext, g_mac, g_image);
  // Said once, as the protocol asks (§6).
  if (!g_keyLen) {
    Serial.println("[LOG] no FM_LOG_KEY in this build: recording to flash only, never uploading");
  } else if (const char *why = blocker()) {
    Serial.printf("[LOG] uploads waiting for setup: %s\n", why);
  } else {
    Serial.printf("[LOG] uploads to %s every %lu s, first in %lu s\n", g_url,
                  (unsigned long)(FM_LOG_PERIOD_MS / 1000), (unsigned long)(FM_LOG_FIRST_MS / 1000));
  }
#if FM_LOG_TLS_INSECURE
  Serial.println("[LOG] !! FM_LOG_TLS_INSECURE: the server certificate is NOT checked. Bench only.");
#endif
}

void fmLogLoop(uint32_t now) {
  if (!g_ready) return;
  if ((int32_t)(now - g_nextStatus) >= 0) {
    g_nextStatus = now + FM_LOG_STATUS_MS;
    makeStatusRecord();
  }
  // A flash write stalls both cores for tens of ms. While a relay or an ACK is
  // counting down its contention delay, that stall would shift it and skew the
  // very mesh timing a field test measures, so the records wait in RAM until
  // the mesh is idle (well under a second), unless the queue is filling up.
  // Every other flash write of the logger waits the same way, the uploader
  // task's included: it cannot see the mesh, so it leaves them to this task.
  const bool meshIdle = !fmMeshBusy();
  if (meshIdle || g_qn >= FM_LOG_RAMQ / 2) flushQueue(pdMS_TO_TICKS(20));
  if (meshIdle) idleFlashChores();

  if (g_busy.load() || (int32_t)(now - g_nextUpload) < 0 || blocker()) return;
  if (fmOtaActive()) {
    g_nextUpload = now + 30000UL;   // WiFi update mode has WiFi; look again soon
    return;
  }
  if (g_qn) return;   // the uploader reads flash only: let this pass's records land first
  if (g_waiting.load() == 0) {
    g_nextUpload = now + FM_LOG_PERIOD_MS;
    return;
  }
  startWindow(now);
}

// ---------------------------------------------------------------- hooks
void fmLogRx(FmLogType t, const char *from, uint8_t id, uint8_t hops, float rssi, float snr,
             const char *text, int ch) {
  Rec *r = newRec(K_RX);
  if (!r) return;
  r->t = t;
  setFrom(r, from);
  r->id = id;
  r->hops = hops;
  r->rssi = rssiAsLogged(rssi);
  r->snr10 = snr10AsLogged(snr);
  r->flags |= FL_ID | FL_HOPS | FL_SIG;
  setText(r, text);
  if (ch >= 0) {
    r->ch = (uint8_t)ch;
    r->flags |= FL_CH;
  }
  commit(r);
}

void fmLogEcho(uint8_t frameType, uint8_t id, float rssi, float snr) {
  Rec *r = newRec(K_ECHO);
  if (!r) return;
  uint8_t t = FM_LOG_T_NONE;
  for (uint8_t i = 0; i < sizeof(g_sent) / sizeof(g_sent[0]); i++) {
    if (g_sent[i].frameType == frameType && g_sent[i].id == id && g_sent[i].t) t = g_sent[i].t;
  }
  if (t == FM_LOG_T_NONE) {
    switch (frameType) {
      case FM_TYPE_ALARM:     t = FM_LOG_T_SOS; break;   // a V4 unit only sends SOS alarms
      case FM_TYPE_TEXT:      t = FM_LOG_T_TEXT; break;
      case FM_TYPE_ACK:       t = FM_LOG_T_ACK; break;
      case FM_TYPE_HEARTBEAT: t = FM_LOG_T_HEARTBEAT; break;
      default:                break;
    }
  }
  r->t = t;
  r->id = id;
  r->rssi = rssiAsLogged(rssi);
  r->snr10 = snr10AsLogged(snr);
  r->flags |= FL_ID | FL_SIG;
  commit(r);
}

void fmLogTx(FmLogType t, int id, bool ok, const char *text, int ch) {
  Rec *r = newRec(K_TX);
  if (!r) return;
  r->t = t;
  if (id >= 0) {
    r->id = (uint16_t)id;
    r->flags |= FL_ID;
    Sent &s = g_sent[g_sentHead];
    g_sentHead = (uint8_t)((g_sentHead + 1) % (sizeof(g_sent) / sizeof(g_sent[0])));
    s.frameType = frameTypeOf(t);
    s.id = (uint8_t)id;
    s.t = t;
  }
  r->flags |= FL_OK | (ok ? FL_OKV : 0);
  setText(r, text);
  if (ch >= 0) {
    r->ch = (uint8_t)ch;
    r->flags |= FL_CH;
  }
  commit(r);
}

void fmLogRelay(const uint8_t *frame, size_t len, const char *from, uint8_t id) {
  Rec *r = newRec(K_RELAY);
  if (!r) return;
  r->t = fmLogFrameType(frame, len);
  setFrom(r, from);
  r->id = id;
  r->flags |= FL_ID;
  commit(r);
}

void fmLogSos(FmLogSosState state, int id, uint8_t ch, uint8_t n) {
  Rec *r = newRec(K_SOS);
  if (!r) return;
  r->state = state;
  if (id >= 0) {
    r->id = (uint16_t)id;
    r->flags |= FL_ID;
  }
  r->ch = ch;
  r->n = n;
  r->flags |= FL_CH;
  commit(r);
}

FmLogType fmLogFrameType(const uint8_t *frame, size_t len) {
  if (!frame || len <= FM_OFF_COMMON_END) return FM_LOG_T_NONE;
  switch (fmFrameType(frame)) {
    case FM_TYPE_ALARM: {
      uint8_t ch;
      return fmAlarmSosChannel(frame[FM_OFF_ALARMTYPE], &ch) ? FM_LOG_T_SOS : FM_LOG_T_ALARM;
    }
    case FM_TYPE_TEXT: {
      // Only the first 5 characters are needed to tell a range-test ping.
      if (frame[FM_OFF_TEXTLEN] < 5 || len < FM_OFF_TEXT + fmTextPackedBytes(5)) return FM_LOG_T_TEXT;
      char head[6];
      fmTextUnpack(frame + FM_OFF_TEXT, 5, head);
      return strcmp(head, "PING ") == 0 ? FM_LOG_T_PING : FM_LOG_T_TEXT;
    }
    case FM_TYPE_ACK:       return FM_LOG_T_ACK;
    case FM_TYPE_HEARTBEAT: return FM_LOG_T_HEARTBEAT;
    default:                return FM_LOG_T_NONE;
  }
}

// ---------------------------------------------------------------- state
bool fmLogBusy() { return g_busy.load(); }

uint32_t fmLogNextInMs(uint32_t now) {
  if (!g_ready) return UINT32_MAX;
  if (g_busy.load() || g_qn || g_chores.load()) return 0;
  uint32_t in = (int32_t)(g_nextStatus - now) > 0 ? g_nextStatus - now : 0;
  if (!blocker()) {
    const uint32_t up = (int32_t)(g_nextUpload - now) > 0 ? g_nextUpload - now : 0;
    if (up < in) in = up;
  }
  return in;
}

void fmLogStatusLine(char *out, size_t n, uint32_t now) {
  if (!n) return;
  const unsigned long w = (unsigned long)(g_waiting.load() + g_qn);
  if (!g_ready) {
    snprintf(out, n, "LOG off");
  } else if (!g_fsOk) {
    snprintf(out, n, "LOG flash error: not recording");
  } else if (!g_keyLen) {
    snprintf(out, n, "LOG %lu kept, no key: no upload", w);
  } else if (g_busy.load()) {
    snprintf(out, n, "LOG %lu waiting, uploading...", w);
  } else if (g_everOk.load()) {
    char age[8];
    fmtAge(now - g_lastOkMs.load(), age, sizeof(age));
    snprintf(out, n, "LOG %lu waiting, sent %s ago", w, age);
  } else {
    snprintf(out, n, "LOG %lu waiting, never sent", w);
  }
}

// ---------------------------------------------------------------- serial
bool fmLogCommand(const char *line) {
  if (strcmp(line, "log") != 0 && strncmp(line, "log ", 4) != 0) return false;
  if (!g_ready) {
    Serial.println("[LOG] field logger not running");
    return true;
  }
  const char *arg = line + 3;
  while (*arg == ' ') arg++;
  if (!*arg) {
    printStatus();
  } else if (strcmp(arg, "send") == 0) {
    if (g_busy.load()) {
      Serial.println("[LOG] already uploading");
    } else if (const char *why = blocker()) {
      Serial.printf("[LOG] cannot upload: %s\n", why);
    } else if (fmOtaActive()) {
      Serial.println("[LOG] WiFi update mode is open - 'ota off' first");
    } else {
      makeStatusRecord();   // so there is always something fresh to send
      flushQueue(pdMS_TO_TICKS(200));
      Serial.printf("[LOG] uploading %lu records now\n", (unsigned long)g_waiting.load());
      startWindow(millis());
    }
  } else if (strcmp(arg, "url") == 0) {
    Serial.printf("[LOG] server %s (%s)\n", g_url[0] ? g_url : "not set",
                  g_urlFromNvs ? "set by 'log url'" : "build default");
  } else if (strcmp(arg, "url default") == 0) {
    Preferences p;
    if (p.begin(FM_NVS_NAMESPACE, false)) {
      p.remove(kKeyUrl);
      p.end();
    }
    loadSettings();
    Serial.printf("[LOG] server back to the build default: %s\n", g_url[0] ? g_url : "none");
  } else if (strncmp(arg, "url ", 4) == 0) {
    const char *u = arg + 4;
    while (*u == ' ') u++;
    if (setUrl(u)) Serial.printf("[LOG] server set to %s (uploads go to %s/api/ingest)\n", g_url, g_url);
    else Serial.println("[LOG] refused - need http://host[:port] or https://host[:port], up to 120 characters");
  } else if (strcmp(arg, "clear") == 0) {
    if (g_busy.load()) {
      Serial.println("[LOG] uploading - try again when the window has closed");
    } else if (g_mx && xSemaphoreTake(g_mx, pdMS_TO_TICKS(1000)) == pdTRUE) {
      const uint32_t unsent = clearStore() + g_qn;
      g_qn = 0;
      xSemaphoreGive(g_mx);
      Serial.printf("[LOG] log cleared: %lu unsent records discarded (not counted as dropped); "
                    "seq and boot counters continue\n",
                    (unsigned long)unsent);
    } else {
      Serial.println("[LOG] the store is busy - try again");
    }
  } else {
    Serial.println("[LOG] usage: log | log send | log url <base> | log url default | log clear");
  }
  return true;
}

bool fmLogWifi2Command(const char *line) {
  if (strncmp(line, "wifi2", 5) != 0) return false;
  // Every "wifi2..." line ends here and is never echoed: it may hold a password.
  const char *arg = line + 5;
  if (strncmp(arg, " ssid ", 6) == 0) {
    const char *s = arg + 6;
    const size_t n = strlen(s);
    Preferences p;
    bool ok = n >= 1 && n <= 32 && printable(s) && p.begin(FM_NVS_NAMESPACE, false);
    if (ok) {
      ok = p.putString(kKeySsid2, s) == n;
      p.end();
    }
    if (ok) {
      snprintf(g_ssid2, sizeof(g_ssid2), "%s", s);
      Serial.printf("[WIFI] network 2 set to '%s'\n", g_ssid2);
    } else {
      Serial.println("[WIFI] network 2 refused - 1 to 32 printable characters");
    }
  } else if (strncmp(arg, " pass ", 6) == 0 || strcmp(arg, " pass") == 0) {
    const char *s = arg[5] ? arg + 6 : "";
    const size_t n = strlen(s);
    Preferences p;
    bool ok = (n == 0 || (n >= 8 && n <= 63)) && printable(s) && p.begin(FM_NVS_NAMESPACE, false);
    if (ok) {
      // An open network is a present-but-empty key, as fm_ota stores it.
      ok = p.putString(kKeyPass2, s) == n && p.isKey(kKeyPass2);
      p.end();
    }
    if (ok) {
      g_hasPass2 = true;
      Serial.println("[WIFI] network 2 password saved");
    } else {
      Serial.println("[WIFI] network 2 password refused - 8 to 63 printable characters");
    }
  } else if (strcmp(arg, " forget") == 0) {
    Preferences p;
    if (p.begin(FM_NVS_NAMESPACE, false)) {
      p.remove(kKeySsid2);
      p.remove(kKeyPass2);
      p.end();
    }
    g_ssid2[0] = '\0';
    g_hasPass2 = false;
    Serial.println("[WIFI] network 2 forgotten");
  } else if (arg[0] == '\0') {
    Serial.printf("[WIFI] network 2 %s%s%s, password %s (field-logger uploads only)\n",
                  g_ssid2[0] ? "'" : "", g_ssid2[0] ? g_ssid2 : "not set", g_ssid2[0] ? "'" : "",
                  g_hasPass2 ? "saved" : "not set");
  } else {
    Serial.println("[WIFI] usage: wifi2 | wifi2 ssid <name> | wifi2 pass <pw> | wifi2 forget");
  }
  return true;
}

bool fmLogHasSsid2() { return g_ssid2[0] != '\0'; }
const char *fmLogSsid2() { return g_ssid2; }
bool fmLogHasPass2() { return g_hasPass2; }

void fmLogForgetSettings() {
  Preferences p;
  if (p.begin(FM_NVS_NAMESPACE, false)) {
    p.remove(kKeySsid2);
    p.remove(kKeyPass2);
    p.remove(kKeyUrl);
    p.end();
  }
  g_ssid2[0] = '\0';
  g_hasPass2 = false;
  loadSettings();
}

#endif  // FM_LOG_ENABLE
