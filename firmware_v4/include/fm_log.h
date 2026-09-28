/**
 * FloodMesh V4 - field logger: every frame sent and received, kept in flash
 * and uploaded to the field-logger web app. docs/field-logger-protocol.md is
 * the contract; this module is the unit side of it.
 *
 * Why it exists
 * -------------
 * In a field test nobody can write down every RSSI, SNR and hop count while
 * walking a street with a unit in one hand. So the unit records what it hears
 * and sends, and uploads it over the volunteer's phone hotspot every 5 minutes.
 * This is a TEST TOOL, not part of the product: the product has no WiFi.
 * Build with FM_LOG_ENABLE=0 and every hook below compiles to nothing.
 *
 * Records (protocol §3)
 * ---------------------
 * boot, rx, echo, tx, relay, sos and status records are made by the hooks in
 * main.cpp and fm_mesh.cpp. Each gets a per-unit `seq` that only ever grows,
 * across reboots too, the boot counter, the uptime, and the Unix time once the
 * clock has been synced on WiFi. The hooks are cheap: they fill a 128-byte
 * record in a RAM queue and return, so a received frame is never delayed by a
 * flash write. fmLogLoop() moves the queue to flash on the next loop pass in
 * which no relay or ACK is waiting to go out: a flash write stalls both CPU
 * cores for tens of ms, which would otherwise shift the relay contention
 * timing that the field tests are measuring. Every other flash write the
 * logger makes waits for such a pass too: the NVS counters (seq block,
 * dropped, acknowledged seq) and the deletion of files the uploader has sent.
 * The uploader task on the other core cannot see the mesh, so it only updates
 * the table in RAM and leaves those writes to fmLogLoop().
 *
 * The store: LittleFS, append-only segment files
 * ----------------------------------------------
 * LittleFS on the "spiffs" partition of default_8MB.csv (1.5 MB). Records are
 * fixed-size (128 bytes) with a CRC-32 each, appended to segment files of
 * FM_LOG_SEG_RECS records. Why this shape:
 *
 *  - Wear: a record is an append, never a rewrite of the store. LittleFS copies
 *    only the last partly-filled 4 KB block of the file on each append and
 *    spreads its erases over the whole partition. At a busy 10,000 records a
 *    day that is ~30 erases per block per day against 100,000 cycles.
 *  - Power loss: LittleFS commits an append atomically at close, so a cut
 *    mid-write leaves the file as it was. The per-record CRC and the fixed size
 *    are the second line: a damaged record, or a chunk of them that cannot be
 *    read at all, is skipped and the ones around it are still read, and a file
 *    whose length is not a whole number of records is sealed (never appended
 *    to again) so nothing after a torn tail is ever misaligned. A file with no
 *    valid record left is deleted at boot, its unsent records counted as
 *    dropped.
 *  - Dropping and deleting are file deletes: when the store holds
 *    FM_LOG_SEGMENTS files the oldest file goes (its unsent records are added
 *    to the lifetime `dropped` counter, protocol §2), and a file whose records
 *    have all been acknowledged by the server is deleted. A partly-sent file
 *    keeps a read cursor instead; the acknowledged seq is saved in NVS once
 *    per upload window, and re-sending after a crash is harmless because the
 *    server stores each (unit, seq) once.
 *
 * Capacity: FM_LOG_SEGMENTS x FM_LOG_SEG_RECS = 24 x 256 = 6,144 records
 * (768 KB), and never fewer than 5,888 right after a drop (protocol: >= 5,000).
 *
 * seq and boot
 * ------------
 * seq uses the same high-water-mark reservation as fm_ids.cpp's replay
 * counter: NVS holds "everything below this may be used", written once per
 * FM_LOG_SEQ_BLOCK records by fmLogLoop() while the mesh is idle, half a block
 * before the reserve runs out, so a record costs no NVS write and an unclean
 * reboot skips fewer than 1.5 x FM_LOG_SEQ_BLOCK values (gaps are allowed,
 * §3). At boot seq also starts above the newest record found in flash, so even
 * a wiped NVS cannot make it go backwards. The boot counter is +1 per boot.
 *
 * The uploader (protocol §1, §4, §5)
 * ----------------------------------
 * Its own FreeRTOS task on core 0 (the Arduino loop runs on core 1), so the
 * radio, keypad and screen keep working while it waits on WiFi and TLS. The
 * loop decides WHEN (millis() keeps counting through light sleep; FreeRTOS
 * ticks do not, so the task never times anything itself) and wakes the task.
 * One upload window:
 *   1. join network 1 (NVS wifissid/wifipass, shared with fm_ota), else
 *      network 2 (wifissid2/wifipass2), ~15 s each;
 *   2. SNTP (pool.ntp.org, time.google.com);
 *   3. POST <base>/api/ingest, <= 100 records and <= 16 KB each, HMAC-SHA256
 *      of the exact body bytes in X-FM-Signature, until nothing is left or a
 *      request fails. The HMAC key is the 16-32 BYTES that FM_LOG_KEY spells
 *      in hex, not the hex text (the server decodes LOG_KEY the same way).
 *      Records with seq <= acked_seq are deleted after a 200. The connection
 *      is opened (and, if the server closed it, reopened) before each body is
 *      built, so no TLS handshake sits between up_ms and the server's receive
 *      time (§5), and kept open across the requests of a window;
 *   4. server_epoch re-anchors the records' clock if SNTP failed; the V4
 *      clock is offered the times heard (see Time below); WiFi off.
 * The first window opens ~60 s after boot, then every FM_LOG_PERIOD_MS.
 *
 * HTTPS checks the server against the Mozilla root bundle that the Arduino
 * core's prebuilt mbedTLS already carries (x509_crt_bundle, 136 roots; the
 * *.vercel.app chain ends at GTS Root R1 / GlobalSign Root CA, both in it).
 * FM_LOG_TLS_INSECURE=1 skips the check, for bench servers only. The HMAC
 * protects the records either way.
 *
 * Time: the logger stamps records from its own anchor (the SNTP or server
 * time and the millis() it was taken at), re-set at every window. It does NOT
 * trust a clock it has not synced in this boot: after a power-off fm_role
 * restores the last hourly save, which can be hours behind, so until the first
 * sync records carry epoch 0 and the server derives their time from the uptime
 * (§5). It does not stamp from the V4 clock either: fmTimeSet() only ever
 * moves forward (responder expiry), so a clock that runs fast would never be
 * corrected.
 * The V4 clock is stricter, because a responder's key is deleted when it
 * passes the grant's expiry and it can never go back: one bad reading years
 * ahead would expire every grant, the next ones too. Neither SNTP (plain UDP)
 * nor server_epoch is authenticated alone, so it moves only when SNTP and a
 * server_epoch over VERIFIED HTTPS agree (FM_LOG_CLOCK_AGREE_MS), or, from one
 * source alone, by at most FM_LOG_CLOCK_SLEW_S + FM_LOG_CLOCK_SLEW_PCT of the
 * uptime per boot (drift, not jumps), and never from an unset clock.
 * server_epoch over http:// or FM_LOG_TLS_INSECURE never reaches it.
 *
 * Coordination: no window while WiFi update mode is open (WiFi is its), and
 * fmOtaStart() refuses while a window runs. BLE provisioning is a separate boot
 * path that ends in a restart before fmLogBegin() runs, so the two never meet.
 * While a window runs the unit does not light-sleep (fmLogBusy() in canSleep()),
 * and the sleep length is capped by fmLogNextInMs() so the unit is awake when
 * the next window is due.
 *
 * Threads: every fmLog* hook and fmLogLoop() must be called from the loop task
 * (they are; a call from anywhere else is refused and reported once). The store
 * is shared with the uploader task under a mutex; the few values the status
 * screen reads are atomics.
 */
