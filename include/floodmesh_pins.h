/**
 * FloodMesh - master pin netlist for Heltec WiFi LoRa 32 V3 / V3.2
 * (ESP32-S3FN8 + SX1262 + SSD1306)
 *
 * THIS FILE MATCHES THE PHYSICALLY SOLDERED BOARD. Where a pin sits somewhere
 * the generic advice says to avoid, there is a comment explaining why it is
 * nonetheless safe in this specific circuit. Those are individual, reviewed
 * exceptions - not a relaxation of the rule.
 *
 * ---------------------------------------------------------------------------
 * ESP32-S3 pin hazards, and how each applies here
 * ---------------------------------------------------------------------------
 *   27..32   In-package quad SPI flash on the S3FN8. Never usable. Rejected
 *            unconditionally below.
 *   26       SPICS1 - the chip select for a SECOND SPI device, which on this
 *            part would be PSRAM. The S3FN8 has NO PSRAM, so nothing drives it
 *            and it is free. Reviewed exception, see FM_GPIO26_REVIEWED.
 *   45, 46   Strapping. Sampled at reset: 45 selects the VDD_SPI rail voltage,
 *            46 selects boot mode. Dangerous when an EXTERNAL device can drive
 *            them during reset. Safe when the ESP32 is the only driver.
 *   0, 3     Strapping (boot mode / JTAG select).
 *   19, 20   Native USB D-/D+. Usable here because the USB socket goes to the
 *            onboard UART bridge, at the cost of USB-CDC and USB-JTAG.
 *   39..42   Pin-based JTAG pads. Free by default.
 *   43, 44   UART0 console to the USB bridge.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

// ------------------------------------------------------------------ system
#define PIN_VEXT_CTRL        36   // Active LOW: powers OLED + external peripherals
#define VEXT_ON              LOW
#define VEXT_OFF             HIGH

#define PIN_VBAT_ADC          1   // Battery divider tap (ADC1_CH0)
#define PIN_ADC_CTRL         37   // Enables the battery divider
#define PIN_LED              35   // Onboard white LED
#define PIN_PRG_BTN           0   // Onboard PRG button (strapping, spare input)

// ------------------------------------------------------------------ OLED (I2C)
#define PIN_OLED_SDA         17
#define PIN_OLED_SCL         18
#define PIN_OLED_RST         21
#define OLED_I2C_ADDR      0x3C

// ------------------------------------------------------------------ SX1262 (SPI)
#define PIN_LORA_NSS          8
#define PIN_LORA_SCK          9
#define PIN_LORA_MOSI        10
#define PIN_LORA_MISO        11
#define PIN_LORA_RST         12
#define PIN_LORA_BUSY        13
#define PIN_LORA_DIO1        14

// ==================================================================
// 2x2 front keypad - a SCANNED MATRIX, not four switches
// ==================================================================
// The module has four pads and NO ground, because each key bridges one column
// line to one row line rather than shorting a pin to GND. Holding all four as
// pulled-up inputs detects nothing at all: a press just ties two high pins
// together and both stay high.
//
// Beware the silkscreen. The module labels its pads L1/L2/R1/R2, and those are
// MATRIX LINES, not key names - L1 is not "the top-left key". Mapping measured
// on the assembled hardware with the discovery build:
//
//        top-left  = L1 x R1        top-right  = L2 x R1
//        bot-left  = L1 x R2        bot-right  = L2 x R2
//
// Scanning: drive one ROW low, leave the other row high-Z, read both COLUMNS
// with pull-ups. A low column means the key at that intersection is down.
#define PIN_KEY_COL_L1        7
#define PIN_KEY_COL_L2        6
#define PIN_KEY_ROW_R1        5
#define PIN_KEY_ROW_R2        4

// No diodes on this module, so three simultaneous keys would ghost. Every
// gesture FloodMesh uses is at most two keys (PTT + one), and in a 2x2 no
// two-key combination can ghost, so the design is safe as built.

// ------------------------------------------------------------------ PTT
// Keys are identified by PHYSICAL POSITION, never by the module's line labels.
// These index the debounced key table in fm_input.
#define FM_KEY_TR             0   // top-right
#define FM_KEY_BR             1   // bottom-right
#define FM_KEY_BL             2   // bottom-left
#define FM_KEY_TL             3   // top-left
#define FM_KEY_COUNT          4

// --- Mode A, PTT up ------------------------------------------------------
//   top-right    channel up
//   bottom-right channel down          (becomes the PTT in bench test mode)
//   bottom-left  inbox previous        ... and CANCEL in the send window
//   top-left     play / select         ... and SEND in the send window
//
// --- Mode B, PTT held ----------------------------------------------------
//   top-right SAFE   bottom-right WATER   bottom-left MEDICAL   top-left EVACUATION

// ------------------------------------------------------------------ side PTT
// BENCH TEST MODE. The side switch is not fitted, so the BOTTOM-RIGHT key
// stands in: hold it to record, and hold it plus another key for that key's
// alarm.
//
// The cost is that the bottom-right key loses its own functions - no channel
// DOWN and no WATER alarm. Channel still cycles with the top-right key, so all
// four channels stay reachable; WATER simply cannot be sent on this build.
//
// Set to 0 and solder the side switch to GPIO 47 for the real layout.
#ifndef FM_TEST_PTT_ON_R2
#define FM_TEST_PTT_ON_R2 1
#endif

#if FM_TEST_PTT_ON_R2
  #define FM_PTT_IS_MATRIX_KEY   1
  #define FM_PTT_KEY             FM_KEY_BR
  #define PTT_HARDWARE_INSTALLED 1        // a real, soldered key
#else
  #define FM_PTT_IS_MATRIX_KEY   0
  #define PIN_BTN_PTT            47       // J2-13, the side switch
  #ifndef PTT_HARDWARE_INSTALLED
  #define PTT_HARDWARE_INSTALLED 0        // 0 until the switch is fitted
  #endif
#endif

// ------------------------------------------------------------------ alerting
#define PIN_BUZZER            2   // 2N2222 NPN base via 1k; active-high.
                                  // Collector rail is 3V3, NOT the board's 5V
                                  // pin - that is USB VBUS, dead on battery.

// ==================================================================
// I2S audio - two independent buses (as soldered)
// ==================================================================
// INMP441 (input). L/R tied to GND, so data lands in the LEFT slot.
// Captured at 16 kHz and decimated x2 in software: at 8 kHz the INMP441's bit
// clock would sit at 512 kHz, the bottom of its usable range.
#define PIN_MIC_WS           41   // word select
#define PIN_MIC_SCK          42   // bit clock
#define PIN_MIC_SD           40   // mic data out -> ESP32 in

// GPIO 42 is a pin-based JTAG pad (MTMS) with no boot-time role, and it is one
// of the board's default I2C pins so it is definitely broken out.
//
// The bit clock briefly lived on GPIO 45 in an earlier revision of this file,
// taken from a written pin list rather than from the board. GPIO 45 is the
// VDD_SPI strapping pin, and the mismatch cost an afternoon: the mic was never
// clocked, so it never drove its data line, and every captured sample was
// exactly zero - a symptom that looks identical to a dead microphone, a broken
// data wire, or the wrong I2S slot. Measure the board, do not trust the list.

// MAX98357A (output).
#define PIN_AMP_LRC          19   // word select  (also native USB D-)
#define PIN_AMP_BCLK         20   // bit clock    (also native USB D+)
#define PIN_AMP_DIN          48   // ESP32 out -> amplifier data in
#define PIN_AMP_SD           26   // amp sleep gate: LOW = shutdown, HIGH = play

// WHY GPIO 26 IS ACCEPTABLE, THOUGH IT SITS IN THE 26-32 RANGE:
// GPIO 26 is SPICS1 - the chip select for a SECOND SPI device. On this part
// that would be PSRAM, and the ESP32-S3FN8 has none: it carries 8 MB of quad
// flash in package, which uses GPIO 27-32 only (SPIHD 27, SPIWP 28, SPICS0 29,
// SPICLK 30, SPIQ 31, SPID 32) - ESP32-S3 datasheet Table 2-13, whose footnote
// 2 says plainly "CS1 is for in-package PSRAM". With no PSRAM fitted nothing
// inside the module drives GPIO 26, and Heltec break it out at J2-15
// (HTIT-WB32LA_V3.2 datasheet, Table 2.2-1).
//
// 27..32 remain unconditionally rejected below - those really are the flash bus.
#define FM_GPIO26_REVIEWED   PIN_AMP_SD

// *** AN EXTERNAL 1 kOHM PULLDOWN FROM SD_MODE TO GND IS REQUIRED. ***
//
// This is not optional tidiness. GPIO 26 comes out of reset input-enabled with
// an internal weak pull-up (~45 kOhm; datasheet Table 2-1 lists "IE, WPU" both
// at and after reset). Against the MAX98357A's internal 100 kOhm SD_MODE
// pulldown that divides to ~2.3 V - well above the part's 1.4 V threshold - so
// the amplifier is FULLY ENABLED from power-up until fmAudioBegin() runs, about
// a second later. GPIO 39 behaves the same way (MTCK also has a reset pull-up),
// so moving the pin does not avoid this.
//
// A 1 kOhm to GND brings the node to ~74 mV, under the 0.16 V shutdown
// threshold with better than 2x margin, and a 3.3 V push-pull output still
// drives it high without trouble. Do not substitute a larger value: 4.7 kOhm
// lands near 0.30 V, which is still an ENABLED band (right channel), not
// shutdown.
//
// Note also that GPIO 26 is powered from VDD_SPI, the flash rail, rather than
// VDD3P3_CPU. Driving it high through the 1 kOhm draws ~3.3 mA from that rail.
// That is expected to be harmless, but if flash instability ever appears during
// playback, move this net to GPIO 39 (J3-10), which sits on the normal CPU rail.

// ==================================================================
// Compile-time pin-conflict checker
// ==================================================================
namespace floodmesh_pinmap {

constexpr uint8_t kUsedPins[] = {
    PIN_VEXT_CTRL, PIN_VBAT_ADC, PIN_ADC_CTRL, PIN_LED,
    PIN_OLED_SDA,  PIN_OLED_SCL, PIN_OLED_RST,
    PIN_LORA_NSS,  PIN_LORA_SCK, PIN_LORA_MOSI, PIN_LORA_MISO,
    PIN_LORA_RST,  PIN_LORA_BUSY, PIN_LORA_DIO1,
    PIN_KEY_COL_L1, PIN_KEY_COL_L2, PIN_KEY_ROW_R1, PIN_KEY_ROW_R2,
#if !FM_PTT_IS_MATRIX_KEY
    PIN_BTN_PTT,                        // dedicated side switch
#endif
    PIN_BUZZER,
    PIN_MIC_WS,    PIN_MIC_SCK,  PIN_MIC_SD,
    PIN_AMP_LRC,   PIN_AMP_BCLK, PIN_AMP_DIN, PIN_AMP_SD,
};
constexpr size_t kUsedCount = sizeof(kUsedPins) / sizeof(kUsedPins[0]);

constexpr bool hasDuplicate(size_t i = 0, size_t j = 1) {
  return (i >= kUsedCount)              ? false
       : (j >= kUsedCount)              ? hasDuplicate(i + 1, i + 2)
       : (kUsedPins[i] == kUsedPins[j]) ? true
       : hasDuplicate(i, j + 1);
}

// The in-package quad flash bus. Not negotiable: a pin here means a chip that
// cannot fetch its own code. GPIO 26 is deliberately NOT in this range - see
// FM_GPIO26_REVIEWED above.
constexpr bool usesFlashPin(size_t i = 0) {
  return (i >= kUsedCount) ? false
       : (kUsedPins[i] >= 27 && kUsedPins[i] <= 32) ? true
       : usesFlashPin(i + 1);
}

constexpr bool usesUart0(size_t i = 0) {
  return (i >= kUsedCount) ? false
       : (kUsedPins[i] == 43 || kUsedPins[i] == 44) ? true
       : usesUart0(i + 1);
}

// Strapping pins are allowed only when they have been individually reviewed and
// named in an FM_GPIO*_REVIEWED macro. This forces a conscious decision with a
// written justification rather than a silent assignment.
constexpr bool strappingReviewed(uint8_t) {
  return false;   // nothing currently sits on a strapping pin
}
constexpr bool usesUnreviewedStrapping(size_t i = 0) {
  return (i >= kUsedCount) ? false
       : ((kUsedPins[i] == 45 || kUsedPins[i] == 46 || kUsedPins[i] == 3) &&
          !strappingReviewed(kUsedPins[i])) ? true
       : usesUnreviewedStrapping(i + 1);
}

}  // namespace floodmesh_pinmap

static_assert(!floodmesh_pinmap::hasDuplicate(),
              "FloodMesh pin map: the same GPIO is assigned to two nets.");
static_assert(!floodmesh_pinmap::usesFlashPin(),
              "FloodMesh pin map: GPIO 27-32 are the in-package SPI flash bus "
              "on the ESP32-S3FN8. That is not a wiring preference, it is a "
              "board that cannot fetch code.");
static_assert(!floodmesh_pinmap::usesUart0(),
              "FloodMesh pin map: GPIO 43/44 are the UART0 console.");
static_assert(!floodmesh_pinmap::usesUnreviewedStrapping(),
              "FloodMesh pin map: a strapping pin (3/45/46) is in use without a "
              "written review. Add an FM_GPIO*_REVIEWED entry explaining why "
              "nothing external can drive it during reset, or move the net.");
static_assert(PIN_AMP_DIN != PIN_OLED_RST,
              "MAX98357A DIN collides with the OLED reset line (GPIO21).");
static_assert(PIN_MIC_SD != PIN_AMP_DIN,
              "the two I2S buses still need separate data lines.");

// ------------------------------------------------------------------ soft warnings
#define FM_WARN_AMP_ON_USB_PINS  (PIN_AMP_LRC == 19 || PIN_AMP_LRC == 20 || \
                                  PIN_AMP_BCLK == 19 || PIN_AMP_BCLK == 20)
#define FM_WARN_NO_PTT           (!PTT_HARDWARE_INSTALLED)
#define FM_WARN_PTT_BORROWED     (FM_TEST_PTT_ON_R2)
