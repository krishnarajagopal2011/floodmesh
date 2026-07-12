/* ============================================================================
 *  FLOODPAGER v1  —  Off-grid LoRa emergency pager for neighbourhood use
 *  ---------------------------------------------------------------------------
 *  Target : ESP32-WROOM-32E + Semtech SX1276 + SSD1306 128x64 OLED
 *  Band   : 865-867 MHz (India, licence-exempt low-power. Verify current WPC
 *           notification before deployment. 17 dBm + ~2 dBi is well inside the
 *           1 W ERP ceiling.)
 *  Libs   : sandeepmistry/arduino-LoRa, Adafruit_SSD1306, Adafruit_GFX
 *
 *  ARCHITECTURE (see design doc)
 *  ---------------------------------------------------------------------------
 *  - Radio sits in RX_CONTINUOUS. CPU sits in LIGHT SLEEP (~0.8 mA) and is woken
 *    by DIO0 going HIGH (RxDone) or by any button going LOW. Deep sleep is not
 *    used: the radio's 11 mA dominates, so deep sleep would buy ~7% for a large
 *    amount of complexity and a FIFO-clearing LoRa.begin() on every wake.
 *  - Flooding with counter-based suppression. A relay is scheduled with a
 *    backoff biased by RSSI (weak signal => far away => relay sooner, pushing
 *    the packet outward). If 2 other nodes relay it first, we cancel.
 *  - "SENT SUCCESSFULLY" is a lie on a broadcast protocol. Instead we listen for
 *    a neighbour rebroadcasting our own (originId, seq) and report a real
 *    RELAY-ACK with the relaying node and its RSSI.
 *  - Node identity lives in NVS, not in the firmware. One binary, N nodes.
 *
 *  BEFORE YOU FLASH
 *  ---------------------------------------------------------------------------
 *  1. Change PSK_KEY. Do not ship the placeholder.
 *  2. Provision each unit: hold BTN_SAFE + BTN_RESCUE during boot, then follow
 *     the serial prompt at 115200 baud.
 *  3. Never power the board without an antenna connected to the SX1276.
 *
 *  CONTROLS
 *  ---------------------------------------------------------------------------
 *    BTN_SAFE     tap        broadcast "SAFE / ALL OK"   (1 hop, 5 min limit)
 *    BTN_SAFE     hold 0.8s  colony roster (last status of every node heard)
 *    BTN_MEDICAL  tap        broadcast "NEED MEDICAL"
 *    BTN_WATER    tap        broadcast "WATER ON GROUND FLOOR"
 *    BTN_WATER    hold 0.8s  PING — link survey, neighbours auto-reply
 *    BTN_RESCUE   hold 1.5s  broadcast "NEED EVACUATION" (hold-to-confirm)
 * ==========================================================================*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>

/* ===========================================================================
 * SECTION 1 — CONFIGURATION
 * ========================================================================= */

/* --- Pin map -------------------------------------------------------------
 * NOTE: BUTTON_1 was moved off GPIO12 and DIO0 off GPIO2. GPIO12 (MTDI) is a
 * strapping pin: held HIGH at reset it sets VDD_SDIO to 1.8 V and the 3.3 V
 * flash on a WROOM-32E will not boot. INPUT_PULLUP idles HIGH. GPIO2 is the
 * boot-mode strap and is the onboard LED on many dev boards.
 *
 * All four buttons and DIO0 are RTC-capable GPIOs. This is required: on the
 * classic ESP32, gpio_wakeup_enable() in light sleep only works on RTC IOs
 * (0, 2, 4, 12-15, 25-27, 32-39).
 * ----------------------------------------------------------------------- */
#define PIN_LORA_NSS      5
#define PIN_LORA_RST     14
#define PIN_LORA_DIO0    26      // was 2 (strapping pin)
#define PIN_LORA_SCK     18
#define PIN_LORA_MISO    19
#define PIN_LORA_MOSI    23

#define PIN_OLED_SDA     21
#define PIN_OLED_SCL     22

#define PIN_BTN_SAFE     32      // was 12 (strapping pin — would brick boot)
#define PIN_BTN_MEDICAL  33      // was 13
#define PIN_BTN_WATER    25
#define PIN_BTN_RESCUE   27      // was 26

#define PIN_BUZZER        4      // piezo. A silent pager is a useless pager.
#define PIN_VBAT_SENSE   35      // ADC1, input-only. 2:1 divider from battery.

/* --- Radio ---------------------------------------------------------------
 * SF9/BW125/CR4:5 gives ~185 ms airtime for our 12-byte packet and 300-800 m
 * in dense urban. Do a range walk (PING mode) before changing these; if you
 * must go to SF10 be aware airtime doubles and so does channel congestion.
 * ----------------------------------------------------------------------- */
#define LORA_FREQ_HZ       865000000UL   // inside 865-867 MHz
#define LORA_SF            9
#define LORA_BW            125000
#define LORA_CR_DENOM      5             // 4/5
#define LORA_PREAMBLE      8
#define LORA_SYNCWORD      0x12          // private network (NOT 0x34/LoRaWAN)
#define LORA_TX_DBM        17            // PA_BOOST

/* --- Protocol ----------------------------------------------------------- */
#define PROTO_MAGIC        0xAD
#define PROTO_VERSION      1
#define ID_UNSET           0
#define ID_MAX             250

#define TX_REPEATS         3             // copies of each originated packet
#define TX_REPEAT_GAP_MS   350           // > airtime, so copies do not overlap

