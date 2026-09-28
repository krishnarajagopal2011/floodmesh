# FloodMesh network architecture: decisions and reasoning

Record of the architecture discussion held on 23 September 2026 between the
owner and Claude (acting as reviewing mesh architect). It records what was
decided, what was proposed and is still awaiting the owner's confirmation, what
was rejected and why, and the numbers behind each call.

Updated 28 September 2026: SOS retry limit and keep-alive (§7.6), the
responder reply key, reminder alarms and a shorter Medical escalation (§13.8),
power sensing on the Heltec builds (§13.2).
Updated 27 September 2026: voice removed from version 1 (§7.4); radio wake,
no relay role, forwarding and display rules, heartbeats and Bluetooth firmware
updates, a single network spreading factor and SOS channels decided (§13).

**Status legend**
- **DECIDED**: the owner stated or confirmed it.
- **PROPOSED**: recommended in discussion, not yet confirmed by the owner.
- **OPEN**: parked; needs a decision later.

The owner of the project is the repository owner (not building for a third
party). Hardware targets are described in `docs/hardware-notes.md` (Heltec V3
dev-board build) and `docs/hardware/pcb-v1/` (custom PCB, not yet fabricated).

---

## 1. Constraints everything is judged against

### 1.1 Regulatory (OPEN, needs verification)
India, 865–867 MHz licence-exempt band. The firmware assumes **G.S.R. 853(E)
(2021) Table II: 2.5% duty cycle (90 s of transmission per hour per device),
125 kHz bandwidth, 500 mW e.r.p.** This has **not** been checked against the
gazette text: sandboxed sessions cannot reach Indian government sites, and
third-party summaries disagree (one says 1%, the older G.S.R. 564(E) mentions no
duty cycle). Still unknown: whether the limit is per device or per channel, and
whether listen-before-talk relaxes it.

Action: put the gazette PDF in `docs/reference/` so it can be read directly.

**Legal risk in the current firmware:** `fm_airtime` exempts Layer 1 alarms
from the duty-cycle ledger as a "life-safety choice". The licence-exempt rules
have no emergency carve-out as far as is known. PROPOSED fix: replace the
exemption with a **reserved alarm budget** (e.g. 0.5% of the 2.5%, ~18 s/hour,
~250 alarms/hour/device) so alarms keep priority without breaking the rule.

### 1.2 Airtime reference (BW 125 kHz, CR 4/5, preamble 16)

| Packet | SF7 | SF9 | SF10 |
|---|---|---|---|
| 10-byte locate ping | 49 ms | 177 ms | 354 ms |
| 22-byte alarm | 65 ms | 239 ms | 436 ms |
| 32-byte distress | 80 ms | 280 ms | 518 ms |
| 60-char text (6-bit packed, ~69 B) | 136 ms | 443 ms | 805 ms |
| 10 s Codec2 voice clip (8 fragments) | **2.6 s** (measured) | ~8.3 s | ~15 s |

Sensitivity gain over SF7: ~5 dB at SF9, ~7.5 dB at SF10. Wet reinforced
concrete costs roughly 10–15 dB per wall.

**Consequence:** voice is tied to SF7 (the shortest range) and costs about 6×
the airtime of a 60-character text sent at SF9. Voice capacity, not mesh
flooding, is what saturates the channel.

### 1.3 Channel capacity
Listen-before-talk with hidden nodes keeps a channel useful up to ~30–40% load,
so **~1,200 s of useful airtime per hour** shared by everyone in range. One voice
clip flooded over 3 hops with 4–6 rebroadcasts uses 10–15 s of it, so roughly
80–120 voice clips per hour for a whole neighbourhood.

### 1.4 Design size (DECIDED)
**Colony of 100 households.**

---

## 2. Roles

**DECIDED:** all units use the **same hardware**; the role comes from firmware
state. Relay is not a role: a unit on external power stays awake and forwards
everything (§13.2).

| | Civilian (default) | Powered unit (any unit on external power, §13.2) | Responder |
|---|---|---|---|
| Setup | **None required.** Works out of the box. Optional registration (§6.3) | None; plug into solar, a power bank or a UPS (§4.3) | Bluetooth registration by the super admin |
| Sends | SOS with a channel, text (§7, §13.8) | Heartbeat every 30 min (§13.5), batched ACKs, zone summaries, forwarded traffic | Text/preset replies, bulletins, LOCATE |
| Sleep | Deep sleep + radio wake (§13.1) | Always listening | Always listening |
| Expiry | None | None | 10 days (§6.2) |

---

## 3. Location: no GPS

**DECIDED:** no GPS module. Reasons: GPS often gets no fix indoors or under
concrete slabs; a cold start draws 25–30 mA for 30 s to minutes; it can't tell
floor 1 from floor 4, and in a flood the floor matters more than the map pin.

Location comes from four layers, coarse to fine:

1. **Address registry (DECIDED).** Each unit's call sign maps to a household
   address and floor. The call sign is derived from the chip's eFuse MAC
   (`fm_ids`), so it never changes; print it on the unit's label with a QR
   code. The registry comes from the handout log or from optional registration
   (§6.3), and is loaded into responder units at their registration so
   responders see "B-12, 2nd floor", not a code.
2. **Relay zone cross-check (PROPOSED).** Relays sit at known positions. If a
   call sign registered in zone B is heard only by the zone F relay, the
   responder screen flags "⚠ location mismatch". This catches families who
   evacuated and took the unit with them.
3. **Hot/cold homing (DECIDED in principle, details PROPOSED).** The responder
   presses LOCATE on a distress. The civilian unit (woken by the LOCATE's
   wake-up preamble, §13.1) sends a 10-byte ping every 3 s for up to
   15 min: ~15 s of its hourly budget. Pings are **never relayed**. The
   responder beeps faster as the signal gets stronger and shows a bar plus
   "warmer/colder". Limits: it shows closeness, not direction (use the
   body-shielding turn, or an optional directional antenna); the signal jumps
   5–10 dB, so average several pings and show the trend; it saturates within
   ~20 m, so the civilian steps its transmit power down in locate mode.
4. **Sound the civilian's buzzer (PROPOSED).** The responder can make the
   civilian unit's buzzer sound, so rescuers hear it through a door, in the dark,
   or from a boat.

Registry risks recorded: devices carried away on evacuation (mitigated by 2 and
a "not at home" option); lent devices; wet or lost paper copies (keep a digital
copy on responder units); privacy (the registry reveals who is vulnerable and
which houses are empty); informal settlements without formal addresses (allow
landmark text).

---

## 4. Relays

**Update 27 Sep 2026:** relay is no longer a role or a mode. Any unit on
external power does this job (§13.2); "relay" below means such a powered unit.

### 4.1 Relays help but are never required (DECIDED)
Cell towers fail in floods because grid power fails, generators flood, backhaul
is cut, and all sites share those causes. The lesson: **nothing may depend on a
single point that shares a failure cause with everything else.** Every
life-safety function must work with **zero relays**:

| Function | With relays | Relays gone |
|---|---|---|
| Distress reaches responders | Relay collects, summarises, forwards | Every unit forwards SOS (§13.3) |
| Location | Registry + relay zone | Registry + homing |
| ACK | Relay ACKs within seconds | Responder ACKs end to end; volunteers cache and replay |
| Density map | Relay summarises by zone | Responder counts raw messages |

Relays have no backhaul: they reach responders over the same radio. They use the
same hardware as civilian units, so a dead relay is a cheap swap. Watch for
shared failure causes: same mounting height, same battery batch, a week of
monsoon cloud.

