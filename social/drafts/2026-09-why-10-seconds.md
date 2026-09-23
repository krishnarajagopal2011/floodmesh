# Why 10 seconds only

Tags: #discussion
Channels: X, Instagram
Visual: screen recording or photo of the OLED mid-playback of a voice note,
next to a phone stopwatch or timer showing the clip length.

## Facts

- Layer 1 alarms: 22 bytes, ~70 ms airtime, never throttled by the duty-cycle
  budget (deliberate life-safety choice). README traffic table.
- Layer 2 voice: 1500 bytes, 8 fragments, ~2.6 s airtime, refused when the
  duty-cycle budget is spent. README traffic table.
- Measured airtime for a 10-second note: 2599 ms, against 2556 ms predicted.
  README Status section.
- Legal budget: India's G.S.R. 853(E) (2021) Table II caps this device class
  at 200 kHz / 500 mW e.r.p. with a 2.5% duty cycle, about 90 seconds of
  transmission per hour, which the firmware tracks and enforces. README.
- 90 s budget / 2.6 s per 10-second clip is about 34 clips an hour for one
  device transmitting alone. Arithmetic on the two figures above, not a
  measured or repo-stated number.

## X thread (each post under 280 characters, checked)

**1/** (244 chars)
#discussion

We are building an off-grid pager for floods. Why cap voice notes at 10
seconds?

Legal budget: 2.5% duty cycle, about 90 s of transmission per hour (India
G.S.R. 853(E), 2021). A 10-second note already measures 2.6 s on the
bench.

[photo: OLED mid-playback, timer alongside]

**2/** (219 chars)
10 s of speech becomes 1500 bytes over 8 LoRa fragments. At 2.6 s of
airtime each, one device alone could send about 34 of these an hour before
hitting the legal cap. Sharing a channel with neighbours cuts that further.

**3/** (263 chars)
Status alarms sit outside that budget on purpose: 22 bytes, about 70 ms,
never throttled. A longer voice note would eat the shared airtime fast.

Fewer, longer notes or more, shorter ones?

Open hardware, built in public: github.com/krishnarajagopal2011/floodmesh

## Instagram caption

#discussion

We are building an off-grid pager for floods. Why does the voice note stop
at 10 seconds?

It comes down to a legal airtime budget, not a design preference. The radio
can transmit for about 2.5% of each hour, roughly 90 seconds, under India's
G.S.R. 853(E) (2021) rules for this device class. On the bench, one
10-second voice note already measures 2.6 seconds of that.

Do the math and one device alone gets about 34 of these an hour before
hitting the cap, before any neighbour also wants to talk. Status alarms sit
outside that budget on purpose, 22 bytes and about 70 ms each, because they
have to get through no matter what.

Would you trade fewer, longer voice notes for more, shorter ones?

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh hops alarms and 10-second voice notes around buildings, for days on
common lithium cells.
Open hardware, built in public: link in bio.

#floodmesh #lora #dutycycle #meshnetwork #disastertech #openhardware #esp32 #buildinpublic
