/**
 * FloodMesh - SX1262 driver layer implementation.
 * See fm_radio.h for the RadioLib hazards this file works around and for the
 * backoff arithmetic.
 *
 * All time arithmetic uses unsigned subtraction (now - then), which is correct
 * across the 49.7-day millis() rollover. Never compare timestamps directly.
 */
#include "fm_radio.h"

#include <RadioLib.h>
#include <esp_random.h>

namespace {

// ------------------------------------------------------------------ hardware
// The SX1262 is the only device on this SPI bus (the OLED is I2C), so it gets
// its own SPIClass instance rather than sharing the global `SPI`. The Heltec V3
// pin-out is not the ESP32-S3 default for any bus, but the S3 GPIO matrix routes
// FSPI anywhere, so an explicit begin() with the pin map is all that is needed.
SPIClass g_spi(FSPI);
Module   g_mod(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, g_spi,
               SPISettings(FM_RADIO_SPI_HZ, MSBFIRST, SPI_MODE0));
SX1262   g_radio(&g_mod);

// ------------------------------------------------------------------ CAD config
/**
 * FM_CAD_SYMBOLS is a symbol COUNT; the chip wants an enum. This is the single
 * place the translation happens, and the static_assert below makes an illegal
 * count a build error rather than a radio that scans for the wrong duration.
 * Getting this wrong is silent at runtime, which is why it is loud here.
 */
constexpr uint8_t kCadSymNum =
    (FM_CAD_SYMBOLS == 1)    ? RADIOLIB_SX126X_CAD_ON_1_SYMB
    : (FM_CAD_SYMBOLS == 2)  ? RADIOLIB_SX126X_CAD_ON_2_SYMB
    : (FM_CAD_SYMBOLS == 4)  ? RADIOLIB_SX126X_CAD_ON_4_SYMB
    : (FM_CAD_SYMBOLS == 8)  ? RADIOLIB_SX126X_CAD_ON_8_SYMB
    : (FM_CAD_SYMBOLS == 16) ? RADIOLIB_SX126X_CAD_ON_16_SYMB
                             : 0xFF;

static_assert(kCadSymNum != 0xFF,
              "FM_CAD_SYMBOLS must be 1, 2, 4, 8 or 16 - it is a symbol count, "
              "and the SX1262 only implements those five.");
static_assert(FM_CAD_CW_MIN >= 1 && FM_CAD_CW_MIN <= FM_CAD_CW_MAX,
              "FM_CAD_CW_MIN must be at least 1 and no larger than FM_CAD_CW_MAX.");
static_assert(FM_CAD_MAX_TRIES >= 1, "at least one CAD attempt is required.");
static_assert(FM_RADIO_MAX_LEN > 0 && FM_RADIO_MAX_LEN <= 255,
              "FM_RADIO_MAX_LEN must fit the SX1262 LoRa payload limit.");

const uint16_t kCadIrqBits =
    RADIOLIB_SX126X_IRQ_CAD_DONE | RADIOLIB_SX126X_IRQ_CAD_DETECTED;

// ------------------------------------------------------------------ state
bool     g_ready    = false;  // begin() succeeded
bool     g_rxWanted = false;  // caller asked to be listening
bool     g_txActive = false;  // a packet is on the air
uint32_t g_txStartMs   = 0;
uint32_t g_txTimeoutMs = 0;   // watchdog: 5x expected time-on-air
float    g_lastSnr     = 0.0f;

/**
 * Set by the DIO1 ISR and cleared before every register read. Nothing else may
 * be done in that ISR: no SPI (the bus driver takes a mutex), no Serial, no
 * allocation. The single dispatcher exists because setDio1Action,
 * setPacketReceivedAction, setPacketSentAction and setChannelScanAction are all
 * the same function and overwrite each other - see fm_radio.h note 6.
 */
volatile bool g_dio1Fired = false;

/**
 * IRQ bits seen but not yet acted on. Needed because both fmRadioPoll() and
 * fmRadioIsTransmitting() drain the ISR flag: without an accumulator, whichever
 * ran first would consume the edge and the other would never see its event.
 */
uint16_t g_pending = 0;

void IRAM_ATTR dio1Isr() { g_dio1Fired = true; }

/**
 * Fold any new hardware IRQ state into g_pending.
 *
 * The flag is cleared BEFORE the register is read, which makes the ISR race
 * harmless: an edge landing between the two lines is already reflected in the
 * value we are about to read, and re-arms the flag so the next call reads
 * again. Worst case we look twice; we never miss an event. That is why there is
 * no critical section here.
 */
void pumpIrq() {
  if (!g_dio1Fired) return;
  g_dio1Fired = false;
  g_pending |= (uint16_t)g_radio.getIrqFlags();
}

/** Re-arm continuous receive if the caller ever asked for it. */
void resumeRx() {
  if (!g_rxWanted) return;
  g_dio1Fired = false;
  g_pending = 0;
  g_radio.startReceive();
}

/**
 * One CAD, bounded. Returns true for "busy". restoreRx is false when the caller
 * is about to transmit and would only have to tear the receiver down again.
 *
 * getChannelScanResult() reports RADIOLIB_LORA_DETECTED / RADIOLIB_CHANNEL_FREE
 * once the scan lands and something else (ERR_UNKNOWN) while neither CAD bit is
 * set, which is exactly the "not finished" signal this loop needs.
 */
bool cadOnce(uint32_t timeoutMs, bool restoreRx) {
  g_dio1Fired = false;
  g_pending = 0;

  ChannelScanConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.cad.symNum   = kCadSymNum;
  cfg.cad.detPeak  = RADIOLIB_SX126X_CAD_PARAM_DEFAULT;  // driver picks SF+13
  cfg.cad.detMin   = RADIOLIB_SX126X_CAD_PARAM_DEFAULT;  // driver picks 10
  cfg.cad.exitMode = RADIOLIB_SX126X_CAD_GOTO_STDBY;     // never auto-enter Rx
  cfg.cad.timeout  = 0;                                  // only used by GOTO_RX
  cfg.cad.irqFlags = RADIOLIB_IRQ_CAD_DEFAULT_FLAGS;
  cfg.cad.irqMask  = RADIOLIB_IRQ_CAD_DEFAULT_MASK;

  if (g_radio.startChannelScan(cfg) != RADIOLIB_ERR_NONE) {
    // The scan never began, so there is nothing to conclude and nothing to
    // clear. Fail open, as documented.
    if (restoreRx) resumeRx();
    return false;
  }

  bool           busy  = false;
  const uint32_t start = millis();
  for (;;) {
    const int16_t res = g_radio.getChannelScanResult();
    if (res == RADIOLIB_LORA_DETECTED) {
      busy = true;
      break;
    }
    if (res == RADIOLIB_CHANNEL_FREE) {
      break;
    }
    if ((millis() - start) >= timeoutMs) {
      break;  // inconclusive -> free; see the fail-open note in the header
    }
    delay(1);  // yields to FreeRTOS rather than spinning the core
  }

  // Mandatory: getChannelScanResult() does not clear these, and a latched DIO1
  // means the next scan produces no rising edge at all.
  g_radio.clearIrqFlags(kCadIrqBits);
  g_dio1Fired = false;
  g_pending = 0;

  if (restoreRx) resumeRx();
  return busy;
}

/** Drop to standby and hand the packet to the radio. */
int16_t launchTx(const uint8_t *buf, size_t len) {
  // stageMode(TX) writes the payload buffer without dropping out of receive
  // first, so standby here rather than trusting it.
  int16_t state = g_radio.standby();
  if (state != RADIOLIB_ERR_NONE) return state;

  g_dio1Fired = false;
  g_pending = 0;

  state = g_radio.startTransmit(buf, len);
  if (state != RADIOLIB_ERR_NONE) {
    resumeRx();
    return state;
  }

  // Same watchdog RadioLib uses in its blocking transmit(): 5 ms plus five
  // times the expected time-on-air. getTimeOnAir() is in microseconds.
  g_txActive    = true;
  g_txStartMs   = millis();
  g_txTimeoutMs = 5UL + (uint32_t)((g_radio.getTimeOnAir(len) * 5UL) / 1000UL);
  return RADIOLIB_ERR_NONE;
}

/**
 * Finish or abandon an in-flight transmission. Called from both fmRadioPoll()
 * and fmRadioIsTransmitting() so the caller cannot forget it.
 */
void serviceTx() {
  if (!g_txActive) return;

  pumpIrq();
  if (g_pending & RADIOLIB_SX126X_IRQ_TX_DONE) {
    g_radio.finishTransmit();  // clears every IRQ flag and returns to standby
    g_txActive  = false;
    g_dio1Fired = false;
    g_pending   = 0;
    resumeRx();
    return;
  }

  if ((millis() - g_txStartMs) > g_txTimeoutMs) {
    // TX_DONE never arrived. Left alone this latches fmRadioIsTransmitting()
    // forever and the node stops both sending and receiving.
    fmRadioAbortTx();
  }
}

}  // namespace

