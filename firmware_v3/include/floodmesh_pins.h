/**
 * FloodMesh firmware V3 - pin map for Heltec WiFi LoRa 32 V3 / V3.2
 * (ESP32-S3FN8 + SX1262 + SSD1306) with a 4x4 keypad and an active buzzer.
 *
 * V3 hardware is deliberately minimal: Heltec + keypad + buzzer. No mic, amp,
 * speaker, MCP23017 or side button. See firmware_v3/README.md for the wiring.
 *
 * ---------------------------------------------------------------------------
 * Pin choices
 * ---------------------------------------------------------------------------
 * Keypad COLUMNS are inputs with pull-ups on GPIO 4-7. These are RTC-capable,
 * so a later deep-sleep build can wake on any key (rows held LOW, ext1 wake on
 * the columns). They were the old 2x2 keypad pins and are known to be broken
 * out on the Heltec header.
 *
 * Keypad ROWS are outputs on GPIO 39-42, the pin-based JTAG pads. They have no
 * boot-time role and are free because the S3 uses USB-Serial-JTAG by default.
 * 40/41/42 were the microphone's pins in the voice build; V3 has no mic.
 *
 * The LoRa DIO1 interrupt is GPIO 14, which is RTC-capable: the radio CAN wake
 * the ESP32 from deep sleep on a received packet. (Not used yet in V3.)
 *
 *   27..32   in-package flash - never usable (checked below)
 *   43, 44   UART0 console - never usable (checked below)
 *   0, 3, 45, 46  strapping - not used
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

// ------------------------------------------------------------------ system
#define PIN_VEXT_CTRL        36   // Active LOW: powers the OLED
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
#define PIN_LORA_DIO1        14   // RTC-capable: radio wake from deep sleep is possible

// ------------------------------------------------------------------ 4x4 keypad
//        COL1 COL2 COL3 COL4
//  ROW1    1    2    3    A        A = UP
//  ROW2    4    5    6    B        B = DOWN
//  ROW3    7    8    9    C        C = OK
//  ROW4    *    0    #    D        D = BACK
#define PIN_KP_COL1           4
#define PIN_KP_COL2           5
#define PIN_KP_COL3           6
#define PIN_KP_COL4           7
#define PIN_KP_ROW1          39
#define PIN_KP_ROW2          40
#define PIN_KP_ROW3          41
#define PIN_KP_ROW4          42

// ------------------------------------------------------------------ buzzer
// ACTIVE buzzer (built-in oscillator): GPIO 2 -> 1 k -> 2N2222 base; emitter
// GND; collector -> buzzer (-); buzzer (+) -> 3V3 (not 5V: dead on battery).
#define PIN_BUZZER            2

// ==================================================================
// Compile-time pin-conflict checker
// ==================================================================
namespace floodmesh_pinmap {
constexpr uint8_t kUsedPins[] = {
    PIN_VEXT_CTRL, PIN_VBAT_ADC,  PIN_ADC_CTRL,  PIN_LED,
    PIN_OLED_SDA,  PIN_OLED_SCL,  PIN_OLED_RST,
    PIN_LORA_NSS,  PIN_LORA_SCK,  PIN_LORA_MOSI, PIN_LORA_MISO,
    PIN_LORA_RST,  PIN_LORA_BUSY, PIN_LORA_DIO1,
    PIN_KP_COL1,   PIN_KP_COL2,   PIN_KP_COL3,   PIN_KP_COL4,
    PIN_KP_ROW1,   PIN_KP_ROW2,   PIN_KP_ROW3,   PIN_KP_ROW4,
    PIN_BUZZER,
};
constexpr size_t kUsedCount = sizeof(kUsedPins) / sizeof(kUsedPins[0]);
constexpr bool hasDuplicate(size_t i = 0, size_t j = 1) {
  return (i >= kUsedCount)              ? false
       : (j >= kUsedCount)              ? hasDuplicate(i + 1, i + 2)
       : (kUsedPins[i] == kUsedPins[j]) ? true
       : hasDuplicate(i, j + 1);
}
constexpr bool usesForbidden(size_t i = 0) {
  return (i >= kUsedCount) ? false
       : ((kUsedPins[i] >= 27 && kUsedPins[i] <= 32) ||   // in-package flash
          kUsedPins[i] == 43 || kUsedPins[i] == 44 ||      // UART0 console
          kUsedPins[i] == 0 || kUsedPins[i] == 3 ||        // strapping
          kUsedPins[i] == 45 || kUsedPins[i] == 46) ? true
       : usesForbidden(i + 1);
}
constexpr bool rtcCapable(uint8_t p) { return p <= 21; }
}  // namespace floodmesh_pinmap
static_assert(!floodmesh_pinmap::hasDuplicate(),
              "V3 pin map: the same GPIO is assigned to two nets.");
static_assert(!floodmesh_pinmap::usesForbidden(),
              "V3 pin map: a flash, UART0 or strapping pin is in use.");
static_assert(floodmesh_pinmap::rtcCapable(PIN_KP_COL1) &&
              floodmesh_pinmap::rtcCapable(PIN_KP_COL2) &&
              floodmesh_pinmap::rtcCapable(PIN_KP_COL3) &&
              floodmesh_pinmap::rtcCapable(PIN_KP_COL4),
              "keypad columns must be RTC-capable (GPIO 0-21) for wake-on-key");
static_assert(floodmesh_pinmap::rtcCapable(PIN_LORA_DIO1),
              "LoRa DIO1 must be RTC-capable for wake-on-radio");
