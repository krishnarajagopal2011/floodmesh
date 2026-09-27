# FloodMesh network architecture: decisions and reasoning

Record of the architecture discussion held on 23 September 2026 between the
owner and Claude (acting as reviewing mesh architect). It records what was
decided, what was proposed and is still awaiting the owner's confirmation, what
was rejected and why, and the numbers behind each call.

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
state.

| | Civilian (default) | Relay | Responder |
|---|---|---|---|
| Setup | **None required.** Works out of the box. Optional registration (§6.3) | See §4.3 | Bluetooth registration by the super admin |
| Sends | Presets, SOS key, text (§7) | Beacons, batched ACKs, zone summaries, forwarded traffic | Text/preset replies, bulletins, LOCATE, voice (if kept, §7.4) |
| Sleep | Deep sleep + shared listening windows (§5) | Always listening | Always listening |
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
   presses LOCATE on a distress. The civilian unit (reached in the listening
   window after one of its SOS retries) sends a 10-byte ping every 3 s for up to
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

### 4.1 Relays help but are never required (DECIDED)
Cell towers fail in floods because grid power fails, generators flood, backhaul
is cut, and all sites share those causes. The lesson: **nothing may depend on a
single point that shares a failure cause with everything else.** Every
life-safety function must work with **zero relays**:

| Function | With relays | Relays gone |
|---|---|---|
| Distress reaches responders | Relay collects, summarises, forwards | Civilian volunteers pass it along (mesh mode §4.4) |
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
~5 s + ~17 voice clips ~45 s ≈ **56 of 90 s**.

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

### 4.3 Who hosts relays (DECIDED, mechanism PROPOSED)
Priority: **households with solar or UPS power first**, then the highest floor
and best battery.

A relay listens all the time (~75 mA), so the 2000 mAh board battery lasts only
about a day. Relay hosts must feed USB-C from solar, a power bank or a UPS; the
board's charger works as a small UPS. A household UPS gets drained by lights and
phones during an outage, so a dedicated small panel plus power bank is more
dependable.

PROPOSED: relay mode switches on **automatically when external power is
detected**, with a manual override on the keypad. This needs the charger's PG
pin routed to a GPIO (PCB change 2, §9). Fallback without it: infer charging
from the fuel gauge's charge rate. OPEN until confirmed.

### 4.4 Mesh mode when relays go quiet (DECIDED)
- Relays send a beacon every few minutes. A unit that misses about 3 in a row
  switches to mesh mode. A unit that sends an SOS and gets no ACK after retries
  switches immediately.
- **No election.** Units above a battery threshold **volunteer**: start at ~50%,
  stop at ~35% (the gap prevents flip-flopping). The MAX17048 fuel gauge on the
  PCB gives a real state of charge.
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
- **Only distress-sized packets are relayed** by civilian volunteers.
- Batteries only fall during a flood, so over days the volunteer pool shrinks.
  After that, responders physically passing by (boats collecting stored
  messages) carry the load.

---

## 5. Sleep and listening

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
  U.FL connector. Either add a small 2.4 GHz antenna to the BoM, or switch to
  the WROOM-1 (built-in PCB antenna, same pinout). **OPEN.**
- Firmware updates: in provisioning mode, over WiFi via the volunteer's phone
  hotspot (faster than Bluetooth). The PCB has no USB data lines.

---

## 7. What users can send

### 7.1 Presets (DECIDED to keep as the fast path)
One-press distress presets (the existing four: SAFE, NEED MEDICAL, WATER GROUND
FLOOR, NEED EVACUATION), carrying head count and flags where possible.

### 7.2 SOS key (DECIDED)
A dedicated SOS on the **side button** (S1 on the PCB). PROPOSED behaviour:
hold 3 s, with a countdown and cancel; highest priority of all traffic;
addressed to responders; other civilian units relay it silently. It still hops
through the mesh; "direct" changes who **sees** it, not the path it takes.

OPEN: whether neighbours also see it. Neighbours are often the fastest
rescuers, but a sleeping neighbour only sees it at their next listening window
(§5).

### 7.3 Text via a 12-key keypad (DECIDED)
Replaces civilian voice as the information-rich channel.
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
  typing 60 characters under stress takes minutes, which is why the presets stay
  as a one-press path.
