# FloodMesh firmware V4: SOS channels, responders, powered units, radio wake

V4 is the next field-test firmware after V3. It puts the architecture decisions
of 27 September 2026 (`docs/architecture.md` §13) onto the same hardware as V3:
a **Heltec WiFi LoRa 32 V3/V3.2, a 4×4 membrane keypad and an active buzzer**.
The wiring is unchanged from V3. There is no voice, no MCP23017, no mic,
speaker or side button.

`firmware_v3/` is kept as it was, for comparison tests. The root firmware
(`src/`, `include/`) is not changed by this build either.

```
pio run -d firmware_v4 -e v4               # build
pio run -d firmware_v4 -e v4 -t upload     # flash
pio device monitor -b 115200               # serial console
```

CI builds every env: the `firmware-v4`, `firmware-v4_sf9` and `firmware-v4_wake`
artifacts of `.github/workflows/build.yml`.

| Env | Radio | Use |
|---|---|---|
| `v4` | SF7, 16-symbol preamble | Default. Hears V3 units' SOS and texts |
| `v4_sf9` | SF9, 16-symbol preamble | The spreading-factor field test (§13.7) |
| `v4_wake` | SF7, 512-symbol (~0.5 s) wake-up preamble, RX duty cycle on battery | The radio-wake experiment (§13.1) |

**Every unit in one test must run the same env.** The spreading factor and
the preamble decide who can hear whom.

**Status (2026-09-28, 4.3.0):** 4.2.0 runs on five units (A–E) since 28 Sep; the SOS,
responder and relay behaviour of §5–6 is being field-tested. 4.3.0 adds the field
logger (§10) and shows civilian units as `USER` in the header (the role is still
`civilian` in serial output and the admin-app protocol).

## 1. What changed from V3

| Decision (`docs/architecture.md`) | V4 behaviour |
|---|---|
| No civilian presets (§7.1) | The A alerts menu is gone |
| SOS channels (§13.8) | After the `*` + `#` hold, a channel screen: 1 Medical, 2 Evacuation, 3 Hazard, 4 Food supply, 0 General. General goes out by itself after 5 s; D cancels |
| SOS for responders only (§13.4) | Civilian units forward an SOS silently: nothing on screen, no buzzer. Responder units show it and sound the buzzer |
| Responder channel filter (§13.8) | Tabs per channel, selection kept in flash, dot + inverted label on an unselected tab holding an unopened SOS, General always selected |
| Delivery ACK and "Help is coming" (§13.8) | A responder unit acknowledges every SOS it receives (one per area, by a delay race). The reply is **hold 4 + 6 for 3 s** on the SOS detail screen, with a progress bar; releasing early cancels |
| Escalation and reminders (§13.8) | An SOS in an unselected channel that nobody answers with "Help is coming" alarms every responder unit that has it: Medical after 5 min, the others after 15. Any unanswered SOS in the list then alarms again every 15 min until someone replies |
| SOS retries (§7.6) | At most 5 retries, after 1, 2, 4, 8 and 15 min. No answer to the 5th: two beeps and `SOS NOT DELIVERED`. After `DELIVERED` the SOS is re-sent every 15 min until "Help is coming", so it survives the one responder unit that had it going off air |
| No relay role (§13.2) | A unit on external power stays awake and forwards everything. The admin app's "relay" role is treated as civilian. External power is measured through an optional GPIO 3 divider, or guessed from the battery-voltage trend without it |
| Forwarding rules (§13.3) | Every unit forwards SOS, ACKs and heartbeats. A battery unit forwards a text only if it has not heard a powered unit directly in the last 65 min |
| Heartbeat (§13.5) | A powered unit sends one every 30 min; every unit forwards it; the status screen lists powered units and how long ago each was heard |
| Radio wake (§13.1) | A battery unit light-sleeps when idle; a received packet (DIO1) or a key press wakes it within about a millisecond |
| One SF, chosen by field tests (§13.7) | SF is a build setting; the relay contention slot and the listen-before-talk slot now follow it |
| Rollback (§13.6) | A newly installed image stays on trial until it has run 60 s with a working radio; otherwise the bootloader returns to the old one |
| Relay race weighting (§4.4) | The relay delay now adds a term for the unit's own battery (external power counts as full) |

Not in V4, because they need more than firmware:

- **Bluetooth firmware updates with signed images** (§13.6). The protocol has
  to be written into `docs/ble-provisioning-protocol.md` first, then the
  Flutter app and the unit. V4 keeps V3's WiFi update mode as a bench tool,
  now with rollback.
- **Presetting a responder's channels from the admin app** at registration
  (§13.8). Same reason: a protocol change. Responders pick channels on the unit.
- **The PCB changes** (§9): power-good pin, DIO1 on IO19, keypad wiring.
- **Open decisions**, left as they are: the battery floor for forwarding,
  heartbeat authentication, the responder time source, "I'm safe" (§13.9),
  and the reserved alarm budget (§1.1: SOS and ACK frames are still exempt
  from the duty-cycle ledger in software).

## 2. Wiring

Exactly as V3. The keypad goes **straight to the Heltec GPIOs** (8 wires). The
buzzer needs one transistor.

![Keypad and buzzer wiring](docs/keypad-wiring.png)

(`docs/keypad-wiring.svg` is the same diagram as a vector file. Regenerate it
with `python3 tools/gen_wiring_svg.py`.)

| Keypad pin (left → right on the ribbon) | Signal | Heltec GPIO |
|---|---|---|
| 1 | ROW1 (1 2 3 A) | **39** |
| 2 | ROW2 (4 5 6 B) | **40** |
| 3 | ROW3 (7 8 9 C) | **41** |
| 4 | ROW4 (* 0 # D) | **42** |
| 5 | COL1 (1 4 7 *) | **4** |
| 6 | COL2 (2 5 8 0) | **5** |
| 7 | COL3 (3 6 9 #) | **6** |
| 8 | COL4 (A B C D) | **7** |

Buzzer: GPIO 2 → 1 kΩ → 2N2222 base; emitter to GND; collector to buzzer (−);
buzzer (+) to 3V3 (not 5V, which is only there on USB). Use an ACTIVE buzzer.

`firmware_v3/README.md` §1 has the keypad checks to do before soldering.

**Power-sense divider (optional, recommended for units that will sit on
solar, a power bank or USB):**

```
Heltec 5V pin ──[100 kΩ]──┬── GPIO 3
                          └──[100 kΩ]── GND
```

The same divider as prototype v2 (`docs/prototype-v2-build.md` §5). The 5V pin
is USB VBUS: GPIO 3 reads ~2.5 V on external power and ~0 V on battery, and the
divider draws current only while plugged in. V4 finds it at boot
(`[PWR] power-sense divider on GPIO 3: fitted`) and then knows the power state
exactly; without it V4 falls back to the battery-voltage trend (§4).

## 3. Keys

```
 1  2  3  A        A = up / previous
 4  5  6  B        B = down / next
 7  8  9  C        C = OK   (hold = send)
 *  0  #  D        D = back / delete
```

| Where | Key | Action |
|---|---|---|
| Anywhere | hold **\*** and **#** together 3 s | **SOS**: the channel screen opens |
| SOS channel screen | **1** Medical, **2** Evacuation, **3** Hazard, **4** Food supply, **0** General | send the SOS on that channel |
| SOS channel screen | no key for 5 s | sent as **General** |
| SOS channel screen | D | cancel, nothing sent |
| Home | 0–9 | start a text (that key is typed) |
| Home | A | SOS list (responder units only) |
| Home | B | status |
| Home | C | inbox |
| Home | hold D | stop your own SOS retries |
| Compose | 2–9 | letters (T9 predicts the word) |
| Compose | 0 | space (accepts the T9 word) |
| Compose | 1 | punctuation `. , ? ! - ' /` (tap repeatedly) |
| Compose | \* | next T9 word |
| Compose | # | mode: T9 → ABC (multi-tap, for Tanglish) → 123 |
| Compose | A / B | previous / next T9 word |
| Compose | C | accept the word; **hold C = send** |
| Compose | D | delete; on an empty text, leave. **Hold D = discard** |
| Inbox | A / B, C, D | scroll, open, back |
| SOS list | 0–4 | jump to that channel's tab |
| SOS list | **hold 0–4 for 1 s** | select / unselect that channel (General always stays) |
| SOS list | A / B, C, D | move, open, back (tab → main list → home) |
| SOS detail | **hold 4 and 6 together 3 s** | send **HELP IS COMING** (bar fills; release early to cancel) |
| SOS detail | C or D | back |
| Status | A / B | scroll the heard list |
| Status | C | range-test ping on/off |
| Status | # | power mode AUTO → ON → OFF (§4) |
| Status | \* | powered units (coverage) |
| Coverage | A / B, D | scroll, back |

A key press while the screen is blank only wakes it. A key press on a popup
only dismisses it. A new SOS on a responder unit beeps for about a minute, or
until a key is pressed.

**Boot combinations** (hold while powering on or pressing RST):

- hold **#**: BLE provisioning with the FloodMesh Admin app;
- hold **B**: WiFi update mode for 10 minutes (§8);
- hold **\*** and **0** for 10 s: factory reset (role, keys, call sign, stored
  WiFi networks and field-log server address, SOS channel selection). The
  field log's records and counters are kept (§10).

## 4. How it behaves

### Sending an SOS (any unit)

1. Hold `*` and `#` for 3 s. The channel screen opens.
2. Press 1–4, or 0 for General (trapped, water rising, anything else). With no
   key it goes out as General after 5 s.
3. The unit sends at once and shows `SOS SENT`. The home screen then shows the
   state:
   - `waiting, retry 2m`: no answer yet. Up to 5 retries, after 1, 2, 4, 8
     and 15 min.
   - `DELIVERED`: a responder unit has it. The SOS is still re-sent every
     15 min until help is coming, so a responder who arrives later, or one
     whose unit replaced the first, still gets it.
   - `HELP IS COMING`: a responder replied. Nothing more is sent.
   - `NOT DELIVERED`: the 5th retry went unanswered (about 31 min after the
     first send). The unit beeps twice and says `SOS NOT DELIVERED`; move
     higher or outside and send again. An answer that still arrives later
     turns it into `DELIVERED`.
4. Hold D on the home screen to stop sending (while waiting or delivered).

### Responder units

- **Home** shows `SOS: n (k NEW)`. **A** opens the SOS list.
- **Tab bar**: selected channels in capitals (`GEN MED EVAC`), the others in
  lower case (`haz food`). An unselected channel holding an SOS this unit has
  not shown yet is **inverted with a dot**. The tab being viewed is underlined.
- **Main list**: SOS in the selected channels, plus any escalated one, newest
  first: call sign, channel, age, `H` if someone sent Help coming, `!` if
  escalated.
- **Detail** (C): channel, first/last heard, count, hops, signal, who answered.
  **Hold 4 and 6 together for 3 s** to send HELP IS COMING: a bar fills like
  the SOS hold, and releasing either key before it is full cancels. The
  sender's unit then shows `HELP IS COMING` and stops sending.
- A repeat of an SOS you already have (a retry or a 15-min re-send) updates
  the entry without a new alarm.
- Every SOS received is acknowledged, shown or not. When several responder
  units hear the same SOS, the first to answer wins and the others hold back.
- An SOS in an unselected channel with no Help is coming escalates: it moves
  into the main list and the unit alarms (`!! SOS ESCALATED !!`). Medical
  escalates after 5 min, the other channels after 15.
- Any unanswered SOS in the main list alarms again every 15 min
  (`!! SOS WAITING !!`) until someone sends Help is coming. Reminders stop if
  the sender has not been heard for an hour; the SOS stays listed.
- The selection survives reboots. `select <1-4>` on serial toggles it too.

### Powered units and heartbeats

**With the GPIO 3 divider (§2)** the unit measures VBUS and knows exactly.

**Without it** V4 watches the battery voltage, read every 10 s:
- Plugging in makes it jump up and keep rising. A jump of 40 mV between two
  readings, or a rise of 30 mV over 10 min, means **external power**.
- Unplugging makes it drop and keep falling. A drop of 40 mV, or a fall of
  20 mV over 10 min, means **battery**.
- A flat voltage changes nothing, because a sleeping unit on battery also
  looks flat for hours. The first reading after boot starts it: 4.15 V or more
  (a charger holds the cell near 4.2 V), or no cell, counts as external.
- Blind spot: a unit plugged in with an already full cell shows no rise.

Use the override when it matters: Status → **#** (AUTO → ON → OFF), or
`power auto|on|off` on serial. The status screen shows `VBUS` or `trend`, and
the header shows `P` before the battery % while the unit counts as powered.
Since 4.3.1 the chosen mode is saved and survives power-offs (factory reset
returns it to AUTO). Without the divider, set it explicitly: AUTO guessed
wrong in the 28 Sep field test (`docs/field-tests.md`).

**Battery reading per board (4.3.2).** Heltec V3 sub-revisions differ in the
battery divider. Unit D, a board printed "V3", read 2.85 V for a 4.07 V cell
(a steady 0.70x; with the divider gate driven LOW it read 0 V, so HIGH is right).
Measure the cell with a multimeter while the unit is on and send
`batt cal <volts>`: the unit stores its own factor, and `batt` shows raw volts,
factor and result. The factor describes the board, so a factory reset keeps
it; `batt cal reset` removes it.

A powered unit:
- never sleeps and forwards every frame;
- sends a heartbeat within a minute of getting power, then every 30 min;
- gets the smallest battery term in the relay race (external power counts
  as a full battery), so it tends to relay first.

A unit on battery:
- sleeps when idle (§5);
- forwards SOS, ACKs and heartbeats always;
- forwards texts only if no powered unit's heartbeat was heard **directly**
  in the last 65 min. The status screen shows `near <age>` or `near none`.

The **coverage page** (Status → `*`) lists every powered unit heard: battery,
hops, time since its last heartbeat, and `ok` / `late` (35 min) / `DOWN?`
(65 min).

## 5. Sleep and radio wake

A unit on battery puts the ESP32 into **light sleep** once the screen has gone
off and nothing is queued. The SX1262 keeps receiving. A complete packet raises
DIO1 (GPIO 14), and a key press pulls a keypad column low (rows are held LOW
while asleep). Either one wakes the ESP32 within about a millisecond. RAM,
timers and the screen contents survive, because light sleep is not a reboot.
Each sleep is capped at 1 s as a safety net, so a missed wake source costs at
most a second, and never runs past the next field-log upload window.

The unit stays awake while any of these hold:
- the screen is on
- a popup is showing or the buzzer is sounding
- something is queued for relay
- a key is held
- the range-test ping is on
- WiFi update mode is open
- a field-logger upload window is running (§10)
- serial input arrived in the last 30 s
- it counts as powered

`sleep off` on serial disables sleep until reboot. The status screen shows the
share of time asleep.

**Estimates, not measurements:**

| Build | Current |
|---|---|
| V3 always on | ~45–50 mA |
| V4 `v4` asleep (receiver on continuously, ~5–6 mA with RX boost) | roughly 7–10 mA |
| V4 `v4_wake`, the receiver's own duty cycle | lower again, if the extra airtime is acceptable |

In `v4_wake`, every frame carries a ~0.5 s preamble: an SOS goes from ~65 ms
to ~0.57 s of airtime. The check interval T is still an open decision (§13.9).

**First bench test for V4:**

1. Battery current in the three states on one unit: screen on, asleep (`v4`),
   asleep (`v4_wake`).
2. Send a text to a sleeping unit. It must wake, beep and show it.
3. Press a key on a sleeping unit. The screen must come on.
4. Hold `*` + `#` on a sleeping unit. The SOS channel screen must open.

## 6. Testing SOS, ACKs and heartbeats

You need a responder unit (registered with the FloodMesh Admin app: boot with
`#` held) and two civilian units.

1. **Silent forwarding:** civilian A sends SOS Medical. Civilian B shows
   nothing and stays silent (serial: `forwarded silently`). The responder
   alarms and lists it. A shows `DELIVERED` within seconds.
2. **Help is coming:** on the responder, open it (A, C), hold 4 and 6. Release
   once halfway: nothing is sent. Hold again until the bar is full: A shows
   `HELP IS COMING`.
3. **Filter and dot:** on the responder, hold 4 for 1 s to unselect Food. A
   sends SOS Food. The responder does not alarm; the `food` tab is inverted
   with a dot. Press 4 to view it; the dot clears. A still shows `DELIVERED`.
4. **Escalation and reminders:** repeat 3 and wait 15 min without Help is
   coming. The responder alarms and the SOS moves into the main list with `!`.
   Wait 15 min more: it alarms again (`!! SOS WAITING !!`). Unselect Medical
   and send SOS Medical: it escalates after 5 min. (For a quicker bench test,
   shorten `FM_SOS_ESCALATE_MS`, `FM_SOS_ESCALATE_MED_MS` and
   `FM_SOS_REALARM_MS` in `platformio.ini`.)
5. **Retries:** send an SOS with no responder in range. Serial shows 5 retries,
   1, 2, 4, 8 and 15 min apart, then after one more minute two beeps and
   `SOS NOT DELIVERED`. Repeat, but switch a responder on after the second
   retry: it answers the next retry and A shows `DELIVERED`.
6. **Keep-alive:** after `DELIVERED`, switch that responder off and another
   one on. Within 15 min the second responder gets the SOS as new.
7. **Heartbeat and the text rule:** set one unit to power ON (Status, #). Its
   heartbeat appears on the others' coverage page within a minute. A battery
   unit one hop from it shows `near 0m`. It stops relaying texts (`relay`
   count on the status line stays still) but still relays SOS.
8. **Mixed V3/V4:** V3 units still exchange texts with V4 units and relay V4
   SOS frames. They show a V4 channel SOS as `ALERT ?`, and they drop ACK and
   heartbeat frames, so put only V4 units on the relay path of an ACK test.

## 7. On air

| Frame | Type | Length | Airtime at SF7/BW125 |
|---|---|---|---|
| SOS (alarm) | 0x01 | 22 B | ~65 ms |
| Text, 60 chars | 0x03 | 67 B | ~131 ms |
| ACK (delivered / help coming) | 0x04 | 28 B | ~75 ms |
| Heartbeat | 0x05 | 23 B | ~70 ms |

**Alarm byte values:**
- **4–8**: SOS on General, Medical, Evacuation, Hazard and Food supply. 4 is V3's SOS value, so a V3 SOS arrives as General.
- **0–3** (V3's presets) are still decoded: MEDICAL → SOS Medical, WATER → SOS General, EVACUATE → SOS Evacuation, and SAFE → a status text everyone sees.

**ACK frame:** carries the responder's call sign in the header, then the kind (0 delivered, 1 help coming), the SOS sender's call sign and the msgId of the SOS it answers.

**Heartbeat:** carries the powered unit's battery % and a power flag.

**Common to all frames:** the PSK signs every frame except the hop byte, and every frame carries an anti-replay counter.

**Relay priority bands, earliest first:** SOS and ACK, then text, then heartbeat. Heartbeats are refused once the hour's airtime passes 80% of the 2.5% budget, so they go quiet first.

## 8. WiFi updates, trial and rollback

As V3 (`firmware_v3/README.md` §7): store a 2.4 GHz network with
`wifi ssid` / `wifi pass`, open update mode (hold B at power-on, or `ota`),
and push `firmware.bin` with `POST /update`. `FM_OTA_PASSWORD` must be given
at build time.

New in V4: an image installed this way **starts on trial**. The unit keeps it
only after the radio comes up and it has run for 60 s. If it fails the radio
self-test, or resets before then (a crash loop, a hang), the bootloader goes
back to the previous image. Entering provisioning or a factory reset accepts
the running image first, because both end in a restart. Units given to users
still need signed images, which come with the Bluetooth update (§13.6).

## 9. Serial commands

`help`, `info`, `prov`, `sos <0-4>` (0 GENERAL, 1 MEDICAL, 2 EVACUATION,
3 HAZARD, 4 FOOD), `sos stop`, `sos list` (responder units), `select <1-4>`
(toggle a responder channel), `text <message>`, `ping on` / `ping off`,
`heard`, `cover`, `power auto|on|off`, `sleep on|off`, `beep`,
`callsign <name>`, `wifi` / `wifi ssid <name>` / `wifi pass <password>` /
`wifi forget`, `ota` / `ota off`, `batt`, `batt cal <volts>`, `batt cal reset`
(§4, battery reading per board), `beep tone <Hz>`, `buzzer`, `buzzer active`,
`buzzer passive [Hz]`.

**Buzzer type (4.3.3).** The wiring expects an ACTIVE buzzer (it has its own
oscillator; the pin is held HIGH). A PASSIVE buzzer driven that way only clicks:
unit E's beeps were feeble for this reason. `beep tone <Hz>` tries a square wave
without saving; `buzzer passive <Hz>` saves it for every beep (E uses 2700 Hz),
`buzzer active` returns to the default. The setting describes the board, so a
factory reset keeps it. A passive buzzer on 3.3 V is still quieter than the
active ones; replacing it is the real fix.

Field logger (§10): `log`, `log send`, `log url <base>`, `log url default`,
`log clear`, `wifi2 ssid <name>`, `wifi2 pass <password>`, `wifi2 forget`.
`wifi` shows both networks. No `wifi` or `wifi2` line is ever echoed back,
because it may carry a password.

Every received frame prints one CSV line:

```
RXLOG,<ms>,<ALARM|TEXT|ACK|HEARTBEAT>,<from>,<msgId>,<hops>,<rssi>,<snr>,<text>
```

`hops` is 0 when heard directly. Other log lines: `[SOS] ...`,
`[MESH] relayed ...`, `[MESH] ack DELIVERED -> ...`,
`[MESH] queued ack ... dropped - already answered`, `[HB] heartbeat -> sent`,
`[PWR] EXTERNAL POWER` / `BATTERY`, `[OTA] new firmware kept`.

Field-logger lines start with `[LOG]`: the boot summary, each upload window
(`joining network 1`, `clock from SNTP ...`, `POST 100 records (seq ...) -> 200`,
`upload window done in 12 s`), drops and flash errors.

## 10. Field logger

During field tests every unit records what it receives and sends, keeps it in
flash, and uploads it every 5 minutes over the volunteer's phone hotspot to the
field-logger web app (`webapp/field-logger/`). Nobody has to write readings
down. The contract with the web app is `docs/field-logger-protocol.md`; the
code is `include/fm_log.h` and `src/fm_log.cpp`.

This is a test tool, not part of the product: it uses WiFi, which the product
does not. `-D FM_LOG_ENABLE=0` builds V4 without it.

**Status (2026-09-28):** runs on unit D over home WiFi against the deployed web app
(floodmesh-field.vercel.app): verified TLS, SNTP with fallback, uploads acknowledged
with 200. Not yet on the field units.

### What is recorded

| Kind | When | Main fields |
|---|---|---|
| `boot` | once per boot | ESP reset reason (`POWERON`, `SW`, `PANIC`, `BROWNOUT` ...), firmware, image id |
| `rx` | every frame received and accepted, including an SOS a civilian unit forwards silently | type (`SOS`, `ALARM`, `TEXT`, `PING`, `ACK`, `HEARTBEAT`), from, msgId, hops, dBm, SNR, text, SOS channel |
| `echo` | a neighbour relayed one of this unit's frames ("passed on") | type, msgId, dBm, SNR |
| `tx` | this unit sent its own frame (SOS, text, ping, ACK, heartbeat, HELP IS COMING) | type, msgId, sent or not, text / channel |
| `relay` | this unit relayed someone else's frame | type, from, msgId |
| `sos` | this unit's own SOS changed state | `SENT`, `RETRY`, `DELIVERED`, `HELP`, `NOT_DELIVERED`, `STOPPED`, channel, retry count |
| `status` | at boot, then every 5 min | battery V and %, external power, airtime, stations heard, share of time asleep, role |

dBm and SNR are exactly the values of the `RXLOG` serial line. Every record
carries a sequence number that only ever grows (across reboots too), the boot
counter, the uptime in ms, and the Unix time once the clock has been synced
(see Time below).

### Setting a unit up

1. **Build with the fleet key and the server.** Both are test secrets or
   settings, passed at build time and never committed:

   ```
   # bash
   export PLATFORMIO_BUILD_FLAGS="-D FM_OTA_PASSWORD='\"<ota pw>\"' -D FM_LOG_KEY='\"<32-64 hex>\"' -D FM_LOG_URL='\"https://<app>.vercel.app\"'"
   # PowerShell
   $env:PLATFORMIO_BUILD_FLAGS = "-D FM_OTA_PASSWORD='`"<ota pw>`"' -D FM_LOG_KEY='`"<32-64 hex>`"' -D FM_LOG_URL='`"https://<app>.vercel.app`"'"
   pio run -d firmware_v4 -e v4 -t upload
   ```

   `FM_LOG_KEY` is the web app's `LOG_KEY`: 32 to 64 hex characters, the same
   for every unit. **The HMAC key is the bytes the hex spells** (16 to 32
   bytes), not the hex text; the server must decode it the same way
   (`Buffer.from(LOG_KEY, 'hex')` in Node). Without `FM_LOG_KEY` a unit still
   records to flash but never uploads, and says so once at boot. Changing
   `PLATFORMIO_BUILD_FLAGS` makes PlatformIO rebuild everything; that is
   expected.
2. **Server address**, if it was not built in or has changed:
   `log url https://<app>.vercel.app` (stored in NVS as `logurl`, it overrides
   `FM_LOG_URL`; `log url default` goes back). For a bench server on the LAN,
   `log url http://192.168.1.42:3000` works too.
3. **WiFi networks.** Network 1 is the WiFi-update network (`wifi ssid` /
   `wifi pass`). Network 2 is for the volunteer's hotspot:
   `wifi2 ssid <name>`, `wifi2 pass <password>` (2.4 GHz, WPA2). Each window
   tries network 1, then network 2, 15 s each. `wifi` shows both.
4. `log send` uploads at once, and `log` shows the result.

### How it behaves

- **Upload windows:** the first about 60 s after boot, then every 5 min
  (`FM_LOG_PERIOD_MS`). A window joins WiFi, syncs the clock, sends
  `POST <server>/api/ingest` requests of at most 100 records and 16 KB until
  nothing is left or one fails, then switches WiFi off. Records are deleted
  from flash only after the server's `200` acknowledges them. A failed window
  keeps everything for the next one.
- **Signature:** every request carries `X-FM-Unit: <call sign>` and
  `X-FM-Signature`: HMAC-SHA256 of the exact body bytes with the fleet key, in
  lower-case hex.
- **Radio and UI keep working** during a window: the upload runs in its own
  task on the other CPU core. The unit does not light-sleep while it runs.
- **Not during WiFi update mode.** A window waits while update mode is open,
  and `ota` is refused while a window runs (`try again in a minute`).
- **Storage:** LittleFS on the `spiffs` partition (1.5 MB). Up to 6,144
  records (24 files of 256), never fewer than 5,888 after the oldest file is
  dropped. When the log is full the oldest file goes and its unsent records
  are added to `dropped`, a lifetime counter that every request reports.
  Records survive reboots and battery swaps. Each record is an append (never
  a rewrite), so flash wear stays low, and each has a CRC, so a record cut by
  a power loss, or a chunk that cannot be read, is skipped without harming
  the others. Flash writes (records, deleting sent files, the counters in
  NVS) wait until no relay or ACK is counting down, so they never shift the
  mesh timing a test measures.
- **Time:** the unit syncs by SNTP (`pool.ntp.org`, then `time.google.com`)
  in each window, or failing that from the server's reply. Until the first
  sync of a boot, records carry `epoch` 0 and the server dates them from the
  uptime. The clock restored after a power-off (saved hourly) is not trusted
  for records, because it can be hours behind. The V4 clock (responder
  expiry, forward only) is moved more carefully, since one bad time would
  expire responders for good: freely only when SNTP and the server's time
  over verified HTTPS agree; from one of them alone by at most 60 s plus 5%
  of the uptime per boot; never from the server's time over `http://` or
  unverified TLS.
- **HTTPS** checks the server certificate against the root bundle inside the
  Arduino core (136 roots; `*.vercel.app` chains to GTS Root R1 /
  GlobalSign). `-D FM_LOG_TLS_INSECURE=1` skips the check for a bench server
  with a self-signed certificate; never for the field.
- **Battery cost (estimate, not measured):** WiFi draws ~100 mA for the 10-30 s
  of a window, about 5-10 mA averaged over 5 minutes. More than the unit
  uses asleep, so field-test battery figures are not product figures.

The **Status screen** (B) shows one line: `LOG 12 waiting, sent 3m ago`
(`never sent`, `uploading...`, or `no key: no upload`).

### Serial commands

| Command | Does |
|---|---|
| `log` | records in flash and waiting, dropped, boot and next seq, last good upload and last try with its result, server, key, next window, clock, flash use, uploader stack and heap margins |
| `log send` | upload now (adds a fresh status record first) |
| `log url <base>` | set the server (`http://` or `https://`, no `/api/ingest`); `log url` shows it; `log url default` returns to `FM_LOG_URL` |
| `log clear` | delete every stored record (not counted as dropped); seq and boot keep counting |
| `wifi2 ssid <name>` / `wifi2 pass <password>` / `wifi2 forget` | the second network (the hotspot); the password is never printed |

### First bench test

1. Run the web app locally with `LOG_KEY` set to the same hex as `FM_LOG_KEY`,
   and the unit's call sign allowed.
2. Build with the key, flash, then on serial: `log url http://<PC IP>:3000`,
   `wifi2 ssid ...`, `wifi2 pass ...`, `log send`.
3. Serial shows `POST n records ... -> 200`; the web app lists the boot and
   status records. Send a text from another unit: an `rx` record appears after
   the next window.
4. Pull the battery mid-window, power up again: nothing is lost, nothing is
   stored twice (the server keeps each `(unit, seq)` once).
5. Check `log` for the uploader's stack and heap margins and note them here.