#define RELAY_BACKOFF_MIN_MS  200        // far node (weak RSSI) relays first
#define RELAY_BACKOFF_MAX_MS 1400        // near node (strong RSSI) waits
#define RELAY_JITTER_MS       250
#define RELAY_SUPPRESS_COUNT    2        // heard N other relays => cancel ours

#define LBT_RSSI_BUSY_DBM   -95          // listen-before-talk threshold
#define LBT_MAX_DEFERS        6
#define LBT_DEFER_MS        120

#define SEEN_CACHE_SIZE      48
#define SEEN_TTL_MS      600000UL        // 10 minutes
#define RELAY_QUEUE_SIZE      8
#define ROSTER_SIZE          32

#define ACK_WINDOW_MS     12000UL        // wait this long for a relay-ACK

/* Rate limits. "SAFE" is limited hard: 20 people pressing the reassuring green
 * button must not jam the channel a rescue call needs.                     */
#define RATELIMIT_SAFE_MS   300000UL     // 5 min
#define RATELIMIT_OTHER_MS   30000UL     // 30 s

/* --- Power / UI --------------------------------------------------------- */
#define DISPLAY_TIMEOUT_MS   30000UL     // OLED off after inactivity
#define MAX_SLEEP_MS          8000UL     // bounded so the WDT gets fed
#define WDT_TIMEOUT_S           30
#define DEBOUNCE_MS             40
#define LONGPRESS_MS           800
#define RESCUE_CONFIRM_MS     1500       // hold-to-confirm, with progress bar

#define VBAT_DIVIDER_RATIO    2.0f
#define VBAT_LOW_MV           3400

/* --- Authentication -----------------------------------------------------
 * CHANGE THIS KEY. Then burn the same key into every unit in the colony.
 * A spoofed "NEED EVACUATION" from a house that is fine will send people into
 * moving water. Four truncated CMAC bytes cost nothing and stop casual abuse.
 * If AES-CMAC fails to compile on your core, set USE_CMAC 0 — but understand
 * that the fallback is a keyed checksum, NOT authentication.
 * ----------------------------------------------------------------------- */
#define USE_CMAC 1
static const uint8_t PSK_KEY[16] = {
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
  0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
};

#if USE_CMAC
  #include "mbedtls/cmac.h"
  #include "mbedtls/cipher.h"
#endif

/* ===========================================================================
 * SECTION 2 — PACKET
 *
 * 12 bytes on air (~185 ms at SF9). JSON for the same four facts would be
 * 60-90 bytes and roughly 450 ms — airtime is the scarcest resource here.
 *
 * The MAC deliberately covers ONLY the immutable fields. `hop` and `relayId`
 * are rewritten by every relay; if they were authenticated, the first hop
 * would invalidate the packet for everyone downstream.
 * ========================================================================= */

enum : uint8_t {
  ST_NONE    = 0,
  ST_SAFE    = 1,
  ST_MEDICAL = 2,
  ST_WATER   = 3,
  ST_RESCUE  = 4,
  ST_PING    = 5,
  ST_PONG    = 6
};

typedef struct __attribute__((packed)) {
  uint8_t  magic;             // network filter — drops foreign LoRa traffic
  uint8_t  ver    : 4;
  uint8_t  hop    : 4;        // MUTABLE
  uint8_t  originId;
  uint16_t seq;               // per-origin, monotonic
  uint8_t  status : 3;
  uint8_t  prio   : 2;
  uint8_t  rsv    : 3;
  uint8_t  relayId;           // MUTABLE — last node to transmit this copy
  uint8_t  vbat;              // (volts - 2.5) * 100  =>  2.50 .. 5.05 V
  uint32_t mac;               // truncated AES-CMAC over immutable fields
} pager_pkt_t;

static_assert(sizeof(pager_pkt_t) == 12, "packet layout changed unexpectedly");

/* Hop budget by priority. SAFE gets one hop; it is reassurance, not a call for
 * help, and it must not consume the channel three hops deep.               */
static uint8_t maxHopFor(uint8_t status) {
  switch (status) {
    case ST_SAFE:    return 1;
    case ST_PING:    return 1;
    case ST_PONG:    return 0;   // never relayed
    case ST_MEDICAL:
    case ST_WATER:
    case ST_RESCUE:  return 3;
    default:         return 0;
  }
}

static const char* statusText(uint8_t s) {
  switch (s) {
    case ST_SAFE:    return "SAFE / ALL OK";
    case ST_MEDICAL: return "NEED MEDICAL";
    case ST_WATER:   return "WATER ON GND FLR";
    case ST_RESCUE:  return "NEED EVACUATION";
    case ST_PING:    return "LINK TEST";
    case ST_PONG:    return "TEST REPLY";
    default:         return "UNKNOWN";
  }
}

/* ===========================================================================
 * SECTION 3 — GLOBAL STATE
 * ========================================================================= */

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
Preferences prefs;

static uint8_t  myNodeId = ID_UNSET;
static uint16_t mySeq    = 0;
static uint16_t seqCommitted = 0;

/* Dedup cache -------------------------------------------------------------*/
struct SeenEntry { uint8_t origin; uint16_t seq; uint32_t t; };
static SeenEntry seenCache[SEEN_CACHE_SIZE];
static uint8_t   seenHead = 0;

/* Relay queue -------------------------------------------------------------*/
struct PendingRelay {
  bool        active;
  pager_pkt_t pkt;
  uint32_t    dueAt;
  uint8_t     dupHeard;
};
static PendingRelay relayQ[RELAY_QUEUE_SIZE];

