# FloodMesh enclosure — designing for IP67

IP67 means dust-tight, and survives 1 metre of immersion for 30 minutes.

For most devices sealing is straightforward. This one is harder, because it has
to **hear and be heard while sealed**. That tension drives most of what follows.

---

## 1. The hard problem: acoustic ports

A microphone and a speaker both need a path to outside air. A hole is a leak.
The industry answer is an **ePTFE acoustic vent** — a microporous membrane that
passes sound but blocks liquid water, rated to IP67/IP68. Gore, Saati and Nitto
all make them as adhesive-backed discs.

Budget roughly **1–3 dB of insertion loss**. That is not free, and this device is
already fighting for loudness on a 3.3 V rail — so oversize the port rather than
squeeze it.

Three details that decide whether this works:

**Seal the mic to its port.** The INMP441's acoustic inlet is a sub-millimetre
hole on the module PCB. If the enclosure vent simply opens into the case
interior, the mic hears the *box*, not the outside: muffled, boomy, and full of
handling noise. Design a short sealed tube from the vent to the mic port, with a
compressed foam or silicone gasket ring around it. Getting this right matters
more to intelligibility than anything in the firmware.

**Give the speaker a sealed back volume.** A speaker radiating into an open box
cancels itself at low frequencies. Partition the speaker chamber from the
electronics bay so the rear wave is trapped. More volume is better; 5–10 cm³ is
workable for a driver this size.

**Point the vents downward** in the carrying orientation. Water pooling on a
membrane heavily attenuates sound long before it defeats the seal — the device
goes quiet in rain, which is exactly when it must not.

---

## 2. The failure nobody designs for: pressure equalisation

This is the most common reason a genuinely well-sealed box still fills with
water.

A sealed enclosure is a fixed volume of air. Warm it in the sun and internal
pressure rises; cool it in rain or immerse it and the air contracts, creating a
partial vacuum that **actively pulls water past the gasket**. Every thermal cycle
pumps a little more in. The box passes a static immersion test on day one and
fails in service a month later.

Fit a **pressure equalisation vent** — the same ePTFE material, sold as a
screw-in or adhesive vent. It passes air and water vapour while blocking liquid.
One is enough.

Do not rely on the acoustic vents to do this job as well. They may, but their
membrane is tuned for acoustic transparency rather than airflow, and you would be
depending on a side effect.

---

## 3. Seal geometry — the numbers to model

Use a **face seal** (axial compression) rather than a radial one. It's far easier
to machine or print, and easier to inspect.

Starting points, then check against a proper chart — the Parker O-Ring Handbook
is the standard reference and is free:

| Parameter | Rule of thumb |
|---|---|
| Compression (static face seal) | **20–30%** of cord diameter |
| Groove depth | ≈ 0.75 × cord diameter |
| Groove width | ≈ 1.3 × cord diameter (the rubber must have somewhere to go) |
| Inside corner radius | ≥ 3 × cord diameter — sharp corners thin the cord and leak |
| Surface finish in the groove | smoother is better; ridges are leak paths |

Three things that catch people:

- **The groove must be a closed loop with no splice.** A cut-and-glued cord leaks
  at the joint. Use a moulded O-ring sized to your groove, or design the groove
  to a standard O-ring size and buy that.
- **Lid stiffness matters more than screw count.** A thin lid bows between
  fasteners and opens the seal mid-span. Either thicken the lid, add a stiffening
  rib just inboard of the groove, or bring the screws closer — **30–50 mm spacing**
  for a box this size.
- **Model the compressed state.** In SolidWorks, sketch the O-ring cross-section
  at its compressed height, not its free diameter, so you can see the groove is
  actually filled to roughly 70–85% and not overstuffed.

---

## 4. Buttons

Five inputs: the 2×2 keypad and the side PTT.

The best answer for this device is a **moulded silicone keypad overlay** with
integrated plungers over the tactile domes, its perimeter compressed by the lid
against a flat land. It's one seal for four buttons, it survives being pressed
with wet or gloved hands, and it keeps genuine tactile feedback — which matters
enormously when the user is panicking and cannot look at the screen.

