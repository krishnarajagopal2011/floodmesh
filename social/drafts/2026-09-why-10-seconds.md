# Why 10 seconds only

Tags: #discussion
Channels: X, Instagram
Visual: photo or short clip of the unit's screen during voice-note playback,
next to a phone stopwatch showing the clip length. Nothing inside the case
visible.

## Facts

Internal only. The posts name no components, radio settings, packet sizes or
protocol details.

- Legal budget: India's G.S.R. 853(E) (2021) Table II, 2.5% duty cycle,
  about 90 seconds of transmission per hour, tracked and enforced by the
  firmware. README. (docs/architecture.md §1.1: not yet checked against the
  gazette text.)
- Measured airtime for a 10-second note: 2599 ms, against 2556 ms predicted.
  README Status section.
- Alarms: about 70 ms airtime, priority over voice. README traffic table.
  The posts do not say alarms are exempt from the budget: docs/architecture.md
  §1.1 flags that exemption as a legal risk with a proposed fix.
- 90 s / 2.6 s is about 34 notes an hour for one device alone. Arithmetic on
  the two figures above, not a measured number.

## X thread (each post under 280 characters, checked)

**1/** (244 chars)
#discussion

We are building an off-grid pager for floods. Why cap voice notes at 10
seconds?

Legal budget: 2.5% duty cycle, about 90 s of transmission per hour (India
G.S.R. 853(E), 2021). A 10-second note already measures 2.6 s on the
bench.

[photo: screen during playback, stopwatch alongside]

**2/** (165 chars)
At 2.6 s each, one device alone could send about 34 voice notes an hour
before hitting the legal cap. Neighbours share the same airtime, so the real
number is lower.

**3/** (237 chars)
Alarms are a fraction of a second and always go ahead of voice. A longer
voice note would eat the shared airtime fast.

Fewer, longer notes or more, shorter ones?

Open hardware, built in public: github.com/krishnarajagopal2011/floodmesh

## Instagram caption

#discussion

We are building an off-grid pager for floods. Why does the voice note stop
at 10 seconds?

It comes down to a legal airtime budget, not a design preference. In India
a device like this can transmit for about 2.5% of each hour, roughly 90
seconds. On the bench, one 10-second voice note already uses 2.6 seconds of
that.

Do the math and one device alone gets about 34 notes an hour before hitting
the cap, and neighbours share the same airtime. Alarms take a fraction of a
second and always go ahead of voice, because they have to get through.

Would you trade fewer, longer voice notes for more, shorter ones?

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh hops alarms and 10-second voice notes around buildings, for days on
common lithium cells.
Open hardware, built in public: link in bio.

#floodmesh #floodsafety #disastertech #offgrid #buildinpublic #chennai #makerindia
