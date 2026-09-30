# Firmware V4, next build (4.4.0): transmit power and power control

Implementation plan, not yet built. The reasoning is in `docs/architecture.md`
§1.1 (the gazette's Table II), §14.4 (antenna gain) and §16 (power,
bandwidth, adaptive power control). Change those first if the design changes.

Current build: 4.3.3, fixed 20 dBm on every frame.

## Scope

| # | Change | Status in `architecture.md` |
|---|---|---|
| 1 | Maximum transmit power **+22 dBm** | §16.1 DECIDED |
| 2 | **Antenna cap**: power limited from the fitted antenna's gain and cable loss | §14.4 PROPOSED |
| 3 | **Adaptive power control (APC)** on texts; SOS, ACKs and heartbeats at full power | §16.3 DECIDED to add, design PROPOSED |
| 4 | APC switch for range tests, and logging | §16.3 |

Not in this build, though also needed before the RF test (§15.5): the reserved
alarm budget (SOS and ACK frames still skip the duty-cycle limit), a lab test
build, and a resident build without Wi-Fi.

Bandwidth stays 125 kHz (§16.2): don't touch `FM_LORA_BW_KHZ`.

## 1. +22 dBm maximum

- `platformio.ini`: `-D FM_LORA_TX_DBM=22`.
- `include/fm_radio.h`: default 22, and fix the comment. It cites the
  superseded 1 W rule; it should say "SX1262 maximum; about 160 mW e.r.p. on
  the half-wave whip, inside G.S.R. 853(E) Table II's 500 mW".
- `FM_LORA_CURRENT_MA` stays 140 mA: enough for +22 dBm.
- `FLOODMESH_VERSION` → `4.4.0`.
- **Every unit in a test runs 4.4.0**, so they all share one maximum (§16.1).

## 2. Antenna cap

Per unit, in NVS (`FM_NVS_NAMESPACE`), because it describes the fitted
hardware; a factory reset keeps it, like the buzzer type.

- Stored: antenna gain in dBi and cable loss in dB, each ×10 as an `int16`.
  Default 2.2 dBi (the standard whip, §14.1) and 0 dB.
- `txMaxDbm = min(FM_LORA_TX_DBM, floor(29.15 − gain + loss))`, and never
  below −9 (the SX1262 minimum). Computed at boot and whenever the setting
  changes.
- Serial:
  - `ant` prints gain, loss, `txMaxDbm` and the e.r.p. it gives.
  - `ant <gain dBi> [loss dB]` saves and recomputes, e.g. `ant 6 1`.
  - `ant reset` goes back to the whip.
- Check against §14.4's table: whip → 22, 6 dBi + 1 dB → 22, 8 dBi + 1 dB →
  22, 12 dBi + 1 dB → 18.

## 3. Adaptive power control

### Radio
- `fm_radio`: add `fmRadioSetPower(int8_t dBm)`. Keep the current setting in
  a variable and only call RadioLib's `setOutputPower()` when it changes; call
  it only between transmissions, never while one is in flight.
- RadioLib's `setOutputPower()` also touches the overcurrent limit on some
  versions: re-apply `FM_LORA_CURRENT_MA` after it, as `fmRadioBegin()` does.

### Which frames use which power
In `fm_mesh.cpp` `sendNow()`, choose the power from the frame type
(`fmFrameType()`), for frames the unit originates and frames it relays:

| Frame | Power |
|---|---|
| SOS (alarm), ACK, heartbeat | `txMaxDbm` always |
| Text (incl. `PING` range-test texts) | `txMaxDbm − reduction` (below) |

### Link samples
- Every frame received that was sent at full power (SOS, ACK, heartbeat)
  adds a sample: its SNR from `fmRadioPoll()`, and the time. This is the last
  hop's link, whoever relayed it, which is what matters.
- Keep samples from the last 65 min (the forwarding window, §13.3): a ring of
  32 entries, oldest dropped.

### Reduction for texts
```
floor   = demodulation floor at FM_LORA_SF:
          SF7 −7.5 dB, SF8 −10, SF9 −12.5, SF10 −15, SF11 −17.5, SF12 −20
margin  = (lowest SNR sample in the window) − floor − 10 dB
reduce  = margin rounded down to an even number of dB, at least 0
power   = max(txMaxDbm − reduce, 8 dBm)
```
- **Full power instead** when there are fewer than 2 samples in the window,
  and for 15 min after the unit sent a text it then didn't hear being
  forwarded by anyone (it hears its own frame again when a neighbour relays
  it).
- Recompute once a minute, not per frame, so the power doesn't jitter. Move
  by at most one 2 dB step per minute when raising the reduction; drop it to
  zero immediately when a new weak sample arrives.

### Switch and logging
- Build flag `FM_APC` (default 1) and serial `apc on|off` (saved). With APC
  off every frame goes at `txMaxDbm`. **Turn it off for range tests**, or the
  `PING` texts go out at reduced power and the results mean nothing.
- `apc` prints: on/off, `txMaxDbm`, sample count, lowest SNR, the current
  reduction and the text power.
- Each transmit logs its power: add `,<dBm>` to the `[MESH]` tx lines.
- Field logger: record the transmit power with each tx record. This changes
  `docs/field-logger-protocol.md`, so change that first, then both sides; skip
  it in 4.4.0 if the web app can't be updated in time.

## 4. README updates

- Status line: 4.4.0 and what it adds.
- §1 table: rows for +22 dBm, the antenna cap and APC.
- §7 On air: which frames go at full power.
- §9 Serial commands: `ant`, `ant <gain> [loss]`, `ant reset`, `apc`,
  `apc on|off`.

## 5. Bench and field checks before release

1. Boot log shows `txMaxDbm 22` with the whip. `ant 12 1` gives 18;
   `ant reset` gives 22 again.
2. Two units 1 m apart, APC on: after a few heartbeats or SOS tests, `apc`
   shows a reduction, and text tx lines show the lower power. SOS and ACK tx
   lines still show 22.
3. Move one unit out of range of the other (or put it in a metal box): the
   reduction goes to zero on the next sample.
4. `apc off`: every tx line shows 22.
5. Current check, if a USB power meter is available: transmit current at 22
   against 20 dBm; confirm the regulator and battery cope.
6. Five-unit field test (A–E) on 4.4.0 with APC off, to compare range at
   +22 dBm against the 28 Sep results at 20 dBm. Then a run with APC on, to
   check that texts still get through.

This sandbox can't build firmware; CI (`.github/workflows/build.yml`) builds
all envs on push.