// ---------------------------------------------------------------- API
bool fmRadioBegin() {
  g_ready    = false;
  g_rxWanted = false;
  g_txActive = false;
  g_pending  = 0;
  g_dio1Fired = false;
  g_lastSnr  = 0.0f;

  g_spi.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

  // begin() also issues setDio2AsRfSwitch(true) and calibrates, so the TCXO
  // voltage has to go in here rather than being patched up afterwards.
  int16_t state = g_radio.begin(
      FM_LORA_FREQ_MHZ, FM_LORA_BW_KHZ, (uint8_t)FM_LORA_SF, (uint8_t)FM_LORA_CR,
      (uint8_t)FM_LORA_SYNC_WORD, (int8_t)FM_LORA_TX_DBM,
      (uint16_t)FM_LORA_PREAMBLE, (float)FM_LORA_TCXO_V,
      FM_LORA_USE_LDO ? true : false);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERR] SX1262 begin failed: %d (-706/-707 usually means the "
                  "TCXO voltage is wrong for this board)\n",
                  (int)state);
    return false;
  }

  // begin() takes power as an argument but clamps silently; setting it again
  // surfaces an out-of-range value as an error we can print.
  state = g_radio.setOutputPower((int8_t)FM_LORA_TX_DBM);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERR] SX1262 setOutputPower(%d dBm) failed: %d\n",
                  (int)FM_LORA_TX_DBM, (int)state);
    return false;
  }

  state = g_radio.setCurrentLimit(FM_LORA_CURRENT_MA);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERR] SX1262 setCurrentLimit failed: %d\n", (int)state);
    return false;
  }

  state = g_radio.setCRC(FM_LORA_CRC_BYTES);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERR] SX1262 setCRC failed: %d\n", (int)state);
    return false;
  }

  // Explicit header: fragment payloads vary (192 B vs the last one's 156 B), so
  // the length has to travel with the packet.
  state = g_radio.explicitHeader();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERR] SX1262 explicitHeader failed: %d\n", (int)state);
    return false;
  }