Avoid these:

- **Individually sealed IP67 pushbuttons** — five of them is bulky, expensive, and
  five separate seals to get right.
- **Capacitive touch through the lid** — tempting because there are no holes at
  all, but wet hands and rain produce false triggers. On a device where a false
  press broadcasts NEED EVACUATION to the whole colony, that is disqualifying.

The side PTT is the one that gets pressed hardest and most often. Give it a
larger plunger and more travel than the face keys, and make it findable by feel
alone — a raised ridge or a different texture.

---

## 5. Penetrations

**Antenna.** Use an SMA bulkhead with an O-ring under the flange, or a purpose-made
IP67 SMA. Torque against a flat, machined land — not a curved wall. Add strain
relief inside for the U.FL pigtail; that tiny connector will not survive being
used as a mechanical stop.

**USB.** Decide deliberately, because it drives the whole product:

| Approach | Trade |
|---|---|
| No external USB; open the case to charge | Best seal. Painful in the field, and every opening risks the gasket |
| IP67 USB-C bulkhead connector | Clean, but adds cost and depth |
| Gasketed screw cap or captive flap | Cheap, common, and depends on the user closing it — which under stress they will not |
| Removable 18650 charged externally | No USB penetration at all; moves the problem to a battery hatch, which is a second seal |

For a device that lives in a bag for months and matters for one week a year, I
would lean toward **no routine external port** and a serviceable battery, rather
than a flap someone leaves open.

---

## 6. Manufacturing reality check

**A 3D-printed FDM case will almost never achieve genuine IP67.** Layer adhesion
leaves microscopic channels straight through the wall; the part is porous even
where it looks solid. You can improve it — 4–6 perimeters, solid infill near
seals, ASA/ABS vapour smoothing, or an epoxy coat — but you are fighting the
process.

Two honest routes:

1. **Prototype now:** buy a commercial IP67 polycarbonate enclosure with a clear
   lid, and design the *internal chassis* in SolidWorks — board mounts, speaker
   baffle, mic tube, button plungers. You get a real seal immediately and spend
   your CAD effort where it adds value.
2. **Production later:** injection moulding or SLA. Design for it now — draft
   angles of 1–2°, uniform wall thickness, no undercuts on the parting line.

Resin/SLA printing sits in between: far less porous than FDM, but brittle and
UV-degrading, so poor for something that lives outdoors.

---

## 7. Flood-specific, and worth more than the seal

**Make it float.** This device will be dropped in water — that is its entire
context. Air volume plus a little closed-cell foam costs nothing and turns "gone"
into "retrieved". Check it floats **screen up**, by placing the battery low and
the foam high. Model the centre of mass in SolidWorks and check it sits below the
centre of buoyancy.

**Make it visible.** High-visibility orange or yellow, and consider a
retroreflective strip. It will be looked for at night, in brown water, by torch.

**Give it a lanyard point.** Moulded into the body, not glued on — sized for a
wrist loop and strong enough to take the device's weight when snatched.

**Design for gloved, wet, cold hands.** Bigger buttons, more travel, more spacing
than feels necessary on the bench.

---

## 8. You cannot claim IP67 without testing it

Two tests, in this order:

**Air pressure test first (non-destructive).** Fit a temporary port, pressurise to
roughly 0.2 bar, submerge, and watch for bubbles. This finds leaks without
putting water near your electronics, and you can iterate on the gasket in
minutes.

**Then the real thing.** Fully assembled but with a **tissue or moisture-indicator
card inside instead of the boards**, submerged to 1 m for 30 minutes. Inspect the
tissue.

Then do it again after **twenty open-close cycles**. Seals that pass once often
fail after the gasket has been compressed, relaxed and slightly displaced a few
times — and in service that lid will be opened for batteries.

Finally, a thermal cycle: warm it, then immerse it cold. That is the condition
that exposes a missing pressure equalisation vent, and it is the one a static dunk
test will never catch.