/* Outbound burst ----------------------------------------------------------*/
struct TxBurst {
  bool        active;
  pager_pkt_t pkt;
  uint8_t     copiesLeft;
  uint32_t    nextAt;
  uint8_t     defers;
};
static TxBurst txBurst;

/* Relay-ACK ---------------------------------------------------------------*/
static bool     ackPending   = false;
static uint16_t ackSeq       = 0;
static uint32_t ackDeadline  = 0;
static uint8_t  ackRelayId   = 0;
static int      ackRelayRssi = 0;

/* Roster ------------------------------------------------------------------*/
struct RosterEntry {
  uint8_t  id;
  uint8_t  status;
  uint8_t  hops;
  int8_t   rssi;
  uint8_t  vbat;
  uint32_t t;
};
static RosterEntry roster[ROSTER_SIZE];

/* Rate limiting -----------------------------------------------------------*/
static uint32_t lastTxAt[8] = {0};

/* UI ----------------------------------------------------------------------*/
enum UiScreen { UI_IDLE, UI_ALERT, UI_SENDING, UI_SENT, UI_ROSTER, UI_CONFIRM };
static UiScreen uiScreen      = UI_IDLE;
static uint32_t uiLastActive  = 0;
static bool     oledOn        = true;
static bool     uiDirty       = true;
static uint32_t alertCount    = 0;
static pager_pkt_t lastAlert;
static int      lastAlertRssi = 0;

/* Buzzer (non-blocking pattern player) ------------------------------------*/
static const uint16_t *buzPattern = nullptr;   // {toneHz, ms, toneHz, ms, ... 0}
static uint8_t  buzIdx  = 0;
static uint32_t buzNext = 0;

static const uint16_t PAT_MEDICAL[] = {2200,150, 0,100, 2200,150, 0,600,
                                       2200,150, 0,100, 2200,150, 0,0};
static const uint16_t PAT_RESCUE[]  = {2700,400, 0,120, 2700,400, 0,120,
                                       2700,400, 0,120, 2700,400, 0,0};
static const uint16_t PAT_WATER[]   = {1800,250, 0,150, 1800,250, 0,0};
static const uint16_t PAT_SAFE[]    = {1400,90, 0,0};
static const uint16_t PAT_TICK[]    = {2000,25, 0,0};

/* ===========================================================================
 * SECTION 4 — SMALL UTILITIES
 * ========================================================================= */

/* millis() is monotonic across light sleep on ESP32 (esp_timer is corrected
 * from the RTC on wake), so plain elapsed() comparisons remain valid.      */
static inline bool elapsed(uint32_t since, uint32_t span) {
  return (uint32_t)(millis() - since) >= span;
}

static uint16_t readVbatMillivolts() {
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(PIN_VBAT_SENSE);
  return (uint16_t)((acc / 8) * VBAT_DIVIDER_RATIO);
}
static uint8_t encodeVbat(uint16_t mv) {
  int v = ((int)mv - 2500) / 10;
  return (uint8_t)constrain(v, 0, 255);
}
static uint16_t decodeVbat(uint8_t v) { return 2500 + (uint16_t)v * 10; }

/* ---- Message authentication --------------------------------------------
 * Canonical buffer over immutable fields only. Never MAC `hop` or `relayId`.
 * ----------------------------------------------------------------------- */
static void macCanonical(const pager_pkt_t &p, uint8_t out[7]) {
  out[0] = p.magic;
  out[1] = p.ver;
  out[2] = p.originId;
  out[3] = (uint8_t)(p.seq & 0xFF);
  out[4] = (uint8_t)(p.seq >> 8);
  out[5] = (uint8_t)(p.status | (p.prio << 3));
  out[6] = p.vbat;
}

static uint32_t computeMac(const pager_pkt_t &p) {
  uint8_t buf[7];
  macCanonical(p, buf);

#if USE_CMAC
  uint8_t tag[16] = {0};
  const mbedtls_cipher_info_t *ci =
      mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
  if (ci && mbedtls_cipher_cmac(ci, PSK_KEY, 128, buf, sizeof(buf), tag) == 0) {
    return ((uint32_t)tag[0]) | ((uint32_t)tag[1] << 8) |
           ((uint32_t)tag[2] << 16) | ((uint32_t)tag[3] << 24);
  }
  // Fall through to the weak path rather than transmitting an unMACed packet.
#endif
  // Keyed checksum fallback. NOT authentication — forgeable by anyone with
  // the source. Present only so the field is never zero.
  uint32_t h = 0x811C9DC5UL;
  for (uint8_t i = 0; i < sizeof(buf); i++) { h ^= buf[i]; h *= 16777619UL; }
  for (uint8_t i = 0; i < sizeof(PSK_KEY); i++) { h ^= PSK_KEY[i]; h *= 16777619UL; }
  return h;
}

static bool macValid(const pager_pkt_t &p) { return computeMac(p) == p.mac; }

/* ---- Dedup cache -------------------------------------------------------- */
static int seenFind(uint8_t origin, uint16_t seq) {
  uint32_t now = millis();
  for (int i = 0; i < SEEN_CACHE_SIZE; i++) {
    if (seenCache[i].origin == origin && seenCache[i].seq == seq) {
      if ((uint32_t)(now - seenCache[i].t) < SEEN_TTL_MS) return i;
      seenCache[i].origin = ID_UNSET;   // expired
      return -1;
    }
  }
  return -1;
}
static void seenAdd(uint8_t origin, uint16_t seq) {
  seenCache[seenHead] = { origin, seq, millis() };
  seenHead = (seenHead + 1) % SEEN_CACHE_SIZE;
}

