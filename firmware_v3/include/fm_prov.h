/**
 * FloodMesh - BLE provisioning mode (FMREG v1).
 *
 * The protocol is specified in docs/ble-provisioning-protocol.md; this module
 * is the unit side of it. The admin app is app/floodmesh_admin.
 *
 * Provisioning mode is a separate boot path: the radio, audio and codec are
 * never started, so BLE has the RAM to itself. fmProvRun() never returns; it
 * reboots into normal mode when done, on timeout, or after too many wrong PINs.
 */
#pragma once
#include <Arduino.h>

#ifndef FM_PROV_IDLE_TIMEOUT_MS
#define FM_PROV_IDLE_TIMEOUT_MS (5UL * 60UL * 1000UL)   // no connection for 5 min
#endif
#ifndef FM_PROV_MAX_MS
#define FM_PROV_MAX_MS (20UL * 60UL * 1000UL)           // hard cap on the session
#endif
#ifndef FM_PROV_MAX_FAILS
#define FM_PROV_MAX_FAILS 5
#endif

/**
 * Screen callback: four short text lines (any may be empty). Called whenever
 * the provisioning state changes. The first line is the title.
 */
typedef void (*FmProvDraw)(const char *l1, const char *l2, const char *l3, const char *l4);

/** Run provisioning mode. Does not return. */
void fmProvRun(FmProvDraw draw);