#pragma once
#include <Arduino.h>

#ifndef FM_LOG_ENABLE
#define FM_LOG_ENABLE 1              // 0 = no logger at all: hooks compile to nothing
#endif
#ifndef FM_LOG_KEY
#define FM_LOG_KEY ""                // fleet HMAC key, 32-64 hex chars; "" = record only
#endif
#ifndef FM_LOG_URL
#define FM_LOG_URL ""                // server base, e.g. "https://x.vercel.app"; NVS logurl wins
#endif
#ifndef FM_LOG_PERIOD_MS
#define FM_LOG_PERIOD_MS 300000UL    // one upload window every 5 min
#endif
#ifndef FM_LOG_FIRST_MS
#define FM_LOG_FIRST_MS 60000UL      // first window this long after boot
#endif
#ifndef FM_LOG_STATUS_MS
#define FM_LOG_STATUS_MS 300000UL    // a status record at boot, then this often
#endif
#ifndef FM_LOG_JOIN_MS
#define FM_LOG_JOIN_MS 15000UL       // give up on one WiFi network after this
#endif
#ifndef FM_LOG_NTP_MS
#define FM_LOG_NTP_MS 2000UL         // wait this long for each SNTP server
#endif
#ifndef FM_LOG_HTTP_MS
#define FM_LOG_HTTP_MS 15000UL       // TCP/TLS connect and response timeout
#endif
#ifndef FM_LOG_MAX_RECORDS
#define FM_LOG_MAX_RECORDS 100       // protocol §1: per request
#endif
#ifndef FM_LOG_MAX_BODY
#define FM_LOG_MAX_BODY 16384        // protocol §1: bytes per request body
#endif
#ifndef FM_LOG_MAX_REQUESTS
#define FM_LOG_MAX_REQUESTS 80       // per window: enough to empty a full store
#endif
#ifndef FM_LOG_SEG_RECS
#define FM_LOG_SEG_RECS 256          // records per segment file (32 KB)
#endif
#ifndef FM_LOG_SEGMENTS
#define FM_LOG_SEGMENTS 24           // segment files kept; the oldest goes when full
#endif
#ifndef FM_LOG_RAMQ
#define FM_LOG_RAMQ 32               // records waiting in RAM for the next flush
#endif
#ifndef FM_LOG_SEQ_BLOCK
#define FM_LOG_SEQ_BLOCK 256         // seq values reserved per NVS write
#endif
#ifndef FM_LOG_TASK_STACK
#define FM_LOG_TASK_STACK 16384      // mbedTLS handshake + HTTPClient; see `log` for the margin
#endif
#ifndef FM_LOG_MIN_HEAP
#define FM_LOG_MIN_HEAP 90000UL      // internal heap needed to start WiFi + TLS
#endif
#ifndef FM_LOG_TLS_INSECURE
#define FM_LOG_TLS_INSECURE 0        // 1 = do not check the server certificate (bench only)
#endif
#ifndef FM_LOG_CLOCK_AGREE_MS
#define FM_LOG_CLOCK_AGREE_MS 10000UL   // SNTP and verified-HTTPS server_epoch "agree" within this
#endif
#ifndef FM_LOG_CLOCK_SLEW_S
#define FM_LOG_CLOCK_SLEW_S 60UL        // one source alone moves the V4 clock this far per boot
#endif
#ifndef FM_LOG_CLOCK_SLEW_PCT
#define FM_LOG_CLOCK_SLEW_PCT 5         // ... plus this % of the uptime (RC-clock drift in sleep)
#endif

