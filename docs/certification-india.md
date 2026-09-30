# Certification for selling FloodMesh units in India

Research notes, 30 September 2026. The hardware in question: an ESP32-S3
(Bluetooth/Wi-Fi at 2.4 GHz), a Semtech SX1262 LoRa radio at 865–867 MHz
(the Wio-SX1262 on PCB V1, onboard on the Heltec V3), and a Li-ion cell.

**Status: not verified.** This is from web search summaries of consultancy
and legal-update pages. The cloud sandbox's network policy blocked the source
pages themselves, including the gazette (see CLAUDE.md, "Cloud sandbox
network"). Confirm everything here with an accredited test lab before
relying on it. Nothing here is a DECIDED item.

## Mandatory

| # | What | Why it applies | Notes |
|---|---|---|---|
| 1 | **WPC Equipment Type Approval (ETA)**, self-declaration route on the Saral Sanchar portal | Two radios: 865–867 MHz LoRa and 2.4 GHz Bluetooth/Wi-Fi. Licence-exempt equipment must still be type approved | RF test report from an accredited lab (ISO/IEC 17025 or NABL). About ₹10,000 per model and 2–4 weeks, per consultants. **Per model**: the Heltec build and the PCB are separate models, and a changed radio or antenna means a new test. The Indian brand owner files it |
| 2 | **BIS CRS registration of the Li-ion battery** (IS 16046 Part 2 / IEC 62133-2) | Li-ion cells and packs are under the Compulsory Registration Scheme | Simplest: buy cells or packs that already carry a BIS R-number on the label. A charger or adapter shipped with the unit needs BIS too; a unit that charges from the user's own USB-C charger does not |
| 3 | **CPCB EPR registration, e-waste** (E-Waste (Management) Rules 2022) | The seller is the "producer" of electronic equipment | Registration plus yearly collection and recycling targets |
| 4 | **CPCB EPR registration, batteries** (Battery Waste Management Rules 2022) | A battery shipped inside a product counts | Separate from the BIS battery registration |
| 5 | **Legal Metrology packaging labels** (Packaged Commodities Rules) | Retail sale in packages | Manufacturer name and address, MRP, month and year of manufacture, customer-care contact |

## Probably not needed; confirm with the lab

- **TEC MTCTE** covers telecom equipment that connects to public networks.
  FloodMesh units don't: no cellular and no internet. Updates to units given
  to users go over Bluetooth (§13.6), which strengthens that case. Check
  whether any notified MTCTE category covers the device, especially because
  the ESP32 has Wi-Fi.
- **BIS CRS for the device itself.** CRS lists specific product categories
  (laptops, power banks, smart watches and so on). A LoRa messenger doesn't
  obviously fall into one; check the current list.

## Worth doing even though not mandatory

- **Product safety test** to IS/IEC 62368-1: limits liability, and government
  buyers often ask for it.
- **IP67 test** to IS/IEC 60529, if IP67 is advertised.
- **UN 38.3 test summary and safety data sheet for the battery**: couriers and
  airlines ask for them to ship lithium batteries.
- **For government or disaster-management buyers** (e.g. GeM): tenders usually
  ask for the statutory certificates above; some also ask for ISO 9001 for the
  manufacturer.

## What the modules' own certificates cover

- **They don't replace the product's ETA.** Espressif's certificates for the
  ESP32-S3-WROOM-1U, and any FCC/CE marks Seeed holds for the Wio-SX1262, do
  not transfer: WPC approves the finished product. Their test reports can
  shorten the product's testing.
- **The antenna is part of the approval.** Espressif certified the WROOM-1U
  with a 2.33 dBi monopole; FloodMesh uses a sticker antenna for Bluetooth
  (§9 #7). The LoRa whip is chosen in `docs/architecture.md` §14. Test with
  the final antennas.

## Firmware to fix before the RF test

The rule now in force for this band is reported to be the **Use of Low Power
Equipment in the Frequency Band 865–868 MHz for Short Range Devices
(Exemption from Licence) Rules, 2021**, notified 10 December 2021. Its tables
are reported to allow:

| Table | Power | Other conditions |
|---|---|---|
| Non-specific short-range devices | 25 mW e.r.p. | 1% duty cycle, frequency hopping (≤ 50 kHz, 58 or more channels) |
| The other table | 500 mW e.r.p. | ≤ 200 kHz bandwidth, a duty-cycle limit, and **adaptive power control required** |

The rules also say the equipment "shall be type approved". The older
G.S.R. 564(E) (2008) conditions (1 W transmitter, 4 W e.r.p., 200 kHz) are
still quoted by some consultants for 865–867 MHz; which one applies is part of
`docs/architecture.md` §1.1.

If FloodMesh falls under the 500 mW table, two firmware gaps would fail the
test:

1. **Adaptive power control.** Firmware V4 always transmits at a fixed
   20 dBm (`FM_LORA_TX_DBM`). §12.2 already proposes that each ACK carry the
   received signal strength so the sender can turn its power down; it would
   become required.
2. **The duty-cycle exemption for SOS and ACK frames** (the legal risk in
   §1.1) has to go, for example replaced by the reserved alarm budget proposed
   there.

## Suggested order

1. Get the gazette PDF of the 2021 rules into `docs/reference/`, and confirm
   which table applies (§1.1).
2. Fix the firmware: adaptive power control if required, and a duty-cycle
   limit that covers SOS and ACK frames.
3. Freeze the hardware and antennas of the model to be sold.
4. RF test at an accredited lab, then file the WPC ETA on Saral Sanchar.
5. Choose BIS-registered cells.
6. Register with CPCB for e-waste and battery EPR.
7. Packaging labels.

Have the ETA in place before units go to residents. For bench and field
tests, ask the lab whether an experimental licence is needed.

## Sources (search summaries; the pages themselves were not readable here)

- [Bureau Veritas: India WPC introduces ETA through self-declaration on Saral Sanchar](https://www.cps.bureauveritas.com/newsroom/india-wpc-wing-introduce-equipment-type-approval-eta-through-self-declaration-saral)
- [Granite River Labs: guide to WPC and ETA certification](https://www.graniteriverlabs.com/en-us/market-access-services/country/india/wpc-mark)
- [PCN Global: WPC ETA for 433 MHz and sub-GHz short range devices](https://pcnindiaglobal.com/2026/07/07/wpc-eta-433-mhz-sub-ghz-short-range-devices-india/)
- [SIACC: WPC ETA approval for IoT and wireless devices (2026)](https://siacc.co.in/blog/wpc-eta-approval-iot-wireless-devices-india-2026)
- [RAKwireless: an example WPC certificate (RAK2287)](https://downloads.rakwireless.com/LoRa/RAK2287-Mini-PCIe/Certification-Report/RAK2287_WPC_Certification.pdf)
- [TEC: Mandatory Testing and Certification of Telecom Equipment](https://www.tec.gov.in/mandatory-testing-and-certification-of-telecom-equipments-mtcte)
- [Nemko: MTCTE](https://www.nemko.com/product-certification/mtcte)
- [Sunren: IoT device certification in India (WPC, BIS, TEC)](https://www.sunren.net/blog/iot-device-certification-india/)
- [EVTL India: BIS certification for Li-ion batteries, IS 16046 Part 2](https://evtlindia.com/bis-crs/bis-certification-for-sealed-secondary-batteries-cells-lithium-ion-battery-is-16046-part-2-2018)
- [PCN Global: BIS CRS registration for Li-ion batteries](https://pcnindiaglobal.com/2026/07/20/bis-crs-registration-lithium-ion-batteries-india/)
- [Battery import compliance, India 2026](https://www.seaaircargosystems.com/battery-import-compliance-india.html)
- [DoT: 865–868 MHz short range devices exemption rules](https://dot.gov.in/spectrummanagement/use-low-power-equipment-frequency-band-865-868-mhz-short-range-devicesexemption)
- [Legality Simplified: 865–868 MHz SRD exemption rules 2021](https://legalitysimplified.com/2021/12/15/the-use-of-low-power-equipment-in-the-frequency-band-865-868-mhz-for-short-range-devices-exemption-from-licence-rules-2021/)
- [Gazette PDF: 865–868 MHz SRD rules 2021 (thc.nic.in)](https://thc.nic.in/Central%20Governmental%20Rules/use%20of%20low%20power%20Equipment%20in%20the%20frequency%20band%20865%20to%20868%20MHz%20for%20Short%20Range%20Devices%20Exemption%20from%20Licence%20Rules,2021.pdf)
- [DoT: delicensing in 865–867 MHz, G.S.R. 564(E)](https://dms.dot.gov.in/sites/default/files/Delicensing%20in%20865-867%20MHz%20band%20%5BGSR%20564%20(E)%5D_0.pdf)
