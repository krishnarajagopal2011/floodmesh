# Setting up who can act on an alarm

Tags: #announcement #feedback
Channels: X, Instagram, LinkedIn
Visual: a phone next to a unit, the phone screen showing a role being set,
photo or short clip. Real screen and real unit, no rendered graphics (this
has not been tested on hardware yet, so no reel showing it "working" end to
end).

## Facts

Internal only. The posts name no components, part numbers, or protocol
details, and do not mention Bluetooth encryption, key sizes or message
formats.

- A phone app registers a unit as one of three roles: civilian (default),
  relay, or responder. app/floodmesh_admin/README.md, lines 1-5.
- Setup is done over Bluetooth from a phone, by one trusted admin.
  app/floodmesh_admin/README.md "How to use".
- A responder role is time-limited and expires on its own: 10 days by
  default, with a 1-day or 10-minute option for a quick test.
  app/floodmesh_admin/README.md "Make responder"; CLAUDE.md: "Responders
  are made by one super admin over Bluetooth ... and expire after 10 days."
- Tested so far: 28 automated tests pass on the app's code, including a
  known-answer cryptography test case, and a one-off check where 10
  signatures made by the app were independently verified against a second,
  separate program. app/floodmesh_admin/README.md "Verification record"
  (commit 32d8dcb, one-off check run 2026-09-23).
- Not yet tested: the app has not been paired with a real unit over
  Bluetooth. CLAUDE.md notes the build sandbox cannot even compile the
  firmware or app; docs/prototype-v2-build.md's bring-up checklist (§9)
  is unticked for this build. The post says this plainly rather than
  implying it has been tried.

## X thread (each post under 280 characters, checked)

**1/** (206 chars)
#announcement

We are building an off-grid pager for floods. Every unit can now be set up
as one of three kinds: an ordinary user, a relay that helps pass messages
on, or a responder who can act on alarms.

[photo: phone next to unit, role screen]

**2/** (177 chars)
One trusted person does the setup, over Bluetooth from a phone. A responder
role is not forever: by default it lasts 10 days and then turns itself off,
so it has to be renewed.

**3/** (223 chars)
So far this is tested in code: 28 automated checks pass, including
known-answer cryptography checks done by hand against a second, independent
program. It has not been tried on a real unit over Bluetooth yet. That is
next.

**4/** (86 chars)
If you were setting this up for your street, who would you trust to be the
responder?

**5/** (152 chars)
Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
Built in public.

## Instagram caption

#announcement

We are building an off-grid pager for floods. Here is a new piece: setting
up who can act on an alarm.

Every unit can now be set up as one of three kinds: an ordinary user, a
relay that helps pass messages on, or a responder who can act on alarms.
One trusted person sets this up over Bluetooth from a phone. A responder
role is not forever, by default it lasts 10 days and then turns itself off,
so it has to be renewed.

So far this is tested in code, not yet on a real unit over Bluetooth. 28
automated checks pass, including known-answer cryptography checks done by
hand against a second, separate program. Pairing it with a real unit is the
next step, and I will post how that goes.

If you were setting this up for your street, who would you trust to be the
responder?

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh passes alarms and 10-second voice notes between buildings with no
tower and no internet. It is designed to run for days on ordinary batteries.
Built in public.

#floodmesh #floodsafety #disastertech #offgrid #buildinpublic #chennai #makerindia #disastermanagement

## LinkedIn

We just built the part that decides who is allowed to act on an alarm, and
I would like feedback on how the roles should work.

Every FloodMesh unit can now be set up as one of three kinds: an ordinary
user, a relay that helps pass messages between buildings, or a responder
who can act on alarms. One trusted admin sets this up over Bluetooth from a
phone. A responder role is not permanent: by default it lasts 10 days and
then turns itself off on its own, so it has to be renewed rather than
forgotten about.

Where it stands: the app's code has 28 automated tests passing, including
known-answer cryptography checks confirmed by hand against a second,
separate program. It has not yet been tried against a real unit over
Bluetooth, that is the next step, and I will share how it goes.

Who I want to hear from:
- Disaster management and volunteer teams: how should a responder role be
  handed out and renewed in practice, and by whom?
- Anyone who has coordinated relief on the ground: does a 10-day expiry
  make sense, or would you want it shorter or longer?

Built in public.

#disastermanagement #chennai #floodsafety
