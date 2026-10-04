# LinkedIn weekly update, week ending 28 September 2026

Channels: LinkedIn only. Queue for Tuesday 29 or Wednesday 30 September,
morning.
Visual: one photo from this week. The sender or relay unit from the
26 September test, at road level, or the unit on the bench. No schematics.

## Facts

- Relay test, 26 September: alarm reached 350 m direct, 550 m total
  through one relay at road level (300 m to the relay, 250 m more to the
  responder). docs/field-tests.md.
- Delivery confirmation decided and written into firmware V4 (4.1.0),
  28 September: up to 5 retries over 15 minutes, "SOS NOT DELIVERED" if
  none answer, "DELIVERED" the moment one does, re-sent every 15 minutes
  after that until a responder answers. Not yet run on a real unit.
  docs/architecture.md §13.8; firmware_v4/README.md; commit b18a92e.
- First version: SOS alarms and short text messages. Decided 27 September.
  docs/architecture.md §7 and §13.
- Priced BoM moved to Indian and Chinese suppliers as the cheaper source
  over US distributors. Decided 27 September. docs/hardware/pcb-v1/README.md.
- Not measured: signal strength during the relay test, so the cause of the
  last hop's failure past 250 m is still open. docs/field-tests.md,
  "Not measured".

## Post

One relay carried an alarm past where a single pager can reach, and the
pager can now tell you when help is actually on the way.

FloodMesh, week of 22 September. It is a small handheld for floods: press
a button and your message is passed from device to device until it
reaches someone who can help, with no phone network.

- We tested one relay carrying an alarm about 550 m, further than the
  350 m a single pager reaches alone.
- We decided a pager should say clearly whether help is coming, not just
  that a message went out, and wrote that into the software this week.
- We decided the first version will do two things, SOS alarms and short
  text messages, so we can test those well before adding more.
- We found where to build the units for less, mostly from Indian and
  Chinese suppliers instead of the US.

What did not work: we still do not know why the last hop of the relay
test stopped just past 250 metres. We did not record signal strength
closely enough to say.

If you live in a flood-prone part of Chennai and have a rooftop or
balcony: would you let us test one relay unit from there?

Built in public.

#disastermanagement #chennai #floodsafety
