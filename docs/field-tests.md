# FloodMesh field tests

Outdoor test results, newest first. Each entry records the firmware and radio
settings, how the test was run, what was measured, and what was **not**
measured, so later results can be compared like for like.

Radio reference used below: SX1262 sensitivity at SF7 / 125 kHz is about
**−124 dBm**, and SF7 decodes down to an SNR of about **−7.5 dB**. The thermal
noise floor in 125 kHz is about −117 dBm; real urban noise can be higher.

---

## 2026-09-28: first V4 test, roof unit (firmware V4 4.2.0)

**Setup**

| | |
|---|---|
| Firmware | `firmware_v4` 4.2.0; E ran the test-only env `v4_bench_responder` |
| Radio | 866.5 MHz, SF7, 125 kHz, CR 4/5, 20 dBm, stock antennas |
| Units | A = person in trouble (fixed, indoors at home), B = walker, C = on a building roof, E = responder |
| Power | All on their own batteries, power mode AUTO. **C was not set to ON** (forgotten), and the 3D-printed cases have no USB opening, so no power bank could be connected. No unit had the GPIO 3 voltage-sense wire |
| Recording | WhatsApp photos of the Status screen plus shared GPS positions. The field-logger web app was not used by volunteers. Duration about 2.5–3 h |

**Unit counters, read over USB after the test** (totals since power-on; the
per-station dBm/SNR had already been overwritten by bench readings, so the
field values exist only in the WhatsApp photos)

| Unit | Relayed | Skipped (neighbour relayed first) | Time asleep | Heartbeats heard from it by others | Heard B's pings | Heard A |
|---|---|---|---|---|---|---|
| A | 293 | 22 | 2% | 13 | ×304 | – |
| B | 26 | 37 | 7% | 1 | – | ×39 |
| C | 31 | 35 | 85% | 1 | ×308 | ×38 |
| E | 16 | 16 | 9% | 1 | ×296 | ×19 |

E's SOS list: one SOS from A, channel **General**, first received about 2 h 36
min before the read-out, received 12 times.

**Observations (owner)**
- For most of E's later messages, A (fixed at home) received nothing; B
  received many of them.

**Interpretation**
- **E's texts to A failed because of the §13.3 forwarding rule combined with
  false power detection.** A battery unit stops relaying text once it has
  heard a powered unit's heartbeat directly within 65 min (`fm_mesh.cpp`). A,
  without the voltage-sense wire, repeatedly guessed "external power" (13
  heartbeats) and acted as a powered unit (sleep 2%, 293 relays). B and C heard
  those heartbeats and stopped relaying text, leaving it to A, but A was the
  destination and indoors. E's texts reached A only while E was in direct
  range; B, usually near E, heard them directly. SOS and ACKs are always
  relayed, which is why A's SOS reached E. Recorded as an open question in
  `architecture.md` §13.9.
- **C did not work as a roof tower**: 85% asleep means it was never in powered
  mode. It still relayed 31 frames (radio wake), but texts were left to A.
- **The SOS went out as General**, not Medical: no channel key was pressed
  within 5 s. 12 copies over 2.6 h fit V4's re-send every ~15 min until
  "Help is coming", so E's hold 4 + 6 was probably never completed.
- **B's range ping ran the whole test and A's mostly did not** (others heard B
  ~300 times, A 19–39 times), so most signal readings in the photos are of B.
- Airtime stayed at 0.2% or less.

**Changes made after the test**
- Firmware V4 4.3.1: the power mode (AUTO / ON / OFF) is saved and survives
  power-offs. C is set to ON; A, B and E to OFF until the voltage-sense wire is
  fitted. All units run the field logger and know the `floodmesh` hotspot, so
  the next test records everything automatically.
- Hardware to do: solder the GPIO 3 voltage-sense wire; add a panel-mount
  USB-C extension to the case for charging, power banks and recovery.

**Still to add:** the field dBm/SNR values and positions from the WhatsApp
photos.

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