/* ---- Roster ------------------------------------------------------------- */
static void rosterUpdate(const pager_pkt_t &p, int rssi) {
  if (p.status == ST_PING || p.status == ST_PONG) return;
  int slot = -1, oldest = 0;
  for (int i = 0; i < ROSTER_SIZE; i++) {
    if (roster[i].id == p.originId) { slot = i; break; }
    if (roster[i].id == ID_UNSET)   { slot = i; break; }
    if (roster[i].t < roster[oldest].t) oldest = i;
  }
  if (slot < 0) slot = oldest;
  roster[slot] = { p.originId, (uint8_t)p.status, (uint8_t)p.hop,
                   (int8_t)constrain(rssi, -128, 0), p.vbat, millis() };
}

/* ---- Buzzer ------------------------------------------------------------- */
static void buzzStart(const uint16_t *pattern) {
  buzPattern = pattern; buzIdx = 0; buzNext = millis();
}
static void buzzService() {
  if (!buzPattern) return;
  if (!elapsed(buzNext, 0)) return;
  uint16_t hz = buzPattern[buzIdx];
  uint16_t ms = buzPattern[buzIdx + 1];
  if (hz == 0 && ms == 0) { noTone(PIN_BUZZER); buzPattern = nullptr; return; }
  if (hz) tone(PIN_BUZZER, hz); else noTone(PIN_BUZZER);
  buzNext = millis() + ms;
  buzIdx += 2;
}
static bool buzzBusy() { return buzPattern != nullptr; }

/* ===========================================================================
 * SECTION 5 — DISPLAY
 * ========================================================================= */

static void oledWake() {
  if (!oledOn) { oled.ssd1306_command(SSD1306_DISPLAYON); oledOn = true; }
  uiLastActive = millis();
  uiDirty = true;
}
/* SSD1306 is OLED — self-emissive, no backlight. DISPLAYOFF takes the panel
 * from ~10-15 mA (as much as the radio) to near zero.                      */
static void oledSleep() {
  if (oledOn) { oled.ssd1306_command(SSD1306_DISPLAYOFF); oledOn = false; }
}

static void drawHeader() {
  uint16_t mv = readVbatMillivolts();
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("H%02u", myNodeId);
  oled.setCursor(60, 0);
  oled.printf("%u.%02uV", mv / 1000, (mv % 1000) / 10);
  if (mv < VBAT_LOW_MV) { oled.setCursor(104, 0); oled.print("LOW"); }
  oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
}

static void drawIdle() {
  oled.clearDisplay(); drawHeader();
  oled.setCursor(0, 18); oled.print("SYSTEM READY");
  oled.setCursor(0, 30); oled.printf("Listening  SF%d", LORA_SF);
  oled.setCursor(0, 42); oled.printf("Alerts seen: %lu", (unsigned long)alertCount);
  oled.setCursor(0, 54); oled.print("Hold B1 = roster");
  oled.display();
}

static void drawAlert() {
  oled.clearDisplay(); drawHeader();
  oled.setTextSize(1);
  oled.setCursor(0, 16);
  oled.printf("ALERT  HOUSE %02u", lastAlert.originId);
  oled.setCursor(0, 30);
  oled.print(statusText(lastAlert.status));
  oled.setCursor(0, 46);
  oled.printf("%d dBm  hop %u", lastAlertRssi, lastAlert.hop);
  oled.setCursor(0, 56);
  oled.printf("via H%02u  bat %u.%02uV", lastAlert.relayId,
              decodeVbat(lastAlert.vbat) / 1000,
              (decodeVbat(lastAlert.vbat) % 1000) / 10);
  oled.display();
}

static void drawSending() {
  oled.clearDisplay(); drawHeader();
  oled.setTextSize(2); oled.setCursor(0, 22); oled.print("SENDING");
  oled.setTextSize(1); oled.setCursor(0, 46);
  oled.print(statusText(txBurst.pkt.status));
  oled.display();
}

/* We never claim delivery we cannot prove. Either a neighbour was heard
 * rebroadcasting our packet, or we say so plainly.                         */
static void drawSent() {
  oled.clearDisplay(); drawHeader();
  oled.setTextSize(1);
  oled.setCursor(0, 16); oled.printf("SENT (%dx)", TX_REPEATS);
  oled.setCursor(0, 30);
  if (ackRelayId) {
    oled.printf("RELAYED BY H%02u", ackRelayId);
    oled.setCursor(0, 42); oled.printf("%d dBm", ackRelayRssi);
    oled.setCursor(0, 54); oled.print("Message is moving.");
  } else if (ackPending) {
    oled.print("listening for relay");
    oled.setCursor(0, 46); oled.print("...");
  } else {
    oled.print("NO RELAY HEARD");
    oled.setCursor(0, 42); oled.print("Retry, or move to");
    oled.setCursor(0, 54); oled.print("higher ground/floor");
  }
  oled.display();
}

static void drawRoster() {
  oled.clearDisplay(); drawHeader();
  oled.setTextSize(1);
  int line = 0;
  uint32_t now = millis();
  for (int i = 0; i < ROSTER_SIZE && line < 4; i++) {
    if (roster[i].id == ID_UNSET) continue;
    uint32_t agoMin = (now - roster[i].t) / 60000UL;
    oled.setCursor(0, 14 + line * 12);
    const char *s = statusText(roster[i].status);
    char abbrev[10];
    strncpy(abbrev, s, 9); abbrev[9] = 0;
    oled.printf("H%02u %-9s %2lum", roster[i].id, abbrev, (unsigned long)agoMin);
    line++;
  }
  if (line == 0) { oled.setCursor(0, 30); oled.print("No nodes heard yet"); }
  oled.display();
}