- Hardware: PROPOSED 12-key pad via an **MCP23017 I²C expander** (TCA8418 is
  hard to source quickly in India), with its interrupt on an RTC-capable pin,
  and a **sealed silicone keypad** for IP67. The side button stays as SOS. (A direct 3×4 matrix
  needs 7 GPIOs, which aren't free.)

### 7.4 Voice (OPEN)
History: the owner first set civilians to 1 × 10 s clip per hour and responders
to free voice, then proposed text instead of voice "due to legal limits".
Options still open:
- **Responders only.** Same hardware; keep the mic, amp and speaker.
- **Drop voice entirely.** Remove the INMP441, MAX98357A and speaker (cost,
  board area, idle current); keep the buzzer.

If civilian voice is kept anywhere: rate limit 1 per hour; a
**READY / BUSY / OFF** channel-load indicator shown before recording (from
`main`'s `docs/ARCHITECTURE.md`); relayed by relays only, never by civilian
volunteers; **off in mesh mode**.

### 7.5 Responder → civilian messages (PROPOSED)
No free keyboard needed on the civilian side. Responders send **preset messages
with a parameter** chosen from OLED menus ("Help coming", "Stay where you are",
"Boat arriving in 15/30/60 min", "Go to relief camp ▸ [list]", "Evacuate now").
On air it's 2 bytes plus the signature. They are sent as a **reply** to a
specific distress or a **broadcast** to a zone. Replies are delivered in the
civilian's post-retry listening window, cached by relays and volunteers.
Broadcasts are delivered as bulletins (§5). With the 12-key keypad, responders
can also send free text.

### 7.6 Acknowledgements
- **First hop:** the "honest relay-ACK" from `main`'s `docs/ARCHITECTURE.md`.
  The sender listens for a neighbour rebroadcasting its packet, at no extra
  airtime cost ("passed on by neighbour").
- **End to end:** an ACK from a relay or responder ("received by responders").
- Unacknowledged distress is retried with growing gaps (1, 2, 4… up to ~15 min).
  OPEN: the retry limit (forever, 24 h, or until 20% battery).

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

---

## 9. PCB V1.0 change list (board not yet fabricated)

See `docs/hardware/pcb-v1/README.md` for the board description.

| # | Change | Why | Status |
|---|---|---|---|
| 1 | NTC thermistor on the MCP73833 `THERM` pin (3-wire LiPo pack) | No battery temperature protection while charging in a hot sealed enclosure | PROPOSED, high priority |
| 2 | Charger `PG` pin → spare GPIO (e.g. IO47) | Firmware must know about external power to pick relay hosts | PROPOSED |
| 3 | `LORA_DIO1` IO38 → **IO19** (RTC-capable) | Radio can wake the ESP32 from deep sleep | PROPOSED (no USB data, so IO19 is free) |
| 4 | Reverse-polarity protection on J3 | JST-PH LiPo packs are wired both ways by different vendors | PROPOSED |
| 5 | MAX17048 on the `BATT+` side of the power switch | Keeps its learned battery model across power cycles | PROPOSED |
| 6 | USB data | Not needed: provisioning is over Bluetooth | DECIDED (charge-only stays) |
| 7 | 2.4 GHz antenna for WROOM-1U, or switch to WROOM-1 | Bluetooth provisioning needs an antenna | OPEN |
| 8 | 12-key keypad via I²C expander **MCP23017** (TCA8418 optional) + sealed silicone pad, interrupt on an RTC pin | Text input (§7.3). MCP23017 is easy to source in India and is what the prototypes use | PROPOSED |
| 9 | Optional 32.768 kHz crystal (move `MIC_WS`/`MIC_SCK` to IO47/IO48 to free IO15/IO16) | Accurate sleep timing, narrower windows. Moot if voice is dropped and the mic removed | OPTIONAL |
| 10 | Mic, amp and speaker | Remove if voice is dropped entirely (§7.4) | OPEN |
| 11 | Power the INMP441 mic from a **switched rail**, not always-on 3V3 | The mic draws ~1.4 mA whenever powered, which would dominate deep-sleep current | PROPOSED |

Check GPIO budget once items 2, 3, 8 and 9 are settled together.

---

## 10. Firmware changes implied (current code on `9th-Sept-2026`)

| Area | Current | Needed |
|---|---|---|
| `fm_airtime` | Alarms exempt from duty cycle | Reserved alarm budget (§1.1) |
| `FM_REPLAY_SLOTS` (`include/fm_auth.h`) | 32 | **≥128** for 100 senders (~2 KB) |
| `FM_DEDUP_SLOTS` (`include/fm_dedup.h`) | 32 | **~256** to survive a burst of 100 distress + retries (~3 KB) |
| Pin map | Heltec V3 only (`include/floodmesh_pins.h`) | Separate board variant for PCB V1 |
| Input | 2×2 matrix + PTT | 12-key via TCA8418 + side SOS button |
| Mesh | SNR-biased delay | Add battery + floor weighting; volunteer thresholds; ACK caching; batched ACKs |
| Sleep | Continuous receive | Deep sleep + synchronised 20 s / 5 min windows |
| Auth | One PSK (public placeholder) | Per-unit alarm keys for registered units; Ed25519 responder signatures; super-admin public key built in |
| Voice | Codec2 Layer 2 | Responders only or removed (§7.4) |
| Message ID (V3) | 8-bit, RAM only, resets at boot; separate 32-bit auth counter in NVS | One 16-bit per-sender ID, flash block reservation of 16, receiver table saved to flash every ~5 min (§12.2) |

---

## 11. Open questions (summary)

1. Gazette text: duty cycle, per device or per channel, listen-before-talk.
2. Shared window: 20 s every 5 min, or another balance?
3. Homing details: LOCATE pings + sounding the civilian's buzzer.
4. Keypad hardware: MCP23017 + silicone pad (prototypes use MCP23017).
5. Voice vs 12-key text: decide **after** the prototype tests (both are built
   on every prototype; see `docs/prototype-v2-build.md`).
6. SOS key: visible to neighbours or responders only?
7. Per-unit alarm keys from optional registration.
8. Built-in shared alarm key for unregistered units: keep or drop?
9. Relay mode: automatic on external power + manual override?
10. Bluetooth antenna: WROOM-1U + antenna, or WROOM-1?
11. Retry limit for unacknowledged distress.
12. Repository: `main` holds an unrelated older history (`docs/ARCHITECTURE.md`,
    one `.ino`). Decide which branch is canonical; `main`'s design principles
    and honest relay-ACK are worth folding in.
13. Naming in docs: the PCB title block says "Vazworks". Confirm whether
    that is the owner's organisation.
14. Frame format, message IDs, ACKs, SOS categories, the Verified Responder
    label and the owner's 26 Sep proposal: see §12.5.

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
- **16-bit per-sender message ID**, persisted in flash (27 Sep; details below).

**PROPOSED layout (67 B, the same airtime as today's V3 text frame):**

| Field | Bits | Notes |
|---|---|---|
| Version + type | 8 | text, SOS, alarm, ACK, beacon, locate, buzz, bulletin |
| Flags | 8 | hide from civilians, ACK requested, retry 0–7, responder-signed |
| Hops | 8 | limit + taken; relays change it, so it is outside the auth tag |
| Source | 24 | 6-digit call sign |
| Destination | 24 | reserved values: all units, all responders, all relays |
| Message ID | 16 | per-sender counter, persisted in flash (DECIDED below) |
| Category + head count | 8 | 4 + 4 bits |
| Battery % | 8 | |
| Age in minutes | 8 | relays add their holding time; outside the auth tag |
| Text | ≤ 360 | 60 characters × 6 bits |
| Auth tag | 64 | truncated HMAC (civilian frames) |

An SOS without text is ~23 B (~70 ms at SF7). Each extra byte costs ~1.5 ms at
SF7 and ~4.5 ms at SF9. Signal strength is **not** sent for the relay race:
each receiver measures it itself (§4.4). PROPOSED: the ACK carries the RSSI/SNR
it heard (1 B), for adaptive power control.

**DECIDED (owner, 27 Sep): message ID is a 16-bit per-sender counter, kept in
flash so that switching the unit off and on never breaks it.** Build this in the
next firmware version.

*Why not a random ID.* The owner first asked for a random 5–10 bit ID. Random
IDs collide by the birthday paradox (50% chance after ~7 messages at 5 bits,
~38 at 10 bits, ~300 at 16 bits, ~4,800 at 24 bits), and a collision silently
drops a real message as a "duplicate". A counter cannot collide until it wraps
(65,536 messages from one unit; over 47 h even at the airtime cap). Only a
counter gives replay protection and ordering (a responder can tell #43 is newer
than #41 and that #42 is missing), and it needs fewer bits than a random ID
large enough to be safe.

*Rules (sender):*
- Each **new** message takes the next ID. A **retry keeps its ID** and raises the
  retry field. Forwarding someone else's message never uses the relay's counter.
- **Flash (NVS) is the source of truth, not RAM and not RTC memory.** Users will
  switch units off often, and a power switch or battery pull wipes both RAM and
  the ESP32-S3's internal RTC memory. The design must not depend on RTC memory
  or on any external RTC chip (the PCB has none); RTC memory may only be used as
  a speed-up.
- **Block reservation**, as Firmware V3's `fm_ids.cpp` already does for its
  32-bit auth counter: flash holds a high-water mark meaning "no ID at or above
  this has been used". Before handing out the first ID of a block, the unit
  writes mark + `BLOCK` to flash, then hands out IDs from RAM until the block is
  used up. The flash write happens **before** the frame is sent, so a power cut at
  any moment can never make the unit reuse an ID; it can only skip unused ones.
- **Block size 16** (not V3's 256). Frequent power cycling skips at most 15 IDs
  per boot that sends something, so 16 bits last ~4,000 such boots before
  wrapping, and wrapping is handled below. That is 1 flash write per 16
  messages, well inside NVS wear limits.
- **Lazy reservation:** a boot that sends nothing writes nothing (most boots).
- Deep-sleep wake is treated like a boot: reload the mark from flash.
- If the flash write fails, still send (a suppressed SOS is worse), and log it
  loudly, as V3 does.
- **Factory reset** starts a new epoch: registration or the unit's first frame
  after reset tells receivers to forget that call sign's old counter (mechanism
  to settle with the key design, §6.3).

*Rules (receiver):*
- Duplicate check on **(call sign, message ID)**, never on the ID alone.
- Per sender, keep the **last ID and the time it was heard**. Compare IDs with
  wrap-around (serial-number) arithmetic: newer if `(id − last) mod 65536` is
  between 1 and 32,767. Equal = duplicate or retry (not shown again). Older =
  replay, dropped. **Gaps are normal** (skipped block remainders); never expect
  `last + 1`.
- Entries **expire after 24 h**, so a unit that wrapped or was off for a long time
  is accepted again. Known limit: a replay older than 24 h is not caught until
  frames carry a time (relay beacon time, §5).
- Receivers also get switched off. Save the per-sender table to flash **every
  ~5 minutes if it changed**, and reload it at boot. After a power cut a
  responder may at worst re-show an SOS heard in the last few minutes.
- Size: ≥128 senders × (3 B call sign + 2 B ID + 4 B time) ≈ 1.2 KB.

*Current code to change:* Firmware V3's `fmNextMsgId()` is a RAM-only counter,
reset at every boot and truncated to 8 bits in `fm_mesh.cpp`; the separate
32-bit auth counter is persisted. The next version merges them into this single
persisted 16-bit ID, which is also the replay counter covered by the auth tag.

**PROPOSED: ACK as its own frame** (~21 B, ~65–70 ms at SF7), only for SOS and
unit-addressed messages, never for broadcasts (100 ACKs would jam the channel).
Relays batch several ACKs (~5 B each, §4.2). ACKs drive the NOT SENT /
NOT CONFIRMED / DELIVERED states. Retries keep the same message ID with the
retry count raised.

**PROPOSED: SOS categories** (at most ~6, so a stressed user can choose):
General SOS, Medical, Trapped / water rising, Evacuation, Food and water,
Hazard. Safeguards: relays never filter; responders see a count of SOS in other
categories; an uncategorised SOS shows to everyone; responder units default to
"All". OPEN: the final list.

### 12.3 SF choice (OPEN)

| | SF7 | SF8 | SF9 |
|---|---|---|---|
| Sensitivity vs SF7 | – | ~+2.5 dB | ~+5 dB |
| Text 67 B | 131 ms | 242 ms | 443 ms |
| SOS 23 B | 70 ms | 130 ms | 239 ms |
| Texts per hour in 90 s | ~690 | ~370 | ~200 |
| SNR floor | −7.5 dB | −10 dB | −12.5 dB |

V3 runs SF7. The 14 Sep decision stands: make SF selectable and let field data
choose. The `fm_mesh` contention slot is tuned for SF7 and needs retuning for
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

1. ~~Message ID~~: DECIDED 27 Sep, 16-bit per-sender counter in flash (§12.2).
2. ACK frame rules as in §12.2.
3. The SOS category list.
4. Verified label: signature from the start, or bits-only for bench tests first.
5. Certificate in every responder broadcast; PIN to send; A + D combination;
   urgent broadcasts wake sleeping units.
6. Sleep: 10 min per hour vs 20 s per 5 min, and the SOS-only wake-up preamble.
7. Registration required or optional; responder registration by organisations.
8. Relay fallback: the owner's self-election vs §4.4 volunteering.
9. Fox hunt: automatic 100 m buzzer vs an explicit BUZZ command.