### 4.2 Relay count for 100 households
The binding limit is **each relay's own 90 s/hour**, not the shared channel.
Worst-hour budget (every household sends distress with ~2 retries; half would
use a voice clip if voice were kept): about 600 s of channel time, around 50%
load. Per relay, with 3 relays: beacons ~3 s + batched ACKs ~3 s + summaries
~5 s + ~17 voice clips ~45 s ≈ **56 of 90 s**. Voice is removed from version 1
(§7.4), which brings this down to ~11 s plus forwarded text.

- **At least 3 relays**, on different buildings, with **at least 2 relays
  hearing every house**.
- **Share the load:** each message is forwarded by the relay that heard it best;
  the others hear it and cancel (existing `fm_mesh` mechanism).
- **Batch ACKs:** one packet acknowledging up to ~10 call signs, instead of one
  ACK per household (~20 s → ~3 s per relay per hour).
- The actual range through wet concrete is unknown. **A two-unit rooftop/ground
  walk test decides placement.**
- First results (26 Sep 2026, relay at road level only): **350 m** direct and
  **550 m** through one relay (300 m + 250 m, line of sight). The raised-relay
  half of the test is still to do. Details: `docs/field-tests.md`.

### 4.3 Who hosts relays (DECIDED)
Priority: **households with solar or UPS power first**, then the highest floor
and best battery.

A relay listens all the time (~75 mA), so the 2000 mAh board battery lasts only
about a day. Relay hosts must feed USB-C from solar, a power bank or a UPS; the
board's charger works as a small UPS. A household UPS gets drained by lights and
phones during an outage, so a dedicated small panel plus power bank is more
dependable.

**DECIDED 27 Sep:** there is no relay mode. A unit on external power stays
awake and forwards everything (§13.2). Detecting external power needs the
charger's PG pin routed to a GPIO (PCB change 2, §9). Fallback without it: infer
charging from the fuel gauge's charge rate.

### 4.4 Forwarding without relays (DECIDED; partly superseded by §13.3)
- ~~Relays send a beacon every few minutes; a unit that misses about 3 switches
  to mesh mode.~~ **Superseded 27 Sep:** there is no mode switch. Every unit
  forwards SOS, and battery units forward text when no powered unit is heard
  nearby (§13.3, §13.5).
- **No election.** Units above a battery threshold **volunteer**: start at ~50%,
  stop at ~35% (the gap prevents flip-flopping). The MAX17048 fuel gauge on the
  PCB gives a real state of charge. Whether a battery floor still applies under
  §13.3 is OPEN.
- **Who repeats first is decided by a race, not coordination.** Each volunteer
  computes its own rebroadcast delay from its **own** battery, floor and the
  signal strength it heard, plus a random part. The first timer to expire
  transmits; the others hear it and cancel. Nobody needs to know anyone else's
  values. If the best unit is dead, the next-best transmits instead, with no
  handover.
- Units below the threshold send only their own distress and listen for the
  ACK. **Volunteers cache ACKs** and replay them when a sleeping sender retries.
- **Always listening, regardless of battery:** a unit that has heard no other
  volunteer for a while (lowers its own threshold), and a unit with its own
  unacknowledged distress.
- ~~Only distress-sized packets are relayed by civilian volunteers.~~
  **Superseded 27 Sep:** battery units also forward text when no powered unit
  is nearby (§13.3).
- Batteries only fall during a flood, so over days the volunteer pool shrinks.
  After that, responders physically passing by (boats collecting stored
  messages) carry the load.

---

## 5. Sleep and listening

**Superseded 27 Sep 2026 (§13.1):** radio wake replaces the shared listening
window. The window analysis below is kept for the record; wake on button still
applies.

- **Wake on button:** all PCB buttons are on RTC-capable GPIOs, so an SOS press
  wakes the unit from deep sleep.
- **Shared listening window (PROPOSED: 20 s every 5 minutes).** The owner
  suggested a long open window every hour or two. Recommended instead: short,
  frequent windows, which give a much shorter delay for less battery:

| Schedule | Awake share | Worst-case delay before neighbours see an SOS |
|---|---|---|
| 10 min every hour | 17% | ~50 min |
| 10 min every 2 hours | 8% | ~110 min |
| **20 s every 5 min** | **7%** | **~5 min** |

  Estimated battery (light sleep with radio receiving ≈ 7 mA, to be measured):
  20 s / 5 min ≈ 0.5 mA average ≈ 5–6 months on 2000 mAh.

  Requirements:
  - Everyone must wake at the **same** time. Relay beacons carry the time, and
    every packet heard in a window re-syncs the clock. The PCB has no 32 kHz
    crystal, so the internal RC oscillator drifts a few seconds per 5 minutes,
    which the 20 s window absorbs.
  - Queued sends go at a random moment within the window to avoid a
    collision burst when it opens.
  - This single window replaces the separate beacon check, the mesh-mode
    listening slots and the bulletin fetch.
- **The SOS itself never waits for a window.** It goes out immediately to
  relays and responders, which are always listening. The window is for
  neighbours, ACKs, replies and bulletins.
- **Bulletins** ("Evacuate zone B") are announced by a bulletin number in the
  relay beacon. A unit that sees a new number stays awake to fetch it.

---

## 6. Identity, keys and registration

### 6.1 Call signs
Derived from the eFuse MAC (`fm_ids`), stable for the life of the chip, printed
on the label. A call sign is a short **name**. It is not a key and is not
secret.

### 6.2 Responders (DECIDED)
- **One super admin**, using a **passphrase-protected app**, can turn **any**
  unit into a responder over **Bluetooth**. There is no predefined call-sign
  list; the admin decides at registration.
- At registration the unit **generates its own key pair inside itself**. The
  private key never leaves the unit. The admin app signs a statement:
  "call sign X is a responder until <date>, public key Y". Responder messages
  are signed with the unit's own key.
- **Every unit's firmware contains only the super-admin public key** (identical
  in all units, not secret). This lets civilian units check that a responder
  was approved by the admin. Changing it means reflashing every unit, so the
  **super-admin key must last for years and be backed up offline** (e.g. a
  printed QR code in a sealed envelope held by a second trusted person). It
  lives only in the admin app, in the phone's secure storage, unlocked by the
  passphrase.
- **Expiry: 10 days.** At expiry the unit **deletes its responder key** and
  becomes a civilian unit. Civilian units reject responder messages whose
  approval has expired, and treat the sender as a civilian.
