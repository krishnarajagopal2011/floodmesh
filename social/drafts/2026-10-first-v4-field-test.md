# First field test of the new firmware: four words sent, zero arrived

Tags: #trials #feedback
Channels: X, Instagram reel
Visual: real footage from the 28 September test if any exists beyond the
WhatsApp screenshots used for the readout (check before shooting new
footage); otherwise re-shoot at the same spots: the unit kept indoors at
the house, the unit on the roof of that house, and the sender's unit about
260 m away. The test map (docs/field-tests/2026-09-28-map.png) is a
rendered graphic: background context only, never the main shot, and never
used in place of real footage for the reel.

## Facts

Internal only. The post text names no radio settings, dBm or protocol
rules; it says only what happened and why in plain words.

- Test on 28 September 2026, the first field test of firmware V4 (4.2.0).
  Four units: one person in trouble, fixed indoors at home; one person
  walking around; one unit on the roof of the same house as the person
  indoors; one responder sending test messages. docs/field-tests.md,
  "2026-09-28: first V4 test, roof unit", Setup table.
- The responder sent four short test words from about 259-270 m west of
  the house. docs/field-tests.md, "Field readings from the WhatsApp
  photos".
- The person indoors received none of the four words. docs/field-tests.md,
  "Geometry and outcome": "A received none of E's four words."
- A second unit, about 57 m from the house and almost on the line between
  the sender and the house, heard both sides well but did not relay the
  words. docs/field-tests.md, "What the photos show" and "Geometry and
  outcome".
- Why it held back: it had heard a signal from the indoor unit that made
  it believe the indoor unit was already running on mains power, so the
  rule meant for a genuinely powered relay told it to leave the job to the
  indoor unit itself. docs/field-tests.md, "Interpretation" (the §13.3
  forwarding rule combined with false power detection).
- The roof unit, on the same house as the person indoors, could also have
  delivered the words a few metres away, but it was in battery-saving
  sleep 85% of the test (not set to stay awake) and had heard the same
  signal, so it held back too. docs/field-tests.md, "Interpretation": "C
  did not work as a roof tower: 85% asleep."
- The indoor unit has no hardware connection yet to tell it whether it is
  really plugged into power, so it guesses from software alone, and
  guessed wrong. docs/field-tests.md, "Interpretation"; README.md "Known
  work remaining".
- Changes already made: firmware V4 4.3.1 saves whether a unit should stay
  fully awake across restarts, and the roof unit is now set to stay on
  that way. docs/field-tests.md, "Changes made after the test".
- Still to do: wire the actual power-sensing connection on the units that
  lack it. docs/field-tests.md, "Changes made after the test" (Hardware to
  do); README.md "Known work remaining".

## X thread (each post under 280 characters, checked)

**1/** (237 chars)
#trials #feedback

We are building an off-grid pager for floods. First field test of the
newest firmware: four units, a real house, a real failure.

Four test words sent from about 260 m away. The person waiting indoors
got none of them.

[clip: sender's unit, about 260 m from the house]

**2/** (210 chars)
Two other units nearby could have passed the words to her. Neither did.

Both had picked up a signal that made them think she already had a
strong, direct line to mains power, so they left it to her unit alone.

**3/** (210 chars)
Her unit has no wire yet to tell it whether it is really plugged in. It
guessed, and guessed wrong. This is the exact situation we are building
for: someone stuck indoors, a helper nearby making the wrong call.

**4/** (251 chars)
Already fixed: units now remember to stay fully awake, and the roof unit
is set to stay on. Still missing: the wire that tells a unit the truth
about its own power.

If your first message did not get through, what would tell you help was
still coming?

**5/** (151 chars)
Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
Built in public.

## Instagram reel script (30 s)

- 0 to 2 s, text on screen: "First field test. 4 words sent. 0 arrived."
- 2 to 8 s: sender's unit, street or open ground in view, about 260 m from
  the house. Text: "Sent from about 260 m away."
- 8 to 16 s: the house, with the indoor unit and the roof unit shown (not
  labelled, just the two units near each other). Text: "Two units nearby.
  Neither passed the message on."
- 16 to 23 s: map graphic (docs/field-tests/2026-09-28-map.png) as
  background only, with real footage of a unit screen on top. Text: "Both
  thought she was already on mains power. She wasn't."
- 23 to 30 s: end card, text: "Fixed: units remember to stay awake now.
  Still to do: the real power wire. Built in public."

## Instagram caption

#trials #feedback

We are building an off-grid pager for floods. First field test of the
newest firmware: a message that did not arrive.

Four units, a real house: one person waiting indoors, one unit on the
roof of that house, one person walking nearby, and one responder sending
four short test words from about 260 metres away.

The person waiting indoors got none of the four words.

Two other units close by could have passed the words on for her. Neither
did. Both had picked up a signal that made them believe she already had a
strong, direct line to mains power, so they left the job to her unit
alone. Her unit has no wire yet to tell it whether it is really plugged
in. It guessed, and guessed wrong.

This is close to the real situation FloodMesh is built for: someone stuck
indoors on a low floor, and a helper nearby silently making the wrong
call. We have already fixed one part: the units now remember whether to
stay fully awake, and we have set the roof unit to stay on. The part still
missing is the wire that tells a unit the truth about its own power.

If your first message did not get through, what would tell you help was
still coming?

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh passes SOS alerts and short text messages between buildings with
no tower and no internet. It is designed to run for days on one charge.
Built in public.

#floodmesh #fieldtest #floodsafety #disastertech #offgrid #buildinpublic #chennai
