/**
 * FloodMesh V3 - WiFi update mode: firmware updates without the USB cable.
 *
 * Off by default. WiFi draws ~100 mA and disturbs LoRa timing, so it only runs
 * in an update window that someone opens on purpose (serial `ota`, or hold B at
 * power-on) and that closes itself after FM_OTA_WINDOW_MS.
 *
 * In the window the unit joins the stored WiFi network (a phone hotspot) and
 * serves:
 *
 *   GET  /id      "FloodMesh <callsign> <version> <image> <mac> up=<s>"  (no auth)
 *   POST /update  firmware image, HTTP auth fm / FM_OTA_PASSWORD
 *   GET  /close   end the window now                    (same auth)
 *
 * <image> is the first 8 hex digits of the SHA-256 of the firmware ELF, which
 * esptool embeds in the app header, so the PC can prove which build a unit is
 * running. The PC only makes outgoing connections to the units, so it needs no
 * inbound firewall rule on a "public" hotspot network.
 *
 * The image goes into the other OTA slot and the boot slot only switches after
 * the whole image has arrived and verified. A dropped connection leaves the
 * running firmware untouched. After a successful update the unit reboots and
 * reopens the window for FM_OTA_RESUME_MS, so the PC can verify the new build.
 *
 * Credentials live in NVS in plain text, acceptable for bench units; flash
 * encryption is the production answer (architecture.md 6.2). The WiFi password
 * is never printed.
 *
 * Built without FM_OTA_PASSWORD the window refuses to open: an unauthenticated
 * update server would let anyone on the same WiFi reflash the unit.
 */
#pragma once
#include <Arduino.h>

#include "fm_prov.h"   // FmProvDraw

#ifndef FM_OTA_WINDOW_MS
#define FM_OTA_WINDOW_MS (10UL * 60UL * 1000UL)
#endif
#ifndef FM_OTA_RESUME_MS
#define FM_OTA_RESUME_MS (5UL * 60UL * 1000UL)   // window after an OTA reboot
#endif
#ifndef FM_OTA_JOIN_MS
#define FM_OTA_JOIN_MS 30000UL                   // give up joining the WiFi after this
#endif

enum FmOtaEvent : uint8_t {
  FM_OTA_EV_NONE = 0,
  FM_OTA_EV_JOINED,   // on the network, update server listening
  FM_OTA_EV_FAILED,   // could not join; window closed
  FM_OTA_EV_CLOSED,   // window ended (timeout, `ota off`, /close)
};

/** Once from setup(). `draw` paints full-screen progress during a transfer. */
void fmOtaBegin(FmProvDraw draw);

/** True when the last reset was an OTA reboot that should reopen the window. */
bool fmOtaResumePending();

bool fmOtaSetSsid(const char *ssid);   // 1..32 characters
bool fmOtaSetPass(const char *pass);   // 8..63 characters, or "" for an open network
void fmOtaForget();                    // wipe the stored network (factory reset)
bool fmOtaHasSsid();
const char *fmOtaSsid();
bool fmOtaHasPass();

/** Open the window for `ms`. False, with the reason on serial, if it cannot. */
bool fmOtaStart(uint32_t ms);
void fmOtaStop();
bool fmOtaActive();

/** Call every loop; cheap when idle. Returns a state change for the UI. */
FmOtaEvent fmOtaLoop(uint32_t now);

/** "fm-a" - the hostname, also shown on screen. */
const char *fmOtaHost();
/** Dotted IP while joined, else "". */
const char *fmOtaIp();
/** Whole minutes left in the window. */
uint32_t fmOtaMinutesLeft(uint32_t now);
/** First 8 hex digits of the running image's ELF SHA-256. */
const char *fmOtaImageId();
