# FloodMesh

**An off-grid, community-owned emergency alert network for flood-prone neighbourhoods.**

When the water rises and the cell towers go down, a street should still be able to say *help*.

---

## Why this exists

In the 2015 Chennai floods, the thing that failed most people wasn't the water — it was the silence. Power went first, and with it went phones, Wi-Fi routers, and the mobile network that everyone assumed would always be there. Families 200 metres apart had no way to tell each other *we're on the roof*, *she needs insulin*, *the ground floor is gone*. Rescue was slow not because help didn't exist, but because no one could point it to the right door.

FloodMesh is a small answer to that specific silence. It is a network of cheap, battery-powered pager units — one per household — that talk to each other directly over long-range radio, with **no cell tower, no internet, and no central server**. Press a button, and every house in the colony knows. The message hops from unit to unit, routing *around* the flooded ground floors and concrete walls that would stop a single radio, until it reaches someone who can act.

It is not a replacement for the fire service, the disaster management authority, or a printed phone tree. It is the layer underneath all of those — the one that keeps working on the one bad night every few years when everything else is dark.

**Our design promise:** the thing that saves a life must be the thing that never fails. Every feature in this project is ranked against that. The cheap, boring, always-works alarm comes first. Everything clever yields to it.

---

## What it does, in one screen

- **Press a button, the colony knows.** Four buttons: *Safe*, *Need medical*, *Water on ground floor*, *Need evacuation*. The message reaches every unit in range and hops onward.
- **It relays around obstructions.** A single walkie-talkie is one hop — blocked by a building, it's blocked. FloodMesh routes House 4 → rooftop → House 19 even when House 4 can't reach House 19 directly. This is the whole reason it exists.
- **It works unattended.** Units sit in months-long low-power sleep and *scream* — buzzer plus screen — when a message arrives. No one has to be watching.
- **It tells the truth.** No fake "message sent." A unit listens for a neighbour re-broadcasting its packet and reports a real confirmation, or honestly says *no relay heard — move higher*.
- **It remembers.** Every unit holds a live roster: the last known status of every house it has heard. One glance answers *who needs what, and where*.

---

## Who we're looking for

This is a small, high-stakes project where one good contributor moves the whole thing forward. You don't need all of these skills — you need one of them and the willingness to test in the rain.

- **Embedded / firmware (C++, ESP32, RadioLib).** The core. Radio driver, mesh protocol, power management, the state machines below.
- **RF / antenna people.** Link budgets, antenna selection, the rooftop relay placement that matters more than any line of code.
- **Hardware / PCB.** Moving from dev-board-in-a-box to a rugged, waterproof, field-serviceable unit.
- **Codec / DSP.** The v2 voice layer — Codec2 encode/decode on the ESP32-S3, fragmentation, reassembly.
- **Mobile / UX.** Optional companion app for provisioning and richer message composition.
- **Documentation, translation (Tamil first), and field-testing.** Not secondary. A system nobody can set up or maintain is a system that fails when it matters. Field-testers who will actually walk a street with two units are worth their weight in gold.
- **Deployment & community organisers.** The 70% of this project that isn't firmware: batteries, drills, laminated cards, the human protocol.

If you've run Meshtastic, built a LoRa link, or just care about the problem — you're who we mean.

---

## Design principles

These are the rules we hold each other to in review. A pull request that violates one needs a very good reason.

1. **The alarm layer is sacred.** The four-button status alert is never rate-limited, never yields, and never depends on any higher-level feature working. Everything else in the system can fail and the alarm still goes through.
2. **Cheap and reliable beats clever and fragile.** Given a choice, we ship the boring thing that works in the wet at 3 a.m.
3. **Never lie to the user.** No unconfirmed "success" messages. If we can't prove delivery, we say so. If a feature is unavailable, we show it *before* the user commits.
4. **Degrade gracefully.** A lost packet, a dead relay, a node that missed a control message — each should cause a small local loss, never a system-wide failure.
5. **Airtime is the scarcest resource.** The radio channel is shared and finite. Every feature is judged partly on what it costs the channel, especially during a crisis when traffic peaks.
6. **The firmware is 30% of the outcome.** Antennas, elevated relays, charged batteries, and a drilled human protocol are the other 70%. We design and document for all of it.
7. **Assume it sits in a drawer for two years and must work once.** Every decision — battery chemistry, solder joints, power switch, storage protocol — is made against that reality.

