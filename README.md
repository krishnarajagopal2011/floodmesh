<div align="center">

# FloodMesh

**An off-grid, community-owned emergency alert network for flood-prone neighbourhoods.**

*When the water rises and the cell towers go down, a street should still be able to say **help**.*

</div>

---

## The problem

In the 2015 Chennai floods, the thing that failed most people wasn't the water — it was the silence. Power went first, and with it went phones, Wi-Fi, and the mobile network everyone assumed would always be there. Families 200 metres apart had no way to tell each other *we're on the roof*, *she needs insulin*, *the ground floor is gone*.

FloodMesh is a small answer to that specific silence: cheap, battery-powered pager units — one per household — that talk to each other directly over long-range radio, with **no cell tower, no internet, and no central server**. Press a button, and every house in the colony knows. The message hops from unit to unit, routing *around* flooded ground floors and concrete walls, until it reaches someone who can act.

It is not a replacement for the fire service or a printed phone tree. It is the layer underneath them — the one that keeps working on the one bad night every few years when everything else is dark.

---

## What it does

- **Press a button, the colony knows.** Four statuses: *Safe*, *Need medical*, *Water on ground floor*, *Need evacuation*.
- **Relays around obstructions.** A walkie-talkie is one hop; blocked, it's blocked. FloodMesh routes House 4 → rooftop → House 19 even when a direct link fails. This is the whole reason it exists.
- **Works unattended.** Units sleep for weeks and *scream* — buzzer plus screen — when a message arrives.
- **Tells the truth.** No fake "message sent." A unit listens for a neighbour re-broadcasting its packet and reports a real confirmation, or honestly says *no relay heard — move higher*.
- **Remembers.** Each unit holds a live roster of every house's last known status. One glance answers *who needs what, and where*.

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
3. **Before flashing:** change the pre-shared key, and provision each unit with its node ID (hold two buttons at boot, follow the serial prompt).
4. Never power a unit without its antenna connected.

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
