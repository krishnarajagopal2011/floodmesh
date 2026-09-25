/**
 * FloodMesh - MCP23017 4x4 keypad driver. See fm_keypad.h for wiring and the
 * scanning scheme.
 */
#include "fm_keypad.h"

#if FM_BOARD_PROTO_V2

#include <Wire.h>

namespace {

// MCP23017 registers, IOCON.BANK = 0 (power-on default).
const uint8_t kIODIRA   = 0x00;
const uint8_t kIODIRB   = 0x01;
const uint8_t kGPINTENA = 0x04;
const uint8_t kINTCONA  = 0x08;
const uint8_t kIOCON    = 0x0A;
const uint8_t kGPPUA    = 0x0C;
const uint8_t kGPPUB    = 0x0D;
const uint8_t kGPIOA    = 0x12;
const uint8_t kOLATA    = 0x14;

// IOCON: ODR (bit 2) = open-drain INT outputs, so the external pull-up sets
// the level and INTA can share a wire-OR later if needed.
const uint8_t kIoconValue = 0x04;

const uint8_t kColMask = 0x0F;   // columns on GPA0..GPA3
const uint8_t kRowShift = 4;     // rows on GPA4..GPA7
const uint8_t kRowMask = 0xF0;

// Physical cable order fix-ups: kRowPin[r] is the GPA row line (0..3) wired to
// logical row r; kColPin[c] likewise for columns.
#ifndef FM_KP_ROW_ORDER
#define FM_KP_ROW_ORDER {0, 1, 2, 3}
#endif
#ifndef FM_KP_COL_ORDER
#define FM_KP_COL_ORDER {0, 1, 2, 3}
#endif
const uint8_t kRowPin[FM_KP_ROWS] = FM_KP_ROW_ORDER;
const uint8_t kColPin[FM_KP_COLS] = FM_KP_COL_ORDER;

TwoWire &bus = Wire1;
bool     g_present = false;
uint16_t g_mask = 0;
uint32_t g_lastScanMs = 0;
bool     g_scanned = false;

bool writeReg(uint8_t reg, uint8_t val) {
  bus.beginTransmission(FM_KP_I2C_ADDR);
  bus.write(reg);
  bus.write(val);
  return bus.endTransmission() == 0;
}

bool readReg(uint8_t reg, uint8_t *val) {
  bus.beginTransmission(FM_KP_I2C_ADDR);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return false;
  if (bus.requestFrom((uint8_t)FM_KP_I2C_ADDR, (uint8_t)1) != 1) return false;
  *val = (uint8_t)bus.read();
  return true;
}

/** Idle: columns inputs, rows outputs driven LOW. */
void idleRows() {
  writeReg(kOLATA, 0x00);
  writeReg(kIODIRA, kColMask);   // columns in, rows out
}

}  // namespace

bool fmKeypadBegin() {
  bus.begin(PIN_KP_SDA, PIN_KP_SCL, 400000);
  bus.setTimeOut(20);
  pinMode(PIN_KP_INT, INPUT);   // external 10 k pull-up

  g_present = writeReg(kIOCON, kIoconValue);
  if (!g_present) {
    Serial.printf("[KEYPAD] !! no MCP23017 at 0x%02X on SDA=GPIO%u SCL=GPIO%u\n",
                  FM_KP_I2C_ADDR, PIN_KP_SDA, PIN_KP_SCL);
    return false;
  }
  // Port B unused: inputs with pull-ups so nothing floats and draws current.
  writeReg(kIODIRB, 0xFF);
  writeReg(kGPPUB, 0xFF);
  // Port A: pull-ups on the columns, interrupt on any change of a column.
  writeReg(kGPPUA, kColMask);
  writeReg(kINTCONA, 0x00);
  writeReg(kGPINTENA, kColMask);
  idleRows();
  uint8_t dummy;
  readReg(kGPIOA, &dummy);      // clears any pending interrupt
  Serial.printf("[KEYPAD] MCP23017 ok at 0x%02X (SDA=GPIO%u SCL=GPIO%u INT=GPIO%u)\n",
                FM_KP_I2C_ADDR, PIN_KP_SDA, PIN_KP_SCL, PIN_KP_INT);
  return true;
}

bool fmKeypadPresent() { return g_present; }

uint16_t fmKeypadScanNow() {
  if (!g_present) return 0;
  uint16_t mask = 0;
  for (uint8_t r = 0; r < FM_KP_ROWS; r++) {
    const uint8_t rowBit = (uint8_t)(1u << (kRowShift + kRowPin[r]));
    // Only the selected row is an output (latched LOW); others float.
    if (!writeReg(kIODIRA, (uint8_t)(0xFF & ~rowBit))) continue;   // only this row drives
    delayMicroseconds(30);
    uint8_t port = 0xFF;
    if (!readReg(kGPIOA, &port)) continue;
    for (uint8_t c = 0; c < FM_KP_COLS; c++) {
      if ((port & (1u << kColPin[c])) == 0) mask |= FM_KP_BIT(r * FM_KP_COLS + c);
    }
  }
  idleRows();
  uint8_t dummy;
  readReg(kGPIOA, &dummy);      // clear the interrupt the scan itself caused
  g_mask = mask;
  g_scanned = true;
  return mask;
}

uint16_t fmKeypadScan(uint32_t nowMs) {
  if (g_scanned && (nowMs - g_lastScanMs) < FM_KP_SCAN_MS) return g_mask;
  g_lastScanMs = nowMs;
  return fmKeypadScanNow();
}

uint16_t fmKeypadMask() { return g_mask; }

bool fmKeypadHeld(uint8_t idx) {
  return idx < FM_KP_KEYS && (g_mask & FM_KP_BIT(idx)) != 0;
}

void fmKeypadArmWake() {
  if (!g_present) return;
  idleRows();
  writeReg(kGPINTENA, kColMask);
  uint8_t dummy;
  readReg(kGPIOA, &dummy);
}

#else  // !FM_BOARD_PROTO_V2: no keypad on this board

bool     fmKeypadBegin() { return false; }
bool     fmKeypadPresent() { return false; }
uint16_t fmKeypadScanNow() { return 0; }
uint16_t fmKeypadScan(uint32_t) { return 0; }
uint16_t fmKeypadMask() { return 0; }
bool     fmKeypadHeld(uint8_t) { return false; }
void     fmKeypadArmWake() {}

#endif  // FM_BOARD_PROTO_V2

char fmKeypadChar(uint8_t idx) {
  static const char kMap[FM_KP_KEYS] = {'1', '2', '3', 'A', '4', '5', '6', 'B',
                                        '7', '8', '9', 'C', '*', '0', '#', 'D'};
  return idx < FM_KP_KEYS ? kMap[idx] : '?';
}

void fmKeypadMaskToString(uint16_t mask, char *out, size_t n) {
  if (!out || n == 0) return;
  size_t w = 0;
  for (uint8_t i = 0; i < FM_KP_KEYS && w + 1 < n; i++) {
    if (mask & FM_KP_BIT(i)) out[w++] = fmKeypadChar(i);
  }
  out[w] = '\0';
}