---

## System architecture

FloodMesh is built in **layers**, each of which is optional and each of which yields to the layer below it. You can deploy Layer 1 alone and have a complete, useful system. The higher layers are upgrades, not dependencies.

```
 Layer 4   Inter-colony bridge      selective, high-priority-only cross-links
 Layer 3   Emergency voice override channel seizure, rate-limited, self-expiring
 Layer 2   Recorded voice clips     short Codec2 messages, best-effort, yields
 Layer 1   Status alarm mesh        4-button alert, always-on, never yields  ← core
 ─────────────────────────────────────────────────────────────────────────────
 Hardware  ESP32 + SX127x/SX1262 LoRa node, buzzer, OLED, buttons, 18650
```

### Layer 1 — the status alarm mesh (the core)

The foundation. A packed 12-byte packet carrying sender, sequence number, status code, and hop count, broadcast over peer-to-peer LoRa. Key mechanisms:

- **Suppressed flooding.** A naive "every node relays every packet" design melts the channel (a broadcast storm). Instead, each relay is scheduled after a backoff *biased by signal strength* — nodes that heard the packet weakly are farther away, so they relay sooner, pushing the message outward instead of sideways. If a node hears the same packet re-broadcast by two others while it waits, it cancels its own relay. This cuts redundant transmissions by more than half with no loss of coverage.
- **Priority-based hop limits.** *Rescue* and *Medical* get up to 3 hops. *Safe* gets 1 hop and a rate limit — twenty people pressing the reassuring green button must never crowd out a rescue call.
- **Honest relay-ACK.** LoRa broadcast has no acknowledgement. So after transmitting, a unit listens for any neighbour re-broadcasting its own packet. Hearing it is proof a real node received it — the only honest confirmation a broadcast can give. The screen shows *relayed by House 7, −84 dBm*, or *no relay heard*.
- **Deduplication.** A rolling cache of recently-seen `(origin, sequence)` pairs, so a packet is never processed or relayed twice.
- **Listen-before-talk.** Every transmission checks the channel is clear first, to reduce collisions.
- **Colony roster.** Each unit caches the last status of every node it has heard. A long-press shows the whole neighbourhood at a glance.
- **Message authentication.** A pre-shared key and a truncated AES-CMAC over the immutable packet fields. A spoofed or accidental *Need evacuation* from a house that's fine could send responders into moving water — so packets are authenticated, and replays are stopped by the sequence number.

### Layer 2 — recorded voice clips

Live voice over LoRa is a physics problem we don't try to solve — the bandwidth isn't there and the duty-cycle rules forbid it. But a *recording* is just a small file, and LoRa moves small files fine.

- A 3–5 second clip, compressed with **Codec2** at its lowest bitrate, is a few hundred bytes — around 8 fragments over the air.
- The clip is captured on an added I2S MEMS microphone, encoded on the ESP32-S3, fragmented into authenticated chunks (`msgId`, `chunkIndex`, `totalChunks`, payload, CRC), and broadcast. The existing relay, dedup, and hop logic carry the chunks unchanged. Receivers reassemble by `msgId` and play the clip through a small speaker.
- Fidelity is rough but intelligible — *"Amma's diabetic, out of insulin, second floor"* comes through, which is the entire point. This is the nuance a four-button pager structurally cannot carry.

**Voice always yields to the alarm layer.** A voice send in flight pauses for an incoming *Rescue* packet and resumes when the channel clears.

### Layer 2.5 — the voice availability indicator