#if FM_LORA_RX_BOOST
  state = g_radio.setRxBoostedGainMode(true, true);
  if (state != RADIOLIB_ERR_NONE) {
    // Not fatal: this only costs sensitivity, so warn and carry on rather than
    // refusing to bring the radio up at all.
    Serial.printf("[WARN] SX1262 setRxBoostedGainMode failed: %d\n", (int)state);
  }
#endif

  // ONE handler for the life of the program. Anything that attaches another
  // DIO1 action later silently disables this one.
  g_radio.setDio1Action(dio1Isr);

  g_radio.standby();
  g_radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  g_dio1Fired = false;
  g_pending = 0;

  g_ready = true;
  return true;
}

bool fmRadioChannelBusy(uint32_t timeoutMs) {
  if (!g_ready) return false;  // fail open, as documented
  if (g_txActive) return true; // our own carrier is on the channel
  return cadOnce(timeoutMs, true);
}

int16_t fmRadioTransmit(const uint8_t *buf, size_t len) {
  if (!g_ready) return FM_RADIO_ERR_NOT_READY;
  if (buf == NULL || len == 0 || len > (size_t)FM_RADIO_MAX_LEN) {
    return FM_RADIO_ERR_TOO_LONG;
  }

  serviceTx();
  if (g_txActive) return FM_RADIO_ERR_TX_ACTIVE;

  uint16_t cw = FM_CAD_CW_MIN;
  for (uint8_t attempt = 0; attempt < FM_CAD_MAX_TRIES; attempt++) {
    if (!cadOnce(FM_CAD_TIMEOUT_MS, false)) {
      return launchTx(buf, len);
    }

    if ((uint8_t)(attempt + 1) >= (uint8_t)FM_CAD_MAX_TRIES) break;

    // Truncated binary exponential backoff. rand() % cw can be 0, which is the
    // point: the window has to include "go immediately" or the first retry is
    // just a fixed delay and two synchronised nodes stay synchronised.
    const uint32_t waitMs = (uint32_t)(esp_random() % cw) * (uint32_t)FM_CAD_SLOT_MS;
    cw = (cw >= (uint16_t)FM_CAD_CW_MAX) ? (uint16_t)FM_CAD_CW_MAX
                                         : (uint16_t)(cw << 1);

    // Listen through the wait. A node that defers to a neighbour and then does
    // not hear what the neighbour said has paid the cost of politeness twice.
    resumeRx();
    if (waitMs > 0) delay(waitMs);
  }

  resumeRx();
  return FM_RADIO_ERR_BUSY;
}