static void drawConfirm(uint32_t heldMs) {
  oled.clearDisplay(); drawHeader();
  oled.setTextSize(1);
  oled.setCursor(0, 18); oled.print("HOLD TO CONFIRM");
  oled.setCursor(0, 30); oled.print("NEED EVACUATION");
  int w = (int)((heldMs * 124UL) / RESCUE_CONFIRM_MS);
  oled.drawRect(0, 46, 128, 12, SSD1306_WHITE);
  oled.fillRect(2, 48, constrain(w, 0, 124), 8, SSD1306_WHITE);
  oled.display();
}

static void uiService(uint32_t confirmHeldMs) {
  if (!oledOn) return;

  if (uiDirty || uiScreen == UI_CONFIRM || uiScreen == UI_SENT) {
    switch (uiScreen) {
      case UI_IDLE:    drawIdle();    break;
      case UI_ALERT:   drawAlert();   break;
      case UI_SENDING: drawSending(); break;
      case UI_SENT:    drawSent();    break;
      case UI_ROSTER:  drawRoster();  break;
      case UI_CONFIRM: drawConfirm(confirmHeldMs); break;
    }
    uiDirty = false;
  }

  if (elapsed(uiLastActive, DISPLAY_TIMEOUT_MS)) {
    uiScreen = UI_IDLE;
    oledSleep();
  }
}

/* ===========================================================================
 * SECTION 6 — RADIO
 * ========================================================================= */

static void radioListen() { LoRa.receive(); }

/* Listen-before-talk. Cheap: REG_RSSI_VALUE is live while in RX_CONTINUOUS. */
static bool channelClear() { return LoRa.rssi() < LBT_RSSI_BUSY_DBM; }

static void radioSend(const pager_pkt_t &p) {
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.write((const uint8_t *)&p, sizeof(p));
  LoRa.endPacket();            // blocking, ~185 ms. Acceptable; the gaps are not.
  radioListen();
}

static void radioInit() {
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  LoRa.setPins(PIN_LORA_NSS, PIN_LORA_RST, PIN_LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ_HZ)) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 16); oled.print("LoRa FAIL");
    oled.setCursor(0, 30); oled.print("Check SPI wiring");
    oled.setCursor(0, 42); oled.print("and antenna.");
    oled.display();
    // Halt loudly. A pager that silently fails its radio is worse than none.
    while (true) { buzzStart(PAT_RESCUE); while (buzzBusy()) buzzService(); delay(2000); }
  }

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR_DENOM);
  LoRa.setPreambleLength(LORA_PREAMBLE);
  LoRa.setSyncWord(LORA_SYNCWORD);
  LoRa.setTxPower(LORA_TX_DBM, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.enableCrc();
  radioListen();
}

/* ===========================================================================
 * SECTION 7 — SEQUENCE NUMBERS
 *
 * A boot counter is not a message counter: two messages in one session would
 * share an ID and the second is dropped everywhere as a duplicate. And if seq
 * restarted at 0 after a battery swap, every neighbour's dedup cache would
 * reject our next 20 messages as replays. So: persist in NVS, and skip a gap
 * on boot to jump past anything still in flight.
 * ========================================================================= */
#define SEQ_COMMIT_STRIDE 16
#define SEQ_BOOT_GAP      32

static void seqInit() {
  prefs.begin("pager", false);
  uint16_t stored = prefs.getUShort("seq", 0);
  mySeq = stored + SEQ_BOOT_GAP;
  seqCommitted = mySeq;
  prefs.putUShort("seq", seqCommitted);
  prefs.end();
}
static uint16_t seqNext() {
  uint16_t s = ++mySeq;
  if (s >= seqCommitted) {
    prefs.begin("pager", false);
    seqCommitted = s + SEQ_COMMIT_STRIDE;
    prefs.putUShort("seq", seqCommitted);
    prefs.end();
  }
  return s;
}

/* ===========================================================================
 * SECTION 8 — TX PATH
 * ========================================================================= */

static bool rateLimited(uint8_t status) {
  uint32_t limit = (status == ST_SAFE) ? RATELIMIT_SAFE_MS : RATELIMIT_OTHER_MS;
  if (lastTxAt[status] == 0) return false;
  return !elapsed(lastTxAt[status], limit);
}

static void originate(uint8_t status) {
  if (txBurst.active) return;
  if (rateLimited(status)) {
    oledWake();
    oled.clearDisplay(); drawHeader();
    oled.setCursor(0, 24); oled.print("RATE LIMITED");
    oled.setCursor(0, 40); oled.print("Sent recently.");
    oled.display();
    delay(1200);
    uiScreen = UI_IDLE; uiDirty = true;
    return;
  }

  pager_pkt_t p = {};
  p.magic    = PROTO_MAGIC;
  p.ver      = PROTO_VERSION;
  p.hop      = 0;
  p.originId = myNodeId;
  p.seq      = seqNext();
  p.status   = status;
  p.prio     = (status == ST_RESCUE || status == ST_MEDICAL) ? 3 : 1;
  p.relayId  = myNodeId;
  p.vbat     = encodeVbat(readVbatMillivolts());
  p.mac      = computeMac(p);

  // Seed our own packet into the dedup cache so echoes are not relayed by us.
  seenAdd(p.originId, p.seq);

  txBurst = { true, p, TX_REPEATS, millis(), 0 };
  lastTxAt[status] = millis();

  if (status != ST_PING) {
    ackPending   = true;
    ackSeq       = p.seq;
    ackRelayId   = 0;
    ackDeadline  = millis() + ACK_WINDOW_MS;
  }

  oledWake();
  uiScreen = UI_SENDING;
  uiDirty = true;
  buzzStart(PAT_TICK);
}

