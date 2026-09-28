# Knowing if your call for help got through

Tags: #announcement #feedback
Channels: X, Instagram
Visual: a photo of the unit itself on the bench, real hardware. No rendered
graphics of the screen or its messages: this has not run on a unit yet, so
there is no real screen to film.

## Facts

Internal only. The post text names no radio settings, frames or protocol
details.

- A responder unit now sends back confirmation when it gets an SOS.
  docs/architecture.md §13.8; firmware_v4/README.md, "Delivery ACK and
  'Help is coming'" row; commit b18a92e.
- The sender retries up to 5 times, after 1, 2, 4, 8 and 15 minutes.
  firmware_v4/README.md, "SOS retries" row (§7.6); commit b18a92e.
- If the 5th try gets no answer within a minute, the unit beeps twice and
  shows "SOS NOT DELIVERED". If someone answers even after that, it still
  changes to "DELIVERED". Commit b18a92e message.
- Once delivered, the SOS is sent again every 15 minutes until a responder
  says help is coming, so it is not lost if the one responder who first
  heard it goes off air. Commit b18a92e, "Keep-alive".
- A responder answers by holding two keys together for 3 seconds on the SOS
  screen; the sender's screen then shows "HELP IS COMING". Commit b18a92e,
  "Responder reply".
- Status: this is written into the firmware (V4, version 4.1.0) and builds
  in CI, but has not yet run on a real unit. firmware_v4/README.md,
  "Status (2026-09-28, 4.1.0): compiles in CI; not yet run on a unit."

## X thread (each post under 280 characters, checked)

**1/** (159 chars)
#announcement #feedback

We are building an off-grid pager for floods. It can now tell you if a call
for help actually got through, not just that it was sent.

[photo: unit on the bench]

**2/** (167 chars)
If nobody answers after 5 tries over 15 minutes, the pager beeps twice and
shows SOS NOT DELIVERED. If someone answers even after that, it still
changes to DELIVERED.

**3/** (166 chars)
Once delivered, it keeps sending every 15 minutes until a responder says
help is coming. That way one SOS is not lost if the responder who first
heard it goes quiet.

**4/** (87 chars)
This is written into the software but not yet tried on a real unit. That
test is next.

**5/** (132 chars)
If your pager could only tell you one thing about a message sent in an
emergency, what should it be: sent, seen, or help is coming?

**6/** (152 chars)
Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
Built in public.

## Instagram caption

#announcement #feedback

We are building an off-grid pager for floods. Here is a new piece: knowing
if your call for help actually got through.

Until now, sending an SOS meant hoping. Now, if nobody answers after 5
tries over 15 minutes, the pager beeps twice and shows SOS NOT DELIVERED so
you know to try something else. If someone does answer, even after that,
it changes to DELIVERED. And once it is delivered, the pager keeps checking
in every 15 minutes until a responder says help is coming, so the message
does not get lost if that one responder loses signal.

This is written into the software but has not run on a real unit yet.
That test is next, and I will post how it goes.

If your pager could only tell you one thing about a message sent in an
emergency, what should it be: sent, seen, or help is coming?

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh passes alarms and 10-second voice notes between buildings with no
tower and no internet. It is designed to run for days on ordinary batteries.
Built in public.

#floodmesh #floodsafety #disastertech #offgrid #buildinpublic #chennai #makerindia #disastermanagement