void fmRadioAbortTx() {
  if (!g_ready) return;

  // standby() IS the abort - there is no cancel command on the SX1262, and
  // dropping to STDBY_RC mid-packet is legal (SPI stays live during TX).
  g_radio.standby();
  g_radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);

  g_txActive  = false;
  g_dio1Fired = false;
  g_pending   = 0;

  resumeRx();
}

bool fmRadioStartReceive() {
  if (!g_ready) return false;

  g_rxWanted = true;

  // Do not stamp on a packet that is halfway out of the PA; serviceTx() will
  // re-arm receive the moment it completes.
  if (g_txActive) return true;

  g_dio1Fired = false;
  g_pending = 0;
  return g_radio.startReceive() == RADIOLIB_ERR_NONE;
}

bool fmRadioPoll(uint8_t *buf, size_t cap, size_t *outLen, float *rssi, float *snr) {
  if (outLen) *outLen = 0;
  if (!g_ready) return false;

  serviceTx();
  if (!g_rxWanted || g_txActive) return false;

  pumpIrq();
  if (!(g_pending & RADIOLIB_SX126X_IRQ_RX_DONE)) return false;
  g_pending = 0;  // readData() below clears the hardware flags for us

  // Length has to be read before readData() so the oversize case can be
  // detected; readData() would happily clamp and hand up a truncated frame.
  const size_t len = g_radio.getPacketLength(true);

  if (buf == NULL || cap == 0 || len == 0 || len > cap) {
    // Drain one byte anyway. Skipping readData() leaves RX_DONE latched, DIO1
    // high, and the receiver permanently deaf to the next packet.
    uint8_t sink = 0;
    g_radio.readData(&sink, 1);
    resumeRx();
    return false;
  }

  if (g_radio.readData(buf, len) != RADIOLIB_ERR_NONE) {
    // CRC or header failure. The frame is gone; re-arming costs a few SPI
    // transactions and guarantees we are not sitting in a half-broken Rx state.
    resumeRx();
    return false;
  }

  g_lastSnr = g_radio.getSNR();
  if (snr) *snr = g_lastSnr;
  if (rssi) *rssi = g_radio.getRSSI();
  if (outLen) *outLen = len;
  return true;
}

float fmRadioLastSnr() { return g_lastSnr; }

bool fmRadioIsTransmitting() {
  if (!g_ready) return false;
  serviceTx();
  return g_txActive;
}