static void txService() {
  if (!txBurst.active) return;
  if (!elapsed(txBurst.nextAt, 0)) return;

  if (!channelClear() && txBurst.defers < LBT_MAX_DEFERS) {
    txBurst.defers++;
    txBurst.nextAt = millis() + LBT_DEFER_MS + random(0, 80);
    return;
  }
  txBurst.defers = 0;

  radioSend(txBurst.pkt);
  txBurst.copiesLeft--;

  if (txBurst.copiesLeft == 0) {
    txBurst.active = false;
    uiScreen = UI_SENT;
    uiDirty = true;
    oledWake();
  } else {
    txBurst.nextAt = millis() + TX_REPEAT_GAP_MS + random(0, 120);
  }
}

/* ===========================================================================
 * SECTION 9 — RELAY (suppressed flooding)
 * ========================================================================= */

/* Backoff biased by RSSI: a node that heard the packet weakly is far from the
 * sender, so it relays sooner. This pushes the flood outward instead of
 * sideways, and lets close-in nodes cancel once they hear it covered.      */
static uint32_t relayBackoff(int rssi) {
  int r = constrain(rssi, -130, -40);
  long base = map(r, -130, -40, RELAY_BACKOFF_MIN_MS, RELAY_BACKOFF_MAX_MS);
  return (uint32_t)base + random(0, RELAY_JITTER_MS);
}

static void relaySchedule(const pager_pkt_t &p, int rssi) {
  if (p.hop + 1 > maxHopFor(p.status)) return;
  for (int i = 0; i < RELAY_QUEUE_SIZE; i++) {
    if (!relayQ[i].active) {
      relayQ[i].active   = true;
      relayQ[i].pkt      = p;
      relayQ[i].pkt.hop  = p.hop + 1;
      relayQ[i].pkt.relayId = myNodeId;
      relayQ[i].dueAt    = millis() + relayBackoff(rssi);
      relayQ[i].dupHeard = 0;
      return;
    }
  }
  // Queue full: the channel is saturated. Dropping is the correct behaviour.
}

/* Counter-based suppression: if RELAY_SUPPRESS_COUNT other nodes have already
 * relayed this packet while we were waiting, our copy adds nothing but
 * collisions. Cancel it.                                                   */
static void relayNoteDuplicate(uint8_t origin, uint16_t seq) {
  for (int i = 0; i < RELAY_QUEUE_SIZE; i++) {
    if (relayQ[i].active && relayQ[i].pkt.originId == origin &&
        relayQ[i].pkt.seq == seq) {
      if (++relayQ[i].dupHeard >= RELAY_SUPPRESS_COUNT) relayQ[i].active = false;
      return;
    }
  }
}

static void relayService() {
  if (txBurst.active) return;   // never step on our own burst
  for (int i = 0; i < RELAY_QUEUE_SIZE; i++) {
    if (!relayQ[i].active) continue;
    if (!elapsed(relayQ[i].dueAt, 0)) continue;
    if (!channelClear()) { relayQ[i].dueAt = millis() + LBT_DEFER_MS + random(0, 80); continue; }
    radioSend(relayQ[i].pkt);
    relayQ[i].active = false;
    return;                     // one relay per pass; let RX drain
  }
}

/* ===========================================================================
 * SECTION 10 — RX PATH
 * ========================================================================= */

static void handlePong(const pager_pkt_t &p, int rssi) {
  oledWake();
  oled.clearDisplay(); drawHeader();
  oled.setCursor(0, 20); oled.printf("REPLY  H%02u", p.originId);
  oled.setCursor(0, 34); oled.printf("RSSI %d dBm", rssi);
  oled.setCursor(0, 46); oled.printf("SNR  %.1f dB", LoRa.packetSnr());
  oled.display();
  buzzStart(PAT_TICK);
  uiScreen = UI_IDLE;
}

static void handlePing(const pager_pkt_t &p) {
  // Auto-reply after a random delay so 20 nodes do not answer at once.
  // PONG is never relayed (maxHop 0), so this cannot cascade.
  delay(random(300, 1500));
  pager_pkt_t r = {};
  r.magic = PROTO_MAGIC; r.ver = PROTO_VERSION; r.hop = 0;
  r.originId = myNodeId; r.seq = seqNext(); r.status = ST_PONG; r.prio = 0;
  r.relayId = myNodeId; r.vbat = encodeVbat(readVbatMillivolts());
  r.mac = computeMac(r);
  seenAdd(r.originId, r.seq);
  radioSend(r);
}

