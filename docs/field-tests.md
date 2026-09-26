# FloodMesh field tests

Outdoor test results, newest first. Each entry records the firmware and radio
settings, how the test was run, what was measured, and what was **not**
measured, so later results can be compared like for like.

Radio reference used below: SX1262 sensitivity at SF7 / 125 kHz is about
**−124 dBm**, and SF7 decodes down to an SNR of about **−7.5 dB**. The thermal
noise floor in 125 kHz is about −117 dBm; real urban noise can be higher.

---

## 2026-09-26: SOS through one relay (firmware V3)

**Setup**

| | |
|---|---|
| Firmware | `firmware_v3` 3.1.0 |
| Radio | 866.5 MHz, SF7, 125 kHz, CR 4/5, 20 dBm, stock Heltec antennas |
| Units | A = user, B = relay, E = responder (Heltec WiFi LoRa 32 V3) |
| Traffic | SOS alarms only (22-byte frame, ~65 ms), each sent once; V3 has no retries |
| Duration | about 15 minutes |
| Relay height | **road level the whole time**; B was never raised |

**Results**

| Link | Distance | Signal where it last worked | Outcome |
|---|---|---|---|
| A → E direct | **350 m** | −122 dBm (at E) | Nothing received beyond 350 m |
| A → B | 300 m, line of sight | −112 dBm (at B) | Received |
| B → E | 250 m, line of sight | −114 dBm (at E) | Received; **failed beyond 250 m** |
| A → B → E total | **550 m** | | Limit of the relay chain at road level |

Other observations:

- Holding the unit **out of a car window** worked better than inside the car
  with the windows up.
- Standing **in front of a shop** worked better than sitting inside its ground
  floor (masonry and glass walls).
- It only worked with line of sight between the units.
- Battery showed 65% → 62% → 65% within 15 minutes.

**Not measured:** SNR at any point; how many of several sends arrived at each
spot; what was between B and E where the chain failed; B at any height; battery
consumption.

**Interpretation**

- **350 m direct is a real edge**: −122 dBm is about 2 dB above SF7
  sensitivity. It matches the 350 m of the first test (below).
- **Road level costs a lot.** At 300 m in free space, 20 dBm would arrive at
  about −61 dBm; B measured −112 dBm. With both antennas about 1 m above the
  road, the ground, cars and people fill the first Fresnel zone, which is about
  5 m in radius at the middle of a 300 m link. Bodies, the small stock antennas
  and vehicles account for the rest.
- **Why a −114 dBm hop failed just beyond 250 m** cannot be settled without SNR.
  Three causes fit: local noise above the thermal floor (then −114 dBm is
  already at the edge), a sudden obstruction past that point (a corner,
  building or trees can cost 10–20 dB within metres), or a fade (at road level
  the signal swings about ±10 dB within a step, and each SOS was sent only
  once).
- **Raising the relay is the largest available gain.** B is one end of both
  hops, so height helps both. Going from ~1 m to 6 m should gain roughly
  15 dB, and to 10 m roughly 20 dB, per hop (two-ray estimate, not measured).
  This agrees with the 2026-09-14 range roadmap: distance comes from raised
  relays, not stronger handhelds.
- **Indoors costs 10–20 dB** (car body, masonry and glass), in line with
  `architecture.md` §1.2. User guidance should say to keep the unit near a
  window or on an upper floor; relays belong outside and high.
- **The battery percentage is voltage-based.** It dips while transmitting and
  recovers, so 15 minutes says nothing about consumption. Relay volunteering
  (§4.4, start at 50%, stop at 35%) needs a smoothed reading; the MAX17048 fuel
  gauge on the PCB gives a real state of charge.

**Next test**

1. Same layout with **B raised** (terrace, first-floor balcony or a pole of
   6 m or more), then walk E further until it fails.
2. Record **SNR** as well as dBm.
3. Use A's **range ping** (every 30 s) instead of single SOS messages, and note
   how many of about 4 pings arrive at each spot (4/4 solid, 2/4 fading).
4. At the failure point, note what is between the units.
5. Battery life at home: one fully charged unit, ping on, voltage every hour
   until it stops.
6. Later: 22 dBm and SF8 / SF9 as A/B comparisons.

---

## 2026-09-14: first range test (voice firmware)

Root firmware (voice build) on two Heltec V3 units. One tester stood in front
of a house; the other sat in a car holding the unit out of the window and moved
to find the limit. Trees and buildings were between them (no line of sight).

- Voice worked up to about **350 m**; beyond that nothing at all was received.
- No alarms were tried, and no RSSI or SNR was logged.
- A note sent out of range vanished without any indication, which led to the
  delivery-confirmation states (NOT SENT / NOT CONFIRMED / DELIVERED).

Conclusion at the time: 350 m fits a ~130 dB link budget minus about 30 dB of
clutter, body, car and ground losses, which points at test geometry rather
than a weak radio. The recommended next steps were a range-test mode with
RSSI/SNR on screen (now in V3) and raised relays.
