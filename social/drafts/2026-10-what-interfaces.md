# What interfaces do you need?

Tags: #discussion #feedback
Channels: X, Instagram
Visual: photo of a unit on the bench showing the keypad and the screen
together, screen lit with a normal status view (no SOS or message content
visible).

## Facts

Internal only. The post text names no pins, chips, protocol details or
radio settings.

- Input is a 16-key pad (1-9, 0, *, #, and four more keys), wired straight
  to the board. firmware_v4/README.md ("Exactly as V3. The keypad goes
  straight to the Heltec GPIOs"); CLAUDE.md firmware_v3 description.
- A small screen shows status, messages and the SOS list.
  firmware_v4/README.md, Home/SOS list behaviour (lines ~191-195); root
  README.md hardware table (0.96" OLED).
- SOS is sent by holding two keys together for three seconds, then picking
  the kind of help needed from a short list. firmware_v4/README.md,
  "Anywhere | hold * and # together 3 s | SOS: the channel screen opens";
  "SOS channel screen | 1 Medical, 2 Evacuation, 3 Hazard, 4 Food supply,
  0 General".
- Short text messages, up to 60 characters, are typed on the same keypad
  the way you type on an old phone (multi-tap). CLAUDE.md, Firmware V3
  description ("60-char T9/multi-tap texts"); docs/architecture.md
  architecture-in-one-paragraph summary.
- There is no voice in this version. docs/architecture.md §8 (voice
  removed); CLAUDE.md architecture summary ("no voice in v1").

## X thread (each post under 280 characters, checked)

**1/** (235 chars)
#discussion #feedback

We are building an off-grid pager for floods. Right now you talk to it
with a small 16-key pad, a screen, and one way to send an SOS: hold two
keys together for three seconds, then pick the kind of help you need.

[photo: unit on the bench, keypad and screen visible]

**2/** (138 chars)
You can also type a short message on the same keypad, up to 60
characters, the way you type on an old phone. No voice in this version
yet.

**3/** (127 chars)
What would make this easier to use in a real flood: bigger keys, a way to
use it without looking at the screen, something else?

**4/** (151 chars)
Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
Built in public.

## Instagram caption

#discussion #feedback

We are building an off-grid pager for floods. Right now you talk to it
with a small 16-key pad, a screen, and one way to send an SOS: hold two
keys together for three seconds, then pick the kind of help you need.

You can also type a short message on the same keypad, the way you type on
an old phone, up to 60 characters. There is no voice in this version yet.

What would make this easier to use in a real flood: bigger keys, a way to
use it without looking at the screen, something else?

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh passes SOS alerts and short text messages between buildings with
no tower and no internet. It is designed to run for days on one charge.
Built in public.

#floodmesh #floodsafety #disastertech #offgrid #buildinpublic #chennai #accessibility