static void rxService() {
  int len = LoRa.parsePacket();
  if (len <= 0) return;

  // parsePacket() drops the radio to STDBY on a successful read.
  if (len != (int)sizeof(pager_pkt_t)) {
    while (LoRa.available()) LoRa.read();
    radioListen();
    return;
  }

  pager_pkt_t p;
  LoRa.readBytes((uint8_t *)&p, sizeof(p));
  int rssi = LoRa.packetRssi();
  radioListen();

  if (p.magic != PROTO_MAGIC)      return;   // foreign traffic
  if (p.ver   != PROTO_VERSION)    return;
  if (!macValid(p))                return;   // forged or corrupt
  if (p.originId == ID_UNSET)      return;

  bool isDuplicate = (seenFind(p.originId, p.seq) >= 0);

  /* --- Relay-ACK ------------------------------------------------------
   * Our own packet coming back means a real neighbour really received it.
   * This is the only honest delivery confirmation a broadcast can give.  */
  if (p.originId == myNodeId) {
    if (ackPending && p.seq == ackSeq && p.relayId != myNodeId) {
      ackPending   = false;
      ackRelayId   = p.relayId;
      ackRelayRssi = rssi;
      buzzStart(PAT_TICK);
      uiScreen = UI_SENT;
      oledWake();
    }
    return;   // never relay our own traffic
  }

  if (isDuplicate) {
    relayNoteDuplicate(p.originId, p.seq);
    return;
  }
  seenAdd(p.originId, p.seq);

  if (p.status == ST_PONG) { handlePong(p, rssi); return; }
  if (p.status == ST_PING) { relaySchedule(p, rssi); handlePing(p); return; }

  /* --- Real alert ---------------------------------------------------- */
  rosterUpdate(p, rssi);
  alertCount++;
  lastAlert     = p;
  lastAlertRssi = rssi;

  oledWake();
  uiScreen = UI_ALERT;
  uiDirty  = true;

  switch (p.status) {
    case ST_RESCUE:  buzzStart(PAT_RESCUE);  break;
    case ST_MEDICAL: buzzStart(PAT_MEDICAL); break;
    case ST_WATER:   buzzStart(PAT_WATER);   break;
    case ST_SAFE:    buzzStart(PAT_SAFE);    break;
  }

  relaySchedule(p, rssi);
}

/* ===========================================================================
 * SECTION 11 — BUTTONS
 * ========================================================================= */

struct Button {
  uint8_t  pin;
  bool     stable;        // true = released (INPUT_PULLUP idles HIGH)
  bool     lastRead;
  uint32_t lastChange;
  uint32_t pressedAt;
  bool     longFired;
};

static Button btn[4] = {
  { PIN_BTN_SAFE,    true, true, 0, 0, false },
  { PIN_BTN_MEDICAL, true, true, 0, 0, false },
  { PIN_BTN_WATER,   true, true, 0, 0, false },
  { PIN_BTN_RESCUE,  true, true, 0, 0, false },
};

static uint32_t rescueHeldMs = 0;

static void onShortPress(int i) {
  switch (i) {
    case 0: originate(ST_SAFE);    break;
    case 1: originate(ST_MEDICAL); break;
    case 2: originate(ST_WATER);   break;
    case 3: /* RESCUE never fires on a tap. Hold-to-confirm only. */
            oledWake(); uiScreen = UI_CONFIRM; uiDirty = true; break;
  }
}
static void onLongPress(int i) {
  switch (i) {
    case 0: oledWake(); uiScreen = UI_ROSTER; uiDirty = true; break;
    case 2: originate(ST_PING); break;
  }
}

static void buttonService() {
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    bool raw = digitalRead(btn[i].pin);          // LOW = pressed

    if (raw != btn[i].lastRead) {
      btn[i].lastRead   = raw;
      btn[i].lastChange = now;
    }
    if ((now - btn[i].lastChange) < DEBOUNCE_MS) continue;
    if (raw == btn[i].stable) continue;

    btn[i].stable = raw;
    if (!raw) {                                  // press edge
      btn[i].pressedAt = now;
      btn[i].longFired = false;
      oledWake();
    } else {                                     // release edge
      if (i == 3) {                              // RESCUE: confirm on release
        if (rescueHeldMs >= RESCUE_CONFIRM_MS) originate(ST_RESCUE);
        else { uiScreen = UI_IDLE; uiDirty = true; }
        rescueHeldMs = 0;
      } else if (!btn[i].longFired) {
        onShortPress(i);
      }
    }
  }

  // Held-down handling (long press, rescue confirm bar)
  for (int i = 0; i < 4; i++) {
    if (btn[i].stable) continue;                 // not held
    uint32_t held = now - btn[i].pressedAt;
    if (i == 3) {
      rescueHeldMs = held;
      if (uiScreen != UI_CONFIRM) { uiScreen = UI_CONFIRM; }
      uiLastActive = now;
      if (held >= RESCUE_CONFIRM_MS && !btn[i].longFired) {
        btn[i].longFired = true;
        buzzStart(PAT_TICK);                     // haptic-ish "armed" cue
      }
    } else if (held >= LONGPRESS_MS && !btn[i].longFired) {
      btn[i].longFired = true;
      onLongPress(i);
    }
  }
}

static bool anyButtonHeld() {
  for (int i = 0; i < 4; i++) if (!digitalRead(btn[i].pin)) return true;
  return false;
}

/* ===========================================================================
 * SECTION 12 — PROVISIONING
 *
 * One binary, N units. Hardcoding the node ID into 20 separate builds means
 * 20 chances at a duplicate and no way to identify a unit sitting in a drawer.
 * ========================================================================= */
static void provisioningMode() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 12); oled.print("PROVISIONING");
  oled.setCursor(0, 28); oled.print("Serial @115200");
  oled.setCursor(0, 40); oled.print("Enter node number");
  oled.display();

  Serial.println();
  Serial.println("== FloodPager provisioning ==");
  Serial.printf("Current node id: %u\n", myNodeId);
  Serial.printf("Enter new id (1-%u), then newline: ", ID_MAX);

  String s;
  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') { if (s.length()) break; }
      else s += c;
    }
    delay(5);
  }
  int id = s.toInt();
  if (id >= 1 && id <= ID_MAX) {
    prefs.begin("pager", false);
    prefs.putUChar("nodeId", (uint8_t)id);
    prefs.putUShort("seq", 0);
    prefs.end();
    Serial.printf("Saved. This unit is now HOUSE %02d. Rebooting.\n", id);
    oled.clearDisplay();
    oled.setCursor(0, 24); oled.printf("SAVED: HOUSE %02d", id);
    oled.display();
    delay(1500);
  } else {
    Serial.println("Invalid. No change.");
  }
  ESP.restart();
}