A feature that silently fails is worse than no feature — the user speaks, believes it sent, and walks away. So voice availability is shown **before** the microphone ever opens, driven by live channel conditions:

- **READY** — channel quiet, no priority traffic, buffers free.
- **BUSY — wait** — channel congested or local send queue not drained. Recording is *blocked*.
- **OFF — alarms only** — heavy traffic or an active rescue in the mesh; voice fully suspended so it can't steal airtime from life-safety packets.

This is a safety mechanism wearing a UI hat: it is the thing that stops the voice layer from crowding out the alarm layer during exactly the crisis the system exists for.

### Layer 3 — emergency voice override (priority preemption)

When a status code genuinely isn't enough and the channel is busy, a user can invoke an emergency seizure (a deliberate two-step action, so it can't fire by accident):

- The node broadcasts a small **CHANNEL-HOLD** control packet: *emergency voice incoming, hold N seconds*. Other nodes suppress their own non-emergency traffic for that window. Alarm-status packets still pass — nothing outranks a life-safety alert.
- The hold is **advisory and self-expiring**: a node that never heard it simply keeps working, so a lost control packet degrades gracefully rather than freezing the mesh.
- **Rate-limited, per node, on a rolling 60-minute window** — at most two seizures in any trailing hour. Per-node by construction (the counter lives in each unit's own memory), so no node and no colony can exhaust anyone else's budget. The remaining budget and the exact time the next slot frees are shown on the screen.
- **Simultaneous seizures** are tiebroken (lowest node-ID wins; the other backs off a few seconds and shows *emergency channel busy — you're next*).
- **At zero budget, the status alarm still works.** The limit caps channel *seizure*, never the ability to signal distress. The screen says so: *emergency voice used up — press Rescue.*

### Layer 4 — inter-colony bridging

The same relay engine plus a `colonyId` byte lets colonies link into a larger network — a chain along a riverfront, a star to a central high point, or a mesh of bridges.

- A designated **bridge node** (ideally an elevated relay with a directional antenna toward its twin in the next colony) links two colonies.
- The bridge is a **firewall, not a repeater.** Local traffic stays local — routine status and voice never cross. Only *Rescue*/*Medical* alerts and explicitly-addressed messages bridge. This is what stops one colony's chatter from drowning another's channel as the network grows.
- Dedup keys extend to `(colonyId, origin, sequence)`; bridged packets get a small extra hop budget for the cross-link.

---

## Hardware

FloodMesh runs on commodity LoRa development boards. Two supported paths:

**Reference / low-cost path — ESP32-WROOM-32E + SX1276**
The original bill of materials. Requires care with strapping pins (GPIO12 and GPIO2 must be avoided for buttons and DIO0 respectively — the wrong choice bricks the boot). Uses the Sandeep Mistry `LoRa` library. Good for learning and cheapest per unit.

**Recommended path — Heltec WiFi LoRa 32 V3 (863–928 MHz) — ESP32-S3 + SX1262**
Integrated board: onboard OLED, USB-C, Li-ion charge/discharge management, RF shielding. The SX1262 draws roughly half the receive current of the SX1276 (~4.6 mA vs ~11 mA) — the single biggest win in the whole power budget. The ESP32-S3 supports the deep-sleep wake mode our buttons need without extra hardware, and its extra RAM enables the Layer 2 voice work. **Requires RadioLib** (the Mistry library is SX127x-only). Board traps to respect: TCXO on DIO3 at 1.8 V, DIO2 as RF switch, Vext gating for the OLED, IPEX/U.FL antenna only.

> ⚠️ **Band matters.** The 863–928 MHz variant is the correct one for India (865–867 MHz, ~1 W ERP allowance). The 433 MHz variant is *not* — India's 433 allowance is far lower power and the band is congested. Confirm current WPC rules for your region before deploying. **Never power a LoRa board without its antenna connected.**

**Added components (all paths):** a piezo buzzer (a silent pager is useless in a dark, flooded house), four tactile buttons, and for Layer 2, an I2S MEMS microphone and I2S amplifier + speaker.

**Power & storage:** a single 18650 Li-ion (or LiFePO4 for better thermal safety and shelf life in a hot climate), a physical power switch, and silica gel in a sealed IP65 enclosure. Rooftop relay nodes add a small solar panel and charge controller.

**Indicative cost:** ~₹4,600 per household unit, ~₹7,000 per solar rooftop relay. A colony of 20 households with 3 relays lands around ₹1.25 lakh. Bulk-ordering boards directly cuts this meaningfully.

---

## Repository layout (proposed)

```
/firmware
  /layer1-status      core mesh: packet, relay, dedup, roster, ACK
  /layer2-voice       Codec2 record/encode/fragment/reassemble/play
  /layer3-override    channel-hold, seizure, rolling rate-limit
  /layer4-bridge      colony bridging + firewall rules
  /hal                board abstraction: SX1276 (Mistry) and SX1262 (RadioLib)
/hardware
  /reference-esp32    schematic, pin map, BOM
  /heltec-v3          schematic, pin map, BOM
  /enclosure          3D-print files, waterproofing notes
/docs
  /protocol           packet formats, state machines, airtime tables
  /deployment         range-walk guide, relay placement, drill protocol
  /field-reports      real coverage maps and test logs from deployments
/tools
  /rangewalk          PING/link-survey mode + RSSI logging
```

---

## How to contribute

1. **Start with a range walk.** The most valuable first contribution isn't code — it's evidence. Two boards, our PING/link-survey mode, and a walk through a real neighbourhood produces an RSSI coverage map. That map decides spreading factor, relay placement, and dead spots. It's an afternoon that's worth more than a week of design.
2. **Pick a layer.** Layer 1 is where the project lives or dies — hardening it is never wasted work. The higher layers are where the interesting problems are.
3. **Test in adverse conditions.** Bench tests lie. We value a pull request with a field log over a clever one without.
4. **Respect the design principles.** Especially: the alarm layer is sacred, and never lie to the user. Reviews check for these first.
5. **Document as you go.** Every board trap you hit, every antenna that surprised you, every drill that revealed a gap — write it down in `/docs/field-reports`. The human knowledge is as important as the code.

**Good first issues:** the range-walk RSSI logger; per-status buzzer cadences; the roster display; the OLED voice-availability indicator; a serial provisioning flow so one binary serves every node.

---

## Roadmap

- **Phase 0 — Prove the link.** Two nodes, a range walk, a coverage map of one real street. *Do not skip this.*
- **Phase 1 — Harden Layer 1.** Rock-solid status alarm: suppressed flooding, relay-ACK, buzzer, roster, provisioning. Run a real drill in one colony.
- **Phase 2 — Elevate and power.** Rooftop relays, solar, RadioLib + receive duty-cycling for multi-week battery life.
- **Phase 3 — Add voice (Layer 2 + 2.5).** Codec2 clips, fragmentation, the availability indicator.
- **Phase 4 — Emergency override (Layer 3).** Preemption, tiebreak, rolling rate-limits.
- **Phase 5 — Bridge colonies (Layer 4).** Selective inter-colony links, tested one bridge at a time.

We earn each phase by proving the one before it. A rock-solid single colony that has survived a drill is worth more than a five-layer system that has never been rained on.

---

## A note on scope, said plainly

FloodMesh is an **adjunct** to official disaster response and low-tech fallbacks — whistles, a pre-agreed muster point, a printed phone tree — not a replacement for any of them. Its real value is the 200 metres around you when everyone's phone is at 4% and the towers are dark. We build it to be worth trusting on that night, and we are honest about everything it cannot do.

If that mission resonates, we'd be glad to have you.

---

## Licence

*(To be decided by the founding team — a permissive open-source licence such as MIT or Apache-2.0 is suggested, to maximise adoption and let other flood-prone communities freely build on this.)*

---

*FloodMesh is a community project. It carries no warranty. Lives should never depend on it alone — but it should be built as though they might.*
