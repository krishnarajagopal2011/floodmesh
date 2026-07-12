<div align="center">

# FloodMesh

**An off-grid, community-owned emergency alert network for flood-prone neighbourhoods.**

*When the water rises and the cell towers go down, a street should still be able to say **help**.*

</div>

---

## The problem

In the 2015 Chennai floods, the thing that failed most people wasn't the water — it was the silence. Power went first, and with it went phones, Wi-Fi, and the mobile network everyone assumed would always be there. Families 200 metres apart had no way to tell each other *we're on the roof*, *she needs insulin*, *the ground floor is gone*.

FloodMesh is a small answer to that specific silence: cheap, battery-powered pager units — one per household — that talk to each other directly over long-range radio, with **no cell tower, no internet, and no central server**. The message hops from unit to unit, routing *around* flooded ground floors and concrete walls, until it reaches someone who can act.

It is not a replacement for the fire service or a printed phone tree. It is the layer underneath them — the one that keeps working on the one bad night every few years when everything else is dark.

---

## The vision — what the finished system does

*This is the complete FloodMesh, all layers built. Some of it works today; some is still ahead (see [What works today](#what-works-today-layer-1) below). Here's the whole picture.*

Imagine every household in a flood-prone colony has a small, weatherproof box that sits quietly in a drawer, charged, for years — and comes alive the night the river comes over the bank.

- **Press a button, the colony knows.** Four statuses — *Safe*, *Need medical*, *Water on ground floor*, *Need evacuation* — reach every unit in range. No phone, no signal, no app.
- **Messages route around obstructions.** A walkie-talkie is one hop; blocked by a building, it's blocked. FloodMesh relays House 4 → rooftop → House 19 even when no direct link exists. This is the whole reason it exists.
- **It works unattended and won't be missed.** Units sleep for weeks and *scream* — buzzer plus screen — the instant a message arrives. Nobody has to be watching.
- **Speak when a button isn't enough.** Record a few seconds of voice — *"Amma's diabetic, out of insulin, second floor, water's at the stairs"* — and the mesh carries the clip to the whole colony. The screen tells you honestly, before you speak, whether the channel can carry it right now.
- **Break through in a real emergency.** When it truly matters, a rescue call can seize the channel — briefly, and rate-limited so it can't be abused — pausing routine chatter so the important message gets through.
- **Colonies call each other for help.** Networks link colony to colony along the riverfront, so a colony with a boat or higher ground learns that the one next door needs it — without either colony's everyday traffic drowning the other.
- **Everyone can see who needs what.** Each unit holds a live roster of every house's last known status. One glance answers *who needs help, and where*.
- **It never lies to you.** No fake "message sent." A unit reports a real confirmation — *relayed by House 7* — or honestly says *no relay heard — move higher*.

That's the destination. Here's exactly where we are on the way there.

---

## What works today (Layer 1)

The firmware in this repository **now** is the foundation: the always-on status alarm mesh. It is a complete, useful system on its own — you can deploy a colony with just this.

- **Four-button status alerts** (*Safe · Need medical · Water on ground floor · Need evacuation*), broadcast to the whole colony.
- **Relaying around obstructions** with suppressed flooding — messages hop outward without a broadcast storm.
- **Unattended operation** — weeks of low-power sleep, with buzzer and screen alerts on incoming messages.
- **Honest relay-ACK** — real delivery confirmation, or an honest *no relay heard*.
- **Live colony roster** — every house's last known status, at a glance.
- **Authentication** — messages are signed with a pre-shared key so a spoofed evacuation call can't send people into moving water.

*(In the layered design below, this is **Layer 1 — the status alarm mesh**.)*

---

## Upcoming features

Designed and specified, **not yet built.** Full technical detail for each is in **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**. Every one of these yields to the Layer 1 alarm above — the alarm layer is never blocked, delayed, or degraded by anything here.

**Layer 2 — Recorded voice clips.** Short Codec2 voice messages relayed over the same mesh — the nuance a four-button alarm can't carry (*"she's diabetic, out of insulin, second floor, water's at the stairs"*). Live voice over LoRa isn't possible (bandwidth and duty-cycle rules), but a 3–5 second *recording* is just a small file the mesh can move. Best-effort; always yields to alarm traffic.

**Layer 2.5 — Voice availability indicator.** The screen shows **READY / BUSY / OFF** *before* the microphone ever opens, driven by live channel conditions. A voice feature that silently fails is worse than none — this makes sure the user knows whether voice will get through *before* they commit to speaking. It's the mechanism that stops the voice layer from crowding out the alarm layer during a crisis.

**Layer 3 — Emergency voice override.** When a status code isn't enough and the channel is busy, a deliberate two-step action broadcasts a self-expiring *channel-hold* so other nodes pause non-emergency traffic. Rate-limited per node on a rolling 60-minute window (at most two seizures per hour), with the remaining budget and next-free time shown on screen. Simultaneous seizures are tiebroken. **At zero budget the status alarm still works** — the limit caps channel *seizure*, never the ability to signal distress.

**Layer 4 — Inter-colony bridging.** The same relay engine plus a `colonyId` byte lets colonies link into a larger network. A designated bridge node acts as a *firewall, not a repeater*: routine local traffic stays local, and only high-priority alerts (*Rescue*/*Medical*) and explicitly-addressed messages cross between colonies — so one colony's chatter never drowns another's channel.

**Roadmap:** we earn each layer by proving the one before it. Phase order — prove the link (range walk) → harden Layer 1 → elevate & power (rooftop solar relays) → voice → emergency override → bridge colonies. A rock-solid single colony that has survived a real drill is worth more than a five-layer system that has never been rained on.

---

## Design principles

The rules we hold each other to. A change that breaks one needs a very good reason.

1. **The alarm layer is sacred.** The status alert is never rate-limited, never yields, never depends on any higher feature.
2. **Cheap and reliable beats clever and fragile.**
3. **Never lie to the user.** No unconfirmed success. If a feature is unavailable, show it *before* the user commits.
4. **Degrade gracefully.** A lost packet or dead relay is a small local loss, never a system-wide failure.
5. **Airtime is the scarcest resource.**
6. **The firmware is 30% of the outcome** — antennas, elevated relays, charged batteries, and a drilled human protocol are the other 70%.
7. **Assume it sits in a drawer for two years and must work once.**

---

## Architecture at a glance

Built in layers. Each is optional; each yields to the layer below. **Layer 1 alone is a complete, useful system.**

```
 Layer 4   Inter-colony bridge      selective, high-priority-only cross-links
 Layer 3   Emergency voice override channel seizure, rate-limited, self-expiring
 Layer 2   Recorded voice clips     short Codec2 messages, best-effort, yields
 Layer 1   Status alarm mesh        4-button alert, always-on, never yields  ← core
```

Full detail — packet formats, suppressed-flooding relay logic, honest relay-ACK, the voice availability state machine, the emergency-override rate limiter, and inter-colony bridging — is in **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

---

## Hardware

| Path | Board | Radio | Library | Notes |
|---|---|---|---|---|
| Recommended | Heltec WiFi LoRa 32 V3 (**863–928 MHz**) | SX1262 | RadioLib | ~half the RX current; onboard OLED, USB-C, charging |
| Reference | ESP32-WROOM-32E + module | SX1276 | sandeepmistry/LoRa | Cheapest; mind the strapping pins |

> ⚠️ **Band matters.** Use the **863–928 MHz** variant for India (865–867 MHz). The 433 MHz variant is the wrong band. Confirm current WPC rules for your region. **Never power a LoRa board without its antenna.**

Indicative cost: ~₹4,600 per household unit, ~₹7,000 per solar rooftop relay.

---

## Repository layout

```
/firmware
  /layer1-status      core mesh: packet, relay, dedup, roster, ACK  ← current
  /layer2-voice       (planned) Codec2 record/fragment/reassemble
  /layer3-override    (planned) channel-hold, seizure, rate-limit
  /layer4-bridge      (planned) colony bridging + firewall rules
/docs
  ARCHITECTURE.md     full vision + technical design
/hardware             (planned) schematics, pin maps, enclosure
/tools                (planned) range-walk RSSI logger
```

---

## Getting started

1. **Do a range walk first.** Two boards, the PING/link-survey mode, a walk through a real street → an RSSI coverage map. This decides spreading factor and relay placement. It's the most valuable afternoon in the project.
2. Flash `firmware/layer1-status/floodpager_v1.ino` (Arduino IDE / arduino-cli, ESP32 board package).
3. **Before flashing:** change the pre-shared key (see the warning below), and provision each unit with its node ID (hold two buttons at boot, follow the serial prompt).
4. Never power a unit without its antenna connected.

> 🔒 **You MUST change the pre-shared key before deploying.**
>
> The firmware ships with a **placeholder `PSK_KEY`** in `floodpager_v1.ino`. It is a public, well-known value — every copy of this repository has the same one. If you deploy without changing it, **your network is effectively unauthenticated**: anyone with the same firmware can inject or spoof messages, including a false *Need evacuation* that could send responders into moving water.
>
> Before flashing a single unit:
> 1. Generate your own random 16-byte key (e.g. `openssl rand -hex 16`).
> 2. Replace the `PSK_KEY` bytes in `floodpager_v1.ino`.
> 3. Flash **every unit in the colony with the same key** — units with mismatched keys cannot hear each other.
> 4. Keep the key private. Do **not** commit your real key back to this or any public repository. Share it only with the people provisioning units, and treat a re-key as a full re-flash of the colony.
>
> This is not optional hardening. On a life-safety network, an unauthenticated channel is a design failure.

---

## Contributing

We need embedded devs, RF/antenna people, hardware/PCB designers, Codec2/DSP folks, and — just as much — **field-testers, documentation writers, and Tamil translators.** A pull request with a field log beats a clever one without.

**Good first issues:** range-walk RSSI logger · per-status buzzer cadences · roster display · OLED voice-availability indicator · serial provisioning flow.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full contribution guide and roadmap.

---

## Scope, said plainly

FloodMesh is an **adjunct** to official disaster response and low-tech fallbacks — whistles, a muster point, a phone tree — not a replacement. Its value is the 200 metres around you when everyone's phone is at 4% and the towers are dark. We build it to be worth trusting on that night, and we're honest about what it cannot do.

---

## Licence

Code: **Apache-2.0** (proposed) · Documentation: **CC-BY-SA 4.0** (proposed) · Name & logo: trademark reserved.

*Permissive licensing so any flood-prone community can freely build on and deploy this. See ARCHITECTURE.md for the reasoning.*

*This project carries no warranty. Lives should never depend on it alone — but it should be built as though they might.*