static void identityInit() {
  prefs.begin("pager", true);
  myNodeId = prefs.getUChar("nodeId", ID_UNSET);
  prefs.end();
}

/* ===========================================================================
 * SECTION 13 — POWER
 *
 * Light sleep, ~0.8 mA. The SX1276 stays in RX_CONTINUOUS (~11 mA) and holds
 * any received packet in its FIFO while the CPU is off. We wake on:
 *   - DIO0 HIGH  (RxDone)
 *   - any button LOW
 *   - a timer, bounded so the watchdog is fed and the display timeout fires
 *
 * Guard: if a wake source is already asserted, esp_light_sleep_start() returns
 * instantly and we spin. So never sleep with DIO0 high or a button down.
 * ========================================================================= */
static void sleepSourcesInit() {
  gpio_wakeup_enable((gpio_num_t)PIN_LORA_DIO0, GPIO_INTR_HIGH_LEVEL);
  for (int i = 0; i < 4; i++)
    gpio_wakeup_enable((gpio_num_t)btn[i].pin, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
}

static uint32_t nextDeadlineMs() {
  uint32_t now = millis(), best = MAX_SLEEP_MS;
  auto consider = [&](uint32_t at) {
    if ((int32_t)(at - now) <= 0) { best = 0; return; }
    uint32_t d = at - now;
    if (d < best) best = d;
  };
  if (txBurst.active) consider(txBurst.nextAt);
  for (int i = 0; i < RELAY_QUEUE_SIZE; i++)
    if (relayQ[i].active) consider(relayQ[i].dueAt);
  if (ackPending) consider(ackDeadline);
  if (oledOn)     consider(uiLastActive + DISPLAY_TIMEOUT_MS);
  return best;
}

static void maybeSleep() {
  if (txBurst.active || buzzBusy()) return;
  for (int i = 0; i < RELAY_QUEUE_SIZE; i++) if (relayQ[i].active) {
    if ((int32_t)(relayQ[i].dueAt - millis()) < 40) return;
  }
  if (anyButtonHeld())               return;   // would wake instantly
  if (digitalRead(PIN_LORA_DIO0))    return;   // packet waiting

  uint32_t d = nextDeadlineMs();
  if (d < 20) return;

  esp_sleep_enable_timer_wakeup((uint64_t)d * 1000ULL);
  esp_light_sleep_start();
}

/* ===========================================================================
 * SECTION 14 — SETUP / LOOP
 * ========================================================================= */

void setup() {
  setCpuFrequencyMhz(80);          // halves active current; plenty for this job
  Serial.begin(115200);

  pinMode(PIN_BUZZER, OUTPUT);
  for (int i = 0; i < 4; i++) pinMode(btn[i].pin, INPUT_PULLUP);
  analogSetPinAttenuation(PIN_VBAT_SENSE, ADC_11db);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setClock(400000);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
    // Not fatal: the buzzer still works, and the mesh still relays.
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.clearDisplay();
  oled.display();

  identityInit();

  // BTN_SAFE + BTN_RESCUE held at boot => provisioning
  delay(50);
  if (!digitalRead(PIN_BTN_SAFE) && !digitalRead(PIN_BTN_RESCUE)) provisioningMode();

  if (myNodeId == ID_UNSET) {
    oled.clearDisplay();
    oled.setCursor(0, 16); oled.print("NOT PROVISIONED");
    oled.setCursor(0, 32); oled.print("Hold B1+B4, reboot");
    oled.display();
    while (true) { delay(1000); }
  }

  seqInit();
  randomSeed(esp_random());
  radioInit();
  sleepSourcesInit();

  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  memset(seenCache, 0, sizeof(seenCache));
  memset(relayQ,    0, sizeof(relayQ));
  memset(roster,    0, sizeof(roster));
  txBurst.active = false;

  oledWake();
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 12); oled.printf("SYSTEM READY");
  oled.setTextSize(2);
  oled.setCursor(0, 28); oled.printf("HOUSE %02u", myNodeId);
  oled.setTextSize(1);
  oled.setCursor(0, 52); oled.printf("SF%d  %luMHz  %ddBm",
                LORA_SF, LORA_FREQ_HZ / 1000000UL, LORA_TX_DBM);
  oled.display();
  buzzStart(PAT_SAFE);
  delay(1500);
  uiScreen = UI_IDLE;
  uiDirty  = true;

  Serial.printf("FloodPager v%d ready. HOUSE %02u, seq base %u\n",
                PROTO_VERSION, myNodeId, mySeq);
}

void loop() {
  esp_task_wdt_reset();

  rxService();
  buttonService();
  txService();
  relayService();
  buzzService();

  // Expire the relay-ACK window so the UI can tell the truth.
  if (ackPending && elapsed(0, 0) && (int32_t)(millis() - ackDeadline) >= 0) {
    ackPending = false;
    if (uiScreen == UI_SENT) { uiDirty = true; oledWake(); }
  }

  uiService(rescueHeldMs);
  maybeSleep();
}
