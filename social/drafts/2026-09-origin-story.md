# The starting story

Tags: #story
Channels: X thread, Instagram carousel, LinkedIn
Visual: photo of the perfboard prototype in hand, OLED lit. Carousel: board,
keypad close-up, a Chennai 2015 flood photo you hold the rights to, the four
alarm labels.

## Facts

- Chennai 2015 and Cyclone Michaung: towers and power failed together. README intro.
- Pager, not walkie-talkie; store-and-forward routes around wet concrete. README.
- Alarms 22 bytes, authenticated, never throttled. Voice 1500 bytes, one 10 s note. README traffic table.
- Verified on hardware 9 September 2026: two-node link, authenticated alarms, 8 of 8 fragments. README Status.
- Range and through-concrete performance untested as of README. First 350 m test done since.

## X thread

**1/**
#story

We are building an off-grid pager for floods. This is why.

In Chennai 2015 and again during Cyclone Michaung, the towers and the power
went down together. People 200 m apart could not tell each other they were
alive.

[photo: prototype in hand]

**2/**
Phones need a tower. Walkie-talkies need line of sight, and wet reinforced
concrete eats radio.

So this is a pager, not a walkie-talkie. Messages are stored and forwarded,
building to building, and the radio is idle most of the time.

**3/**
Two kinds of traffic:

Alarms: SAFE, NEED MEDICAL, WATER GROUND FLOOR, NEED EVACUATION. 22 bytes,
authenticated, never throttled.

Voice: one 10-second note, 1500 bytes, best effort.

**4/**
Runs for days on common lithium cells. Built on a Heltec LoRa 32 and a
handful of parts from Ritchie Street.

Verified on the bench on 9 Sept 2026: two-node link, authenticated alarms,
8 of 8 voice fragments reassembled. Range work has just started.

**5/**
Everything is open and I am building it in public because a safety device
nobody has kicked is not safe.

Why: towers and power fail together in floods.
FloodMesh hops alarms and 10-second voice notes around buildings, for days on
common lithium cells.
Open hardware: github.com/krishnarajagopal2011/floodmesh

## Instagram caption

#story

We are building an off-grid pager for floods. Here is the starting story.

In Chennai 2015, and again during Cyclone Michaung, the mobile towers and
the power went down together. Families two hundred metres apart had no way
to say "we are okay" or "we need a boat".

FloodMesh is a handheld that passes short alarms and 10-second voice notes
from building to building without any network. It is a pager, not a
walkie-talkie: messages are stored and forwarded, so they route around wet
concrete instead of needing line of sight.

Four alarms: SAFE, NEED MEDICAL, WATER GROUND FLOOR, NEED EVACUATION.
Four channels: Medical, Rescue Boats, Volunteers, Supplies.

It works on the bench as of 9 September 2026. Range and through-wall tests
are just starting, and every result gets posted, good or bad.

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh hops alarms and 10-second voice notes around buildings, for days on
common lithium cells.
Open hardware, built in public: link in bio.

#floodmesh #lora #meshnetwork #chennaifloods #disastertech #openhardware #esp32 #buildinpublic #makerindia #floodsafety

## LinkedIn

I am building an off-grid emergency pager for urban floods, in public, and I
would like your help kicking it.

In Chennai 2015 and during Cyclone Michaung, the towers and the power failed
together. Communities a couple of hundred metres apart were cut off from
each other completely. Rescue teams could not hear who needed a boat.

FloodMesh is a handheld that hops authenticated distress alarms and short
voice notes from building to building on LoRa radio, with no tower and no
internet, for days on common lithium cells. Store-and-forward, so it routes
around wet reinforced concrete instead of needing line of sight.

Where it stands on 9 September 2026: verified on the bench, two-node link,
authenticated alarms, full voice notes reassembled. Range testing has begun.
Enclosure, pilot units and regulatory work are next.

Who I want to hear from:
- Disaster management and volunteer network people: which four alarms would
  you actually want on the buttons?
- Anyone in a flood-prone colony in Chennai willing to host a pilot this
  season.
- Hardware and RF people who can tell me what I have got wrong.

Everything is open: github.com/krishnarajagopal2011/floodmesh

#disastermanagement #openhardware #chennai