static_assert((FM_LOG_SEGMENTS - 1) * FM_LOG_SEG_RECS >= 5000,
              "protocol section 7: the store keeps >= 5,000 records even right after a drop");

// What a record is about (protocol §3, field `t`).
enum FmLogType : uint8_t {
  FM_LOG_T_NONE = 0,
  FM_LOG_T_SOS,
  FM_LOG_T_ALARM,       // a legacy V3 alarm that is not an SOS (SAFE)
  FM_LOG_T_TEXT,
  FM_LOG_T_PING,        // a text starting "PING " (range-test ping)
  FM_LOG_T_ACK,         // "delivered" ACK (rx: either kind; text says which)
  FM_LOG_T_HEARTBEAT,
  FM_LOG_T_HELP,        // tx only: this responder sent HELP IS COMING
};

// This unit's own SOS (protocol §3, kind `sos`, field `state`).
enum FmLogSosState : uint8_t {
  FM_LOG_SOS_SENT = 1,
  FM_LOG_SOS_RETRY,
  FM_LOG_SOS_DELIVERED,
  FM_LOG_SOS_HELP,
  FM_LOG_SOS_NOT_DELIVERED,
  FM_LOG_SOS_STOPPED,
};

/** What main.cpp knows for a status record (protocol §3, kind `status`). */
struct FmLogStatus {
  float    battV;
  uint8_t  battPct;
  bool     powered;
  uint16_t airPermille;
  uint8_t  heard;       // stations in the heard list
  uint8_t  sleepPct;
  bool     responder;
};
typedef void (*FmLogStatusFn)(FmLogStatus *out);

