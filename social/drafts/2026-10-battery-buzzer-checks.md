# Two problems found checking units before a test

Tags: #trials #feedback
Channels: X, Instagram reel
Visual: real footage of a unit's screen showing its battery reading next
to a multimeter on the same cell, and a clip of the two different buzzers
(the quiet one and a normal one) being triggered side by side. No
schematics, no rendered graphics.

## Facts

Internal only. The post text names no part numbers, pins or protocol
details; it says only what was found and fixed, in plain words.

- Unit D's screen showed about 2.85 V for a cell a multimeter measured at
  4.07 V (later recalibrated against 4.11 V): a steady undercount, close
  to 0.70x of the real voltage. firmware_v4/README.md, "Battery reading
  per board (4.3.2)"; commit 1e2b532, "Firmware V4 4.3.2: per-unit battery
  calibration (batt cal)".
- Fix: each unit now learns its own correct reading, checked against a
  multimeter once. The setting stays even after a factory reset, because
  it describes the board, not the user's settings. firmware_v4/README.md,
  same section; commit 1e2b532.
- Unit E's alarm beep was feeble: it uses a different kind of buzzer part
  from the other units, and the way it was driven only made it click, not
  sound. firmware_v4/README.md, "Buzzer type (4.3.3)"; commit e35bfe9,
  "Firmware V4 4.3.3: passive-buzzer mode; audible power-on beep".
- Fix: a setting that drives that kind of buzzer part properly, tried at
  different pitches and set to the loudest found (2700 Hz) for unit E.
  Still quieter than the other units' buzzers; swapping the part is the
  real fix. firmware_v4/README.md, same section.
- Why it matters: "[the alarm sound] is the one part that must work when
  everything else has failed." README.md, "Known work remaining".

## X thread (each post under 280 characters, checked)

**1/** (201 chars)
#trials #feedback

We are building an off-grid pager for floods. Checking each unit before
our last field test, we found two real problems.

One unit's battery looked almost dead on screen. It was not.

**2/** (187 chars)
Every board reads its own battery a little differently. One unit was
showing less than three volts when it actually had just over four. We
now let each unit learn its own correct reading.

**3/** (206 chars)
A second unit uses a different kind of buzzer part. Driven the normal way
it barely clicked. We found a setting that makes it louder, though still
quieter than the others; swapping the part is the real fix.

**4/** (175 chars)
The alarm sound is the one part that has to work when everything else has
failed. If you were trying a device like this, what would you want us to
check before you trusted it?

**5/** (151 chars)
Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
Built in public.

## Instagram reel script (25 s)

- 0 to 2 s, text on screen: "2 problems found. Both fixed."
- 2 to 9 s: a unit's screen showing its battery reading, a multimeter
  probe touching the same cell beside it. Text: "This battery looked
  almost dead. It was not."
- 9 to 15 s: the fixed unit's screen after the correction. Text: "Every
  board reads a little differently. Now each one learns its own."
- 15 to 21 s: the two buzzers triggered side by side (same clip, same
  distance from the camera), one clearly quieter. Text: "One alarm was
  barely audible. Now it is louder, still not perfect."
- 21 to 25 s: end card. Text: "The alarm has to work when everything else
  fails. Built in public."

## Instagram caption

#trials #feedback

We are building an off-grid pager for floods. Checking each unit before
our last test, we found two real problems. Here is what they were.

One unit's battery looked almost dead on its screen. It was not: every
board reads its own battery a little differently, and this one was
showing less than three volts for a cell that actually had just over
four. Each unit now learns its own correct reading, checked once against
a multimeter.

A second unit's alarm beep was barely audible. It uses a different kind
of buzzer part from the others, and the way it was driven only made it
click, not sound. We found a setting that makes it louder. It is still
quieter than the other units' alarms; swapping the part is the real fix.

The alarm sound is the one part that has to work when everything else has
failed. If you were trying a device like this, what would you want us to
check before you trusted it?

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh passes SOS alerts and short text messages between buildings with
no tower and no internet. It is designed to run for days on one charge.
Built in public.

#floodmesh #fieldtest #floodsafety #disastertech #offgrid #buildinpublic #chennai