- **Renewal: Bluetooth only in version 1** (PROPOSED, owner's direction). This
  also gives a physical check of who holds the unit (the PIN on its OLED). LoRa
  renewal was considered, but without roll call the admin would renew units
  without knowing who holds them now, so LoRa renewal, roll call and an
  IDENTIFY beep are **deferred to v2 as one package**. The unit warns from
  **day 8**. An expired unit still works fully as a civilian unit.
- **No roll call** in version 1 (DECIDED).
- Time source for expiry (PROPOSED): the phone sets the time at registration; the
  sleep timer keeps counting; the last known time is saved to flash every hour;
  relay beacons carry timestamps. The clock may only move forward. Worst case,
  a switched-off responder gets a few extra days.
- Hardening (PROPOSED): enable ESP32-S3 **flash encryption and secure boot** at
  production, so a stolen responder unit's key can't be read out.
- Rejected: a shared button code to unlock responder mode. A 6-press code on 4
  buttons has only 4,096 combinations, it spreads by word of mouth, and it
  can't be revoked. A 32-byte key can't be typed on buttons anyway.

### 6.3 Civilians (DECIDED: optional registration)
- **Unregistered units work fully as civilian units.** All units are civilian by
  default.
- **Optional registration** from residents' own phones, using the same app in a
  public mode:
  - **Flutter app** for Android and iPhone (Bluetooth via `flutter_blue_plus`).
    The web-app idea was dropped because iPhone Safari has no Web Bluetooth.
    iPhone needs the App Store or TestFlight (Apple developer account).
  - **OTP verification** of the phone number. If using your own SMS gateway,
    TRAI **DLT registration** of the sender ID and template is needed first,
    which takes days to weeks.
  - **The PIN on the unit's OLED** proves the person has the unit in hand. Do
    the PIN check inside the app's own protocol, not through system Bluetooth
    pairing (it behaves differently on iOS and Android).
  - **Helpers** can register neighbours without smartphones. With a basic phone,
    the OTP goes to the owner's number and they read it out. With no phone at
    all, the record is marked "assisted", with the helper as contact.
  - The **owner builds the database** (DECIDED). Needs: a consent screen,
    encryption at rest, role-based access, an offline queue in the app, and an
    export to responder units.
  - The owner is the **Data Fiduciary under India's DPDP Act 2023**: consent,
    stated purpose, security safeguards, deletion on request, breach reporting.
- **PROPOSED, awaiting confirmation:** registration gives each unit **its own
  16-byte alarm key**, generated in the unit, stored in the database and
  exported to responder units. Alarms keep the existing 8-byte `fm_auth` tag
  (no extra airtime). Responders then show "✔ verified" for registered units
  and "unverified" for others. Cost: the database and responder units hold
  these keys and must be protected.
- **OPEN:** unregistered units: keep a built-in shared alarm key (proves "genuine
  FloodMesh packet", not identity) or drop it. The current firmware drops
  alarms that fail authentication (`fm_mesh.h`), which must be reconciled with
  unregistered units.
- Provisioning mode is entered with a button combination at power-on. The OLED
  shows a PIN, and Bluetooth turns off again afterwards. A factory-reset
  combination (hold ~10 s) wipes identity and keys.
- **Bluetooth antenna:** the WROOM-1U module has no built-in antenna, only a
  U.FL connector. **PROPOSED (owner, 27 Sep):** an adhesive flex ("sticker")
  2.4 GHz antenna with a U.FL/MHF1 lead, stuck inside the case (§9 #7). The
  WROOM-1 (built-in PCB antenna, same pinout) is not a drop-in: it is about
  6 mm longer at the antenna end.
- Firmware updates (**DECIDED 27 Sep**, §13.6): over Bluetooth from the app,
  with signed images and automatic rollback. The PCB has no USB data lines.

---

## 7. What users can send

### 7.1 Presets (DECIDED 27 Sep: removed for civilians)
~~One-press distress presets (the existing four: SAFE, NEED MEDICAL, WATER
GROUND FLOOR, NEED EVACUATION), carrying head count and flags where possible.~~
**Removed 27 Sep 2026:** civilians have the SOS with a channel (§13.8) and
60-character text, which cover the same needs. Responder units keep preset
replies (§7.5).

### 7.2 SOS key (DECIDED)
A dedicated SOS on the **side button** (S1 on the PCB). On the V3 keypad build
(no side button): **hold `*` and `#` together for 3 s** (DECIDED 27 Sep). The
sender picks an SOS channel (§13.8). PROPOSED behaviour:
hold 3 s, with a countdown and cancel; highest priority of all traffic;
addressed to responders; other civilian units relay it silently. It still hops
through the mesh; "direct" changes who **sees** it, not the path it takes.

**DECIDED 27 Sep (§13.4):** neighbours don't see it. Civilian units forward an
SOS silently; only responder units show it and sound the buzzer. Texts, not
SOS, are how neighbours are asked for help.

### 7.3 Text via a 4×4 keypad (DECIDED)
Replaces voice as the information-rich channel. It is the only free-form
channel in version 1, which has no voice (§7.4).
- **Maximum 60 characters**, packed 6 bits per character (capital letters,
  digits, space, common punctuation): ~45 bytes, **~0.45 s at SF9**, versus
  2.6 s for voice at SF7. More range, ~6× less airtime, ~180 messages/hour
  within the legal budget.
- **English letters only. No Tamil script or fonts** (DECIDED). Tanglish is
  typed letter by letter.
- **Input modes, switched with `#`:** **T9 predictive (English)** with `*` to
  cycle matches; **ABC multi-tap** for Tanglish, names and anything the
  dictionary lacks; **123** for numbers.
- Dictionary: ~10–15k English words by frequency (~100–200 KB of flash), with
  flood and medical words ranked higher (insulin, dialysis, oxygen, pregnant,
  elderly, wheelchair, roof, floor, boat, water, trapped, injured, baby). No
  learning in version 1.
- The OLED fits 4 lines × 21 characters, so 60 characters shows on one screen.
- Concerns recorded: excludes people who can't read or type Latin letters;
  typing 60 characters under stress takes minutes, which is why the SOS with a
  channel is the fast path (§13.8).
- Hardware: **4×4 keypad (DECIDED 27 Sep):** 1–9, 0, `*`, `#`, and A–D as
  navigation keys (UP, DOWN, OK, BACK), as on prototype v2 and V3. PROPOSED for
  the PCB: via an **MCP23017 I²C expander** (TCA8418 is
  hard to source quickly in India), with its interrupt on an RTC-capable pin,
  and a **sealed silicone keypad** for IP67. The side button stays as SOS. (A direct 4×4 matrix
  needs 8 GPIOs. Removing voice frees 7 RTC-capable GPIOs on the PCB, so a
  direct matrix needs one more pin, e.g. IO47 as a column; see §9 #8.)

### 7.4 Voice (DECIDED: removed from version 1)
**Decided 27 September 2026:** version 1 has **no voice for anyone**, civilian or
responder. All free-form messages are text from the keypad (§7.3).

History: the owner first set civilians to 1 × 10 s clip per hour and responders
to free voice, then proposed text instead of voice "due to legal limits". The
two options left open were voice for responders only, or no voice; the owner
chose no voice.

Consequences:
- Hardware: remove the INMP441 mic, MAX98357A amp and speaker (§9 #10); keep
  the buzzer. On the PCB this frees IO4–IO6 and IO15–IO18.
- Firmware: remove Codec2 Layer 2 voice, PTT and the voice ring buffer (§10).
- Radio: the current firmware is fixed at SF7 only because voice needed the
  airtime (`FM_LORA_SF` in `include/fm_radio.h`). That reason is gone, so the
  spreading factor needs choosing again (§12.3).

If voice returns in a later version: rate limit 1 per hour; a
**READY / BUSY / OFF** channel-load indicator shown before recording (from
`main`'s `docs/ARCHITECTURE.md`); relayed by relays only, never by civilian
volunteers; **off in mesh mode**.

### 7.5 Responder → civilian messages (PROPOSED)
**DECIDED 27 Sep:** "Help coming" is a preset reply on responder units (§13.8);
28 Sep: sent by holding 4 + 6, shown as "HELP IS COMING".
The rest of this section is PROPOSED.

No free keyboard needed on the civilian side. Responders send **preset messages
with a parameter** chosen from OLED menus ("Help coming", "Stay where you are",
"Boat arriving in 15/30/60 min", "Go to relief camp ▸ [list]", "Evacuate now").
On air it's 2 bytes plus the signature. They are sent as a **reply** to a
specific distress or a **broadcast** to a zone. Replies and broadcasts carry
the wake-up preamble, so they reach sleeping units directly (§13.1); powered
units and volunteers cache replies. With the 12-key keypad, responders
can also send free text.

### 7.6 Acknowledgements
- **First hop:** the "honest relay-ACK" from `main`'s `docs/ARCHITECTURE.md`.
  The sender listens for a neighbour rebroadcasting its packet, at no extra
  airtime cost ("passed on by neighbour").
- **End to end:** an ACK from a relay or responder ("received by responders").
- Unacknowledged distress is retried with growing gaps. **DECIDED 28 Sep:** at
  most **5 retries**, after 1, 2, 4, 8 and 15 min. If the 5th is not answered
  (within a minute), the unit beeps twice and shows **"SOS not delivered"**;
  an answer arriving later still counts.
- **DECIDED 28 Sep: keep-alive after delivery.** A delivery ACK ends the
  retries but not the SOS: the unit re-sends it every 15 min until a
  responder sends "Help is coming", or the user stops it. Otherwise an SOS
  heard by a single responder unit is lost if that unit is switched off,
  flooded or carried away before anyone acts. A responder that already has it
  just updates its entry (no new alarm); one that arrives later gets it fresh.
  Cost: one SOS frame (~65 ms at SF7) plus its ACK per open SOS every 15 min,
  relayed through the area.

---

## 8. Rejected or deferred, and why

| Idea | Outcome | Reason |
|---|---|---|
| Approach 1: everyone relays voice | Rejected for civilians | Channel collapses at ~80–120 clips/hour per area; ~3-day battery |
| Approach 1 var. 2: call-sign-gated playback | Rejected as security | A firmware `if`, not encryption; addressing saves no airtime in a flood mesh |
| Approach 2: GPS in every unit | Rejected | Indoor fix failures, power draw, no floor information |
| Relays as the only path | Rejected | Recreates the tower single point of failure |
| Electing the highest-battery unit as relay | Rejected | Needs coordination airtime, ignores position, single point of failure; replaced by threshold volunteering + weighted delay |
| Shared button code for responder mode | Rejected | Leaks, can't be revoked |
| LoRa renewal + roll call | Deferred to v2 | Unsafe without roll call's physical check |
| Long listening window every 1–2 h | Replaced | 20 s / 5 min gives ~10× lower delay for less battery |
| Tamil script on the OLED | Rejected | Tanglish in Latin letters instead |
| Web Bluetooth app | Replaced by Flutter | No iPhone support |
| Alarm exemption from duty cycle | To be replaced | Legal risk; use a reserved budget |
| Voice in version 1 (civilian and responder) | Removed | ~6× the airtime of a text and tied to SF7's short range; text covers the need (§7.4) |
| Shared listening window (20 s / 5 min, or 10 min / hour) | Replaced | Radio wake: lower current, sub-second delay, no clock sync (§13.1) |
| Relay as a role or mode; self-election; beacon-driven mesh mode | Replaced | Any unit on external power stays awake and forwards everything (§13.2, §13.3) |
| Relay beacon every few minutes | Replaced | Heartbeat to responders every 30 min (§13.5) |
| SOS shown to neighbours | Rejected | Responders only; neighbours see texts (§13.4) |
| Firmware updates over a phone hotspot | Replaced | Bluetooth from the app, signed images, rollback (§13.6) |
| Dropping to a lower SF when airtime runs low | Rejected | Fewer units hear it, so more retries and more total airtime (§13.7) |
| Per-link adaptive data rate (LoRaWAN style) | Rejected | Needs fixed point-to-point links; a broadcast mesh has none (§13.7) |

---

## 9. PCB V1.0 change list (board not yet fabricated)

See `docs/hardware/pcb-v1/README.md` for the board description.

| # | Change | Why | Status |
|---|---|---|---|
| 1 | NTC thermistor on the MCP73833 `THERM` pin (3-wire LiPo pack) | No battery temperature protection while charging in a hot sealed enclosure | PROPOSED, high priority |
| 2 | Charger `PG` pin → spare GPIO (e.g. IO47) | Firmware must know about external power: powered units stay awake and forward everything (§13.2) | PROPOSED (power detection is required) |
| 3 | `LORA_DIO1` IO38 → **IO19** (RTC-capable) | Radio can wake the ESP32 from deep sleep | **DECIDED: required** for radio wake (§13.1). No USB data, so IO19 is free |
| 4 | Reverse-polarity protection on J3 | JST-PH LiPo packs are wired both ways by different vendors | PROPOSED |
| 5 | MAX17048 on the `BATT+` side of the power switch | Keeps its learned battery model across power cycles; with the switch off-board (#12) it would also see the switch lead's resistance | PROPOSED |
| 6 | USB data | Not needed: provisioning is over Bluetooth | DECIDED (charge-only stays) |
| 7 | 2.4 GHz antenna for WROOM-1U: adhesive flex ("sticker") antenna on a U.FL/MHF1 lead, stuck inside the case (Molex 146153-0100 in the priced BoM). WROOM-1 is not a drop-in: about 6 mm longer at the antenna end | Bluetooth provisioning and firmware updates need an antenna; the sticker needs no PCB change | PROPOSED (owner, 27-09-2026) |
| 8 | **4×4 keypad** via I²C expander **MCP23017** + sealed silicone pad, interrupt on an RTC pin; or a direct matrix on the 7 GPIOs freed by #10 plus one more (e.g. IO47 as a column) | Text input and navigation (§7.3). MCP23017 is easy to source in India and is what prototype v2 uses; V3 wires the matrix directly | 4×4 DECIDED; MCP23017 vs direct matrix PROPOSED |
| 9 | Optional 32.768 kHz crystal on IO15/IO16 (free once the mic is removed, #10) | Accurate sleep timing, narrower windows. Competes with a direct keypad matrix (#8) for those pins | Not needed (no shared window, §13.1) |
| 10 | Remove the mic, amp and speaker | Voice is removed from version 1 (§7.4). Frees IO4–IO6 and IO15–IO18 | DECIDED |
| 11 | Power the INMP441 mic from a **switched rail**, not always-on 3V3 | The mic draws ~1.4 mA whenever powered, which would dominate deep-sleep current | Not needed (mic removed, #10) |
| 12 | Remove slide switch S6; a 2-pin header in the `BATT+` → `BATT_IN` break goes to a sealed (IP67) panel rocker on the case | Owner's case design. Charging still works with the switch off, and the MCU never sensed S6 | DECIDED (owner, 27-09-2026) |
| 13 | Switch header in a different connector series from the battery's JST-PH: JST GH (latching) or ZH | With two PH 2-pin headers the battery and switch plugs can be swapped; with the rocker ON that drives `BATT_IN` below GND through IC2/IC3. XH or 3-pin PH headers still accept a PH plug | PROPOSED |
| 14 | C21 → at least 4.7 µF, plus bulk capacitance on `BATT_IN` at the switch header | TI's typical input capacitor; the off-board lead adds inductance, so switch-on ringing must stay under the buck's 6 V VIN maximum (check on a scope) | PROPOSED |
| 15 | Re-pick parts that are only sold by US distributors: TPS6282533DMQR (e.g. adjustable TPS62825DMQR + FB divider, on LCSC), USB4730 (keep an **IP67-sealed** USB-C that LCSC stocks), TS11-674-135 and TS04-66-95 tactile switches (check actuator heights against the case), MCP73833T-FCI/UN option | Owner sources from Indian retailers and China, not US distributors (priced BoM, 27-09-2026) | PROPOSED |

Check GPIO budget once items 2, 3 and 8 are settled together (9 is no longer
needed). Removing voice
frees IO4–IO6 and IO15–IO18, all RTC-capable.

---

## 10. Firmware changes implied (current code on `9th-Sept-2026`)

| Area | Current | Needed |
|---|---|---|
| `fm_airtime` | Alarms exempt from duty cycle | Reserved alarm budget (§1.1) |
| `FM_REPLAY_SLOTS` (`include/fm_auth.h`) | 32 | **≥128** for 100 senders (~2 KB) |
| `FM_DEDUP_SLOTS` (`include/fm_dedup.h`) | 32 | **~256** to survive a burst of 100 distress + retries (~3 KB) |
| Pin map | Heltec V3 only (`include/floodmesh_pins.h`) | Separate board variant for PCB V1 |
| Input | 2×2 matrix + PTT | 4×4 keypad (MCP23017 or direct matrix, §9 #8) + side SOS button; no PTT |
| Mesh | SNR-biased delay | Add power + battery + floor weighting; forwarding rules (§13.3); ACK caching; batched ACKs; 30-min heartbeat (§13.5) |
| Sleep | Continuous receive | Deep sleep + SX1262 receive duty cycle, wake on `DIO1`; wake-up preamble on packets for sleeping units (§13.1) |
| Auth | One PSK (public placeholder) | Per-unit alarm keys for registered units; Ed25519 responder signatures; super-admin public key built in |
| Voice | Codec2 Layer 2, PTT, voice ring buffer | Remove (§7.4) |
| Spreading factor | Fixed SF7 (`FM_LORA_SF`), chosen for voice | One network SF chosen by field tests; retune the `fm_mesh` contention slot. v1.1: SF step-up retries for SOS, dual-SF scan on powered units (§13.7) |
| Display | – | SOS shown and sounded only on responder units; text shown on every unit (§13.4) |
| SOS channels | V3: SOS has no category; alerts menu (A) sends 4 presets | Channel screen after the SOS hold; remove the civilian alerts menu; responder channel filter in NVS; tab bar with dot/highlight for unopened SOS in unselected channels; "Help coming" preset reply; escalation (§13.8) |
| Firmware update | Root: USB only. V3: WiFi update mode (bench) | Bluetooth from the app, signed images, rollback (§13.6) |

---

## 11. Open questions (summary)

1. Gazette text: duty cycle, per device or per channel, listen-before-talk.
2. ~~Shared window.~~ **Decided 27 Sep:** radio wake, no window (§13.1).
3. Homing details: LOCATE pings + sounding the civilian's buzzer.
4. Keypad: **4×4 decided 27 Sep.** Still open for the PCB: MCP23017 + silicone
   pad, or a direct matrix on the GPIOs freed by removing voice plus one more.
5. ~~Voice vs 12-key text.~~ **Decided 27 September 2026:** text only, no voice
   in version 1 (§7.4).
6. ~~SOS visibility.~~ **Decided 27 Sep:** responders only (§13.4).
7. Per-unit alarm keys from optional registration.
8. Built-in shared alarm key for unregistered units: keep or drop?
9. ~~Relay mode.~~ **Decided 27 Sep:** no relay role; a unit on external power
   stays awake and forwards everything (§13.2).
10. Bluetooth antenna: sticker FPC antenna on the WROOM-1U proposed (§9 #7).
11. ~~Retry limit for unacknowledged distress.~~ **Decided 28 Sep:** 5 retries,
    then "SOS not delivered"; re-sent every 15 min after delivery (§7.6).
12. Repository: `main` holds an unrelated older history (`docs/ARCHITECTURE.md`,
    one `.ino`). Decide which branch is canonical; `main`'s design principles
    and honest relay-ACK are worth folding in.
13. Naming in docs: the PCB title block says "Vazworks". Confirm whether
    that is the owner's organisation.
14. Frame format, message IDs, ACKs, SOS categories, the Verified Responder
    label and the owner's 26 Sep proposal: see §12.5.
15. Details left open by the 27 Sep decisions: see §13.9.

Measurements needed from the bench: deep-sleep current at the battery (OLED
off); light-sleep + radio-receive current; buzzer loudness at 3.3 V; a
rooftop-to-ground range walk test.

---

## 12. Owner's proposal and frame-format discussion (26 Sep 2026)

Recorded from a session on 26 September 2026 so the discussion can continue on
any device. Items the owner confirmed are **DECIDED**; the rest of the owner's
proposal is **under discussion** and does not yet replace earlier DECIDED items.
Airtime figures are for BW 125 kHz, CR 4/5, 16-symbol preamble.

### 12.1 Owner's proposal (under discussion)

The owner proposed the following. Claude's counterpoints are noted; nothing
here is settled unless marked DECIDED in §12.2.

| Owner proposed | Conflicts with | Claude's counterpoint (numbers are estimates to measure) |
|---|---|---|
| Civilian registration **required** via the app (Bluetooth, OTP), with a secret key per unit stored in the database | §6.3 DECIDED: registration optional | Units get handed out, lent and grabbed in a hurry; an unregistered unit must still send SOS. Keep optional unless there is a new reason |
| **6-digit numeric call sign** assigned by the database | §3 / §6.1: MAC-derived | Accepted (see §12.2). Unregistered units still need a number; MAC-derived numbers in a 10⁶ space collide ~0.5% of the time at 100 units, ~39% at 1,000 |
| Organisations get an **access code** to register responders; expiry 10–15 days | §6.2 DECIDED: one super admin; shared codes rejected | Codes leak. Option: the super admin signs per-organisation keys. But a second certificate pushes a signed responder frame past 255 B (§12.4), which favours the single super admin |
| Units awake **10 min every hour** (e.g. 10:00–10:10), and an **SOS wakes all nearby units** by radio interrupt | §5 PROPOSED 20 s / 5 min; §8 replaced long windows | As written: ~8 mA awake share + ~4 mA radio listening ≈ **12 mA, ~7 days on 2000 mAh**. Counter-proposal: processor always asleep, SX1262 receive duty-cycle (well under 1 mA, to measure); **only SOS and LOCATE carry a ~1 s wake-up preamble** (SOS 65 ms → ~1.07 s at SF7, at most ~80 per hour per unit); normal traffic waits for the window. Clock drift needs beacon time sync (no 32 kHz crystal) |
| A civilian unit **self-elects as relay** after 5 missed beacons if battery > 50%, and returns below 50% | §4.4 DECIDED: no election; volunteer 50% on / 35% off | Full relay mode draws ~50–75 mA, so 50% of 2000 mAh lasts ~13–20 h. One threshold for on and off flip-flops as voltage sags during transmit. 5 beacons at 3 per window is up to 2 h to notice. Relay beacons must be signed, or a fake beacon suppresses mesh mode |
| The relay **switches to user mode** during key presses | – | Not needed: V3 already relays while someone types |
| Relays assign **queue numbers** to SOS | – | Two relays number the same SOS differently, and relay-changed fields sit outside the auth tag and can be forged. Superseded by §12.2 |
| **SOS hidden** from nearby civilian units but relayed by them | Settles §7.2 OPEN | Agreed as display behaviour. Counter-argument: neighbours are often the fastest rescuers |
| **Fox hunt**: a responder selects a unit; that unit's buzzer sounds when a responder is **within 100 m** | §3 homing DECIDED in principle, buzzer PROPOSED | Signal strength cannot judge 100 m (open air ≈ −51 dBm, behind two wet walls ≈ −80). Use an **explicit signed BUZZ command** plus hot/cold RSSI on the responder. The Heltec antenna gives no direction: body-shielding turn, or an optional directional antenna on U.FL |
| Civilian **broadcasts**, rate-limited; relays queue and replay them | – | Agreed. Capacity: ~200 s usable airtime per 10-min window per area; a 3-hop flooded text ≈ 0.5–0.8 s, so **~2–3 broadcasts per household per hour at SF7, ~1 at SF9** |

Queue memory (owner asked): the ESP32-S3 has ~320 KB of RAM (V3 uses
40–63 KB), room for ~3,000 queued messages at ~80 B, and ~18,000 in the 1.5 MB
flash partition. Deep sleep wipes RAM, so sleeping units keep queues in RTC
memory (8 KB, ~100 messages) or flash. The real limit is airtime: under 2.5% a
relay sends at most ~1,380 alarms or ~660 texts per hour, so queues need a drop
policy (for example, keep the newest SOS per call sign).

### 12.2 Frame format

**DECIDED (owner, 26 Sep):**
- Fields are sent as **bits**, not characters. 108 B as characters vs 67 B as
  bits: 193 vs 131 ms at SF7, 628 vs 443 ms at SF9.
- **No queue number.** The responder sorts SOS by its own receive order.
- Add **battery %**, **retry number**, and an **SOS category** with a
  responder-side **category filter**.
- **6-digit numeric call signs** (3 bytes on air).
- **Verified Responder broadcasts** as a feature; the mechanism is OPEN (§12.4).

**PROPOSED layout (67 B, the same airtime as today's V3 text frame):**

| Field | Bits | Notes |
|---|---|---|
| Version + type | 8 | text, SOS, alarm, ACK, beacon, locate, buzz, bulletin |
| Flags | 8 | hide from civilians, ACK requested, retry 0–7, responder-signed |
| Hops | 8 | limit + taken; relays change it, so it is outside the auth tag |
| Source | 24 | 6-digit call sign |
| Destination | 24 | reserved values: all units, all responders, all relays |
| Message ID | 16 | per-sender counter (OPEN below) |
| Category + head count | 8 | 4 + 4 bits |
| Battery % | 8 | |
| Age in minutes | 8 | relays add their holding time; outside the auth tag |
| Text | ≤ 360 | 60 characters × 6 bits |
| Auth tag | 64 | truncated HMAC (civilian frames) |

An SOS without text is ~23 B (~70 ms at SF7). Each extra byte costs ~1.5 ms at
SF7 and ~4.5 ms at SF9. Signal strength is **not** sent for the relay race:
each receiver measures it itself (§4.4). PROPOSED: the ACK carries the RSSI/SNR
it heard (1 B), for adaptive power control.

**OPEN: message ID.** The owner asked for a random 5–10 bit ID. Claude
recommends a **16-bit per-sender counter**. Random 10-bit IDs reach a 50%
collision after ~38 messages (network-wide, or from one sender within the dedup
window), and a collision silently drops a real message as a duplicate. The
counter repeats only after 65,536 messages from one unit (over 47 h at the
airtime cap). Duplicate detection needs no reference state either way; **replay
protection** does (the last counter per sender, ~1 KB for 128 senders, already
in `fm_auth`), and random IDs cannot provide it. Dedup entries need an expiry,
so a later retry passes, and must live in RTC memory to survive deep sleep.

**PROPOSED: ACK as its own frame** (~21 B, ~65–70 ms at SF7), only for SOS and
unit-addressed messages, never for broadcasts (100 ACKs would jam the channel).
Relays batch several ACKs (~5 B each, §4.2). ACKs drive the NOT SENT /
NOT CONFIRMED / DELIVERED states. Retries keep the same message ID with the
retry count raised.

**SOS categories: DECIDED 27 Sep as channels** General, Medical, Evacuation,
Hazard, Food supply (§13.8). Original proposal (at most ~6, so a stressed user
can choose):
General SOS, Medical, Trapped / water rising, Evacuation, Food and water,
Hazard. Safeguards: relays never filter; responders see a count of SOS in other
categories; an uncategorised SOS shows to everyone; responder units default to
"All".

### 12.3 SF choice (DECIDED 27 Sep: one network SF chosen by field tests, §13.7)

| | SF7 | SF8 | SF9 |
|---|---|---|---|
| Sensitivity vs SF7 | – | ~+2.5 dB | ~+5 dB |
| Text 67 B | 131 ms | 242 ms | 443 ms |
| SOS 23 B | 70 ms | 130 ms | 239 ms |
| Texts per hour in 90 s | ~690 | ~370 | ~200 |
| SNR floor | −7.5 dB | −10 dB | −12.5 dB |

V3 runs SF7. The 14 Sep decision stands: make SF selectable and let field data
choose. With voice removed from version 1 (§7.4), SF7 is no longer
needed for airtime. The `fm_mesh` contention slot is tuned for SF7 and needs retuning for
SF8 and SF9. Field data so far (`docs/field-tests.md`) says relay **height** is
the bigger lever.

### 12.4 Verified Responder label (mechanism OPEN)

The owner's view: a few bits in the frame mark a message as coming from a
responder. Claude's view: the bits are the label, but the proof must be a
signature. The firmware source is public and every civilian unit holds the
network key, so anyone can set the bits. A shared-key tag cannot work either,
because every civilian unit would hold the key and could forge it.

PROPOSED: responder frames carry a P-256 signature and the super-admin
certificate (`FMCERT1|callsign|responder|expiry|key`). The code already exists
(`fmRoleSign`, `fmP256Verify`, provisioning in `fm_prov.cpp`); only on-air use
is missing. Civilian frames are unchanged. A civilian unit shows the label only
when the bits are set **and** the signature verifies; bits with a bad or
missing signature show "claims to be responder, NOT verified".

| Responder broadcast | Size | SF7 | SF9 |
|---|---|---|---|
| Text + signature + certificate | 228 B | 367 ms | 1.16 s |
| Text + signature (certificate cached) | 123 B | 213 ms | 689 ms |
| Plus an organisation certificate | 333 B | over the 255 B LoRa limit | |

Also needed: replay protection (counter and time inside the signed part);
expiry needs the §6.2 time source; the label is drawn outside the text area
(civilians can type "VERIFIED"); a PIN to send verified broadcasts, against
stolen units; urgent broadcasts use the wake-up preamble. Suggested key
combination: hold A + D for 2 s on a responder unit (not `*` + `#`, and not a
power-on combination). Middle path offered: bits-only, marked "TEST, not
verified", for bench tests, with the signature added before any unit reaches
users.

### 12.5 Decisions still needed

1. Message ID: 16-bit counter (recommended) or a random ID.
2. ACK frame rules as in §12.2.
3. ~~The SOS category list.~~ **Decided 27 Sep** (§13.8).
4. Verified label: signature from the start, or bits-only for bench tests first.
5. Certificate in every responder broadcast; PIN to send; A + D combination;
   urgent broadcasts wake sleeping units.
6. ~~Sleep windows.~~ **Decided 27 Sep:** radio wake for all traffic that
   sleeping units must hear, no window (§13.1).
7. Registration required or optional; responder registration by organisations.
8. ~~Relay fallback.~~ **Decided 27 Sep:** no relay role; forwarding rules in
   §13.2 and §13.3.
9. Fox hunt: automatic 100 m buzzer vs an explicit BUZZ command.

---

## 13. Decisions of 27 September 2026

Confirmed by the owner on 27 September 2026. Where these change earlier
sections, those sections point here. Airtime figures are for BW 125 kHz, CR 4/5;
current and airtime estimates are still to be measured.

### 13.1 Radio wake replaces the shared listening window (DECIDED)
- **No shared listening window** of any length. A sleeping unit keeps the ESP32
  in deep sleep while the SX1262 runs its own receive duty cycle: it listens for
  a few milliseconds every T and wakes the ESP32 through `DIO1` only when a
  complete, valid FloodMesh packet arrives. The unit shows, forwards or ACKs it,
  then sleeps again after a quiet period.
- Every packet that sleeping units must hear carries a **wake-up preamble at
  least T long**, so it can't fall between two checks. This extends the §12.1
  counter-proposal (SOS and LOCATE only) to all such traffic, because no window
  is left for normal traffic.
- PROPOSED: T ≈ 0.5 s. Estimated radio average ~0.1–0.15 mA, against ~0.5 mA
  for 20 s every 5 min, and under a second per hop instead of up to ~5 min. The
  cost is ~0.5 s of airtime on every such packet: an SOS goes from 70 ms to
  ~0.55 s at SF7, or from 239 ms to ~0.66 s at SF9.
- Only a valid packet with FloodMesh's own sync word wakes the processor. Other
  LoRa equipment in 865–867 MHz (Meshtastic's India band is the same) must not.
- No clock sync is needed for listening, so the 32 kHz crystal (§9 #9) isn't
  needed.
- **PCB change 3 (`LORA_DIO1` IO38 → IO19) is required.** The Heltec V3 already
  has `DIO1` on GPIO14, which can wake the chip, so the prototypes can test this
  now.
- Supersedes the §5 window, the 10 min per hour in §12.1 and §12.5 item 6.

### 13.2 No relay role: powered units (DECIDED)
- Relay is no longer a role, a mode or a menu. **Any unit on external power**
  (solar, a power bank or a UPS on USB-C) **stays awake and forwards
  everything.** On battery the same unit sleeps (§13.1) and forwards by §13.3.
- The hosting advice in §4.3 stands: solar or UPS households first, high up.
  Height is the biggest range lever (`docs/field-tests.md`).
- The unit must detect external power: PCB change 2 (charger `PG` → GPIO), or
  infer charging from the fuel gauge.
- **DECIDED 28 Sep, Heltec builds:** a 100 k / 100 k divider from the Heltec 5V
  pin (USB VBUS) to GPIO 3, as on prototype v2, measures external power
  exactly. Units without it fall back to the battery-voltage trend: a rise
  means charging, a fall means battery, flat changes nothing. "Constant
  voltage = external power" was considered and rejected: a sleeping unit on
  battery loses only ~1–2 mV per hour, so it also looks constant, and would
  wrongly stay awake and drain.
- The jobs §4 gave relays (batched ACKs, zone summaries, caching for sleeping
  units, the §3 zone cross-check) become jobs of powered units.
- PROPOSED: external power gets the strongest weight in the §4.4 race, so
  powered units forward first and battery units hear them and cancel.
- Supersedes the relay mode in §4.3, the beacon-driven mesh mode in §4.4, and
  the self-election proposal in §12.1 and §12.5 item 8.

### 13.3 Forwarding rules (DECIDED)
- **SOS: every unit forwards it**, on external power or on battery, whether or
  not a powered unit is nearby, hop by hop until it reaches a responder (within
  the frame's hop limit).
- **Text: powered units always forward it. A battery unit forwards text only
  when it has not heard a powered unit's heartbeat nearby** (§13.5).
- Forwarding keeps the §4.4 race: each unit that would forward waits its own
  weighted delay and cancels if it hears someone else forward the packet first.
  An area sends each packet about once per hop, not once per unit.
- PROPOSED: "nearby" means a heartbeat heard directly (hops taken = 0) within
  the last ~65 min, i.e. two heartbeat periods.
- Supersedes the §4.4 mode switch and "only distress-sized packets are relayed
  by civilian volunteers".

### 13.4 Who sees what (DECIDED)
- **SOS: only responder units show it** (the call sign resolved to its registry
  address, plus category and battery) **and sound the buzzer.** Civilian units
  forward it silently, with nothing on the display and no buzzer. This settles
  §7.2, §11 item 6 and the §12.1 "SOS hidden" proposal.
- **Text is shown on every unit that receives it**, so anyone nearby can help
  instead of waiting for responders. A civilian who wants neighbours' help sends
  a text; the SOS is for responders.
- Text is public to every unit in range. User guidance should say so.

### 13.5 Heartbeat from powered units to responders (DECIDED)
- Every powered unit sends a **heartbeat every 30 min**, addressed to all
  responders. Any unit forwards it, hop by hop, until it reaches responders.
- Responder units list which powered units were heard and when, mapped to their
  registered addresses, so responders see which areas still have coverage.
- Battery units use the same heartbeats for the text rule in §13.3.
- Replaces the relay beacon every few minutes (§4.4). Bulletins are no longer
  announced in beacons; they go out as broadcasts with the wake-up preamble.
- Airtime estimate: a ~20 B heartbeat with the wake-up preamble is ~0.55 s per
  transmission at SF7. 10 powered units × 2 per hour × ~5 forwards ≈ 55 s per
  hour of channel time, ~5% of the ~1,200 s usable (§1.3). It grows with the
  number of powered units and hops.
- PROPOSED: heartbeats have the lowest priority and are dropped first when a
  unit's airtime budget runs low; they carry a hop limit and are de-duplicated
  like other frames.

### 13.6 Firmware updates over Bluetooth (DECIDED)
- Firmware updates go **over Bluetooth from the Flutter app**, with **signed
  images** and **automatic rollback**. The app downloads the image over mobile
  data and pushes it to the unit; nobody renames a hotspot.
- The unit writes the image to its spare app slot (both firmware builds already
  use a two-slot partition table) and marks it good only after a self-test;
  otherwise it boots the previous image.
- PROPOSED: updates only in provisioning mode (button combination + PIN on the
  OLED); a release-signing key separate from the super-admin key, both backed up
  offline; a battery check before starting; the app checks for new versions and
  notifies users; volunteer update drives for units whose owners don't have the
  app. Estimate: ~1.1 MB in roughly 0.5–1.5 min (slower on iPhone).
- Units that never update must keep working, so the on-air frame format must
  stay backward compatible (the version field, §12.2), and the network's SF must
  never change through an update.
- The V3 WiFi update mode (`firmware_v3/README.md` §7) stays a bench tool.
  Supersedes the hotspot updates in §6.3.

### 13.7 Spreading factor (DECIDED)
- **Version 1: one SF for the whole network**, chosen by field tests (this keeps
  the 14 Sep decision in §12.3). Every unit hears every packet, and radio wake
  (§13.1) works at a single SF. Without voice, SF9 is affordable (§12.3 table).
- **Version 1.1: step up the SF when an SOS gets no ACK.** The SOS goes out at
  the network SF. After N unanswered retries it retries at a higher SF, for
  example SF11 (~+5 dB over SF9; a 23 B SOS is ~0.95 s before any wake-up
  preamble). Units on external power and responder units check both SFs in
  turn, which is cheap when they aren't on battery. Battery units stay on the
  network SF and don't hear these retries. This targets someone indoors whose
  normal signal can't get out, and only the unit in trouble pays the extra
  airtime. The step-up packet's preamble must cover the powered units' scan
  cycle.
- **Rejected: dropping to a lower SF when airtime runs low.** Fewer units hear
  the packet, so there are more retries and more total airtime. When the budget
  is low, stop forwarding other people's texts first, keep the reserved SOS
  budget (§1.1) and space out retries.
- **Rejected: per-link adaptation like LoRaWAN's adaptive data rate.** It needs
  fixed point-to-point links, which a broadcast mesh doesn't have.
- Transmit power is the setting that can adapt without changing who hears a
  packet (§12.2, PROPOSED). The network SF must never change through a firmware
  update (§13.6).

### 13.8 SOS channels (DECIDED)
The SOS category in §12.2 is called a **channel** here. It is a field in the
SOS frame (4 bits, up to 16 values), not a separate radio channel; every SOS
still uses the one network SF.

**DECIDED (owner, 27 Sep):**
- Holding **`*` + `#`** for 3 s (V3's 4×4 keypad) or the side button (PCB) is
  the SOS. It is for responders only (§13.4).
- **Channels: General, Medical, Evacuation, Hazard, Food supply.** Trapped /
  water rising goes under **General**, as does anything that fits no other
  channel.
- **The sender picks a channel after the hold.** The bar fills as now (release
  early to cancel), then a channel screen appears:
  ```
  SOS: PICK HELP TYPE
  1 MED  2 EVAC 3 HAZ
  4 FOOD 0 GENERAL
  SENDS GENERAL IN 5s
  ```
  One digit sends the SOS on that channel. With no key pressed it goes out as
  **General** after 5 s, so a user who panics or can't read the screen still
  gets an SOS out. D cancels.
- The unit label prints the legend ("Hold * + # for SOS, then 1 Medical,
  2 Evacuation, 3 Hazard, 4 Food, 0 General: trapped, water rising, anything
  else").
- One channel per SOS. A user who needs two sends the most urgent one and
  describes the rest in a text.
- **No civilian presets** (§7.1). A civilian has the SOS with a channel, and
  60-character text.
- **Responder units choose which channels they receive, and can change the
  choice at any time:** a menu (e.g. B → SOS channels) where digits toggle
  channels and OK saves. It is kept in NVS, so it survives reboot and sleep.
  Default: all channels. The admin app can preset it at registration (e.g. a
  medical team). The status bar shows the active filter (e.g. "SOS: MED EVAC"),
  so a responder doesn't forget they are filtering.
- **"Help coming" is a preset reply on responder units** (§7.5), sent as a
  reply to a specific SOS. It tells the civilian someone is on the way, and
  tells other responder units that hear it that the SOS is being handled.
  **DECIDED 28 Sep:** on the SOS detail screen, **hold 4 and 6 together for
  3 s**; a progress bar fills as with the SOS hold, and releasing either key
  early cancels. The civilian's screen shows **"HELP IS COMING"**.
- **Safeguards, so a filter never loses an SOS:**
  1. Filtering is on the responder's display only. Every unit still forwards
     every SOS (§13.3); powered units never filter.
  2. **General SOS always shows** on every responder unit; it can't be filtered
     out.
  3. **Tab indicator (DECIDED 27 Sep):** the responder SOS screen has one tab
     per channel. A channel the responder hasn't selected that holds an SOS
     they haven't opened is marked with a dot and highlighted, so a hidden SOS
     is never silent. See the proposed screen below.
  4. **Escalation:** if no responder has answered an SOS with "Help coming"
     within ~15 min, it appears on every responder unit that received it, even
     units whose filter hides that channel. Example: a Food supply SOS when
     every responder in range has chosen Medical only. After 15 min with no
     "Help coming", all of them see it and one can pick it up or pass it on.
     **DECIDED 28 Sep:** Medical escalates sooner: 5 min in firmware V4
     (the owner asked for "shorter"; 5 min is the proposed value, to confirm).
  4a. **Reminders (DECIDED 28 Sep):** an unanswered SOS in a responder's list
     alarms again every 15 min, counted from its last alarm, until anyone sends
     "Help is coming". This covers both an SOS that alarmed on arrival (a
     selected channel) and one that escalated. Reminders stop if the sender
     has not been heard for an hour: while it still needs help it re-sends
     every 15 min (§7.6), so an hour of silence means it stopped or went off
     air. The entry stays listed without alarming.
  5. A responder unit sends the delivery ACK for every SOS it receives, shown or
     not, with the usual race so only one responder ACKs per area. The civilian
     sees DELIVERED, and a filter never triggers retries or the SF step-up
     (§13.7).

**DECIDED 27 Sep: the responder SOS screen.** Both boards have a monochrome
128×64 OLED, so "highlight" means an inverted label (white box, black text), not a
colour.
```
GEN MED EVAC [haz•] food
B-12 2nd fl  MED    4 min
C-04 ground  GEN   12 min
0-4:TAB A/B:MOVE C:OPEN
```
- Tab bar: channels the responder selected in capitals (`GEN MED EVAC`),
  channels not selected in lower case (`haz`, `food`). General is always
  selected.
- A channel not selected that holds an SOS not yet opened on this unit is
  **inverted with a dot**: `[haz•]` above. The dot clears once that tab is
  opened and comes back with the next new SOS there.
- The list below the tabs shows the SOS in the selected channels, newest
  first, with registry address, channel and age.
- Digits jump to a tab, matching the sender's channel screen (0 General,
  1 Medical, 2 Evacuation, 3 Hazard, 4 Food supply). Holding a digit for 1 s
  adds or removes that channel from the selection. A / B move up and down the
  list, C opens an SOS, D goes back to the home screen (where digits start a
  text, as on civilian units).
- Buzzer: SOS in selected channels sound it; a highlighted tab stays silent
  until the SOS escalates (safeguard 4), which sounds the buzzer and moves it
  into the list.

### 13.9 Still open from these decisions
1. Wake check interval T (0.5 s proposed), with measured current and airtime.
2. Battery floor for forwarding. §4.4's 50% on / 35% off was for relaying
   without relays. Claude's suggestion: forward SOS at any charge above a small
   reserve kept for the unit's own SOS (e.g. 15–20%), and keep 50% / 35% for
   forwarding text.
3. ~~The one-press presets.~~ **Decided 27 Sep:** removed for civilians; the
   channel list is decided (§13.8).
4. Heartbeat authentication. With the shared network key anyone can forge one:
   responders would think an area is covered, and nearby battery units would
   stop forwarding text (SOS is unaffected). Sign with a per-unit key from
   registration (§6.3), or accept the risk for v1.
5. Time source for responder expiry (§6.2) now that relay beacons, which were to
   carry time, are gone.
6. The network SF (field tests; SF9 likely), and for v1.1 the retry count N and
   the step-up SF.
7. "I'm safe". The removed SAFE preset let a household tell everyone it was
   safe, so responders could strike it off their search list. A user can still
   type SAFE as text, but responder units can't count free text by area. Keep a
   one-key "I'm safe" status, or leave it to text?

### 13.10 Firmware V4
`firmware_v4/` (Heltec V3 + 4×4 keypad, the V3 wiring) implements the
decisions above that need only firmware: 13.1 as light sleep with DIO1 and
keypad wake (the RX duty-cycle wake-up preamble is the `v4_wake` build, since
T is open), 13.2–13.5, 13.7 (SF as a build setting; `v4_sf9`), 13.8, and the
rollback part of 13.6. Version 4.1.0 (28 Sep) adds the 5-retry limit with
"SOS not delivered", the 15-min keep-alive after delivery, the 4 + 6 reply
hold, and GPIO 3 power sensing with the trend fallback. Version 4.2.0 adds the
15-min reminder alarms and the 5-min Medical escalation (safeguards 4 and 4a).
Not in V4: Bluetooth updates with signed images (the
protocol doc and the app come first), presetting responder channels from the
app, the PCB changes, and the open items in 13.9. Details:
`firmware_v4/README.md`.