#if FM_LOG_ENABLE

/**
 * Once from setup(), after fmIdsBegin() and bootCombos(): mounts the store,
 * bumps the boot counter, makes the boot record and starts the uploader task.
 * `status` fills a status record (called from fmLogLoop, in the loop task).
 */
void fmLogBegin(FmLogStatusFn status);

/** Every loop pass: flush records to flash, status records, start windows. */
void fmLogLoop(uint32_t now);

// ---------------------------------------------------------------- hooks
/** A frame received and accepted. ch < 0: no SOS channel. text may be null. */
void fmLogRx(FmLogType t, const char *from, uint8_t id, uint8_t hops, float rssi, float snr,
             const char *text, int ch);
/** A neighbour relayed one of our own frames. frameType is FM_TYPE_*. */
void fmLogEcho(uint8_t frameType, uint8_t id, float rssi, float snr);
/** This unit transmitted its own frame. id < 0: none was assigned (refused). */
void fmLogTx(FmLogType t, int id, bool ok, const char *text, int ch);
/** This unit relayed someone else's frame (the frame as sent). */
void fmLogRelay(const uint8_t *frame, size_t len, const char *from, uint8_t id);
/** This unit's own SOS changed state. id < 0: no frame to name. n = retries so far. */
void fmLogSos(FmLogSosState state, int id, uint8_t ch, uint8_t n);

/** SOS / ALARM / TEXT / PING / ACK / HEARTBEAT of an on-air frame. */
FmLogType fmLogFrameType(const uint8_t *frame, size_t len);

// ---------------------------------------------------------------- state
/** An upload window is running: WiFi is ours and the unit must stay awake. */
bool     fmLogBusy();
/** ms until the logger next needs the loop (upload or status); UINT32_MAX if never. */
uint32_t fmLogNextInMs(uint32_t now);
/** One status-screen line: records waiting and the age of the last upload. */
void     fmLogStatusLine(char *out, size_t n, uint32_t now);

// ---------------------------------------------------------------- serial
/**
 * `log`, `log send`, `log url <base>`, `log url default`, `log clear`.
 * Returns false if the line is not a log command.
 */
bool fmLogCommand(const char *line);
/**
 * `wifi2 ssid <name>`, `wifi2 pass <pw>`, `wifi2 forget`. The password is
 * never printed. Returns false if the line is not a wifi2 command.
 */
bool fmLogWifi2Command(const char *line);
/** Network 2 for the `wifi` summary line: "'name'" / "not set", and password state. */
bool        fmLogHasSsid2();
const char *fmLogSsid2();
bool        fmLogHasPass2();
/** Factory reset: forget network 2 and the server address (records are kept). */
void        fmLogForgetSettings();

#else  // FM_LOG_ENABLE == 0: the product build carries none of it

inline void fmLogBegin(FmLogStatusFn) {}
inline void fmLogLoop(uint32_t) {}
inline void fmLogRx(FmLogType, const char *, uint8_t, uint8_t, float, float, const char *, int) {}
inline void fmLogEcho(uint8_t, uint8_t, float, float) {}
inline void fmLogTx(FmLogType, int, bool, const char *, int) {}
inline void fmLogRelay(const uint8_t *, size_t, const char *, uint8_t) {}
inline void fmLogSos(FmLogSosState, int, uint8_t, uint8_t) {}
inline FmLogType fmLogFrameType(const uint8_t *, size_t) { return FM_LOG_T_NONE; }
inline bool fmLogBusy() { return false; }
inline uint32_t fmLogNextInMs(uint32_t) { return UINT32_MAX; }
inline void fmLogStatusLine(char *out, size_t n, uint32_t) { if (n) out[0] = '\0'; }
inline bool fmLogCommand(const char *) { return false; }
inline bool fmLogWifi2Command(const char *) { return false; }
inline bool fmLogHasSsid2() { return false; }
inline const char *fmLogSsid2() { return ""; }
inline bool fmLogHasPass2() { return false; }
inline void fmLogForgetSettings() {}

#endif
