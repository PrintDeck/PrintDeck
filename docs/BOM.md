# PrintDeck bill of materials

This document describes the hardware used by the PrintDeck reference builds.
It is not a shopping guide, a compatibility list or a wiring and assembly
manual. Supplier links and untested substitute parts are intentionally not
listed.

Building from independently sourced parts requires enough electronics
experience to verify connector type, polarity, voltage, insulation and power
routing before anything is connected. If you are not comfortable making those
checks, please [contact PrintDeck Support](https://printdeck.xyz/support/)
about DIY sets or complete devices.

Choose one display and enclosure combination. Battery power, wireless
charging, magnets and audio are optional unless a case-specific list states
otherwise. Component compatibility and safe assembly remain the builder's
responsibility.

## Supported display boards

- **Round:** Waveshare ESP32-S3-Touch-AMOLED-1.75 with a 1.75-inch,
  466 x 466 capacitive touch AMOLED.
- **Square:** Waveshare ESP32-S3-Touch-LCD-1.54 with a 1.54-inch,
  240 x 240 capacitive touch LCD.
- **KNOMI2:** KNOMI2,
   240 x 240 capacitive touch LCD.

The display, ESP32-S3, touch controller, USB-C connector, battery charger and
audio electronics are integrated into these boards.

## The Riverstone

The Riverstone uses the round Waveshare ESP32-S3-Touch-AMOLED-1.75 board.

### Required

- **1x display board:** Waveshare ESP32-S3-Touch-AMOLED-1.75.
- **1x printed enclosure set:** The Riverstone version matching the selected
  5 mm or 6 mm magnet pockets.
- **3x screws:** M2 x 4 mm.

### Optional

- **1x LiPo battery:** protected single-cell, 3.7 V nominal, MX1.25 2-pin
  plug; maximum 35 x 50 x 10 mm.
- **1x insulating sheet:** electrically insulating, preferably
  flame-retardant; placed between the battery and PCB.
- **4x case magnets:** 5 mm diameter and at least 2 mm high, or 6 mm diameter
  for the matching enclosure version.
- **1x Qi receiver system:** see [Qi wireless charging](#qi-wireless-charging).
- **1x MagSafe-compatible magnetic ring:** at least 45 mm inner diameter and
  no more than 53 mm outer diameter.
- **1x speaker:** see [Speaker](#speaker).

The four case magnets are optional. The Riverstone works perfectly well
without them, but you lose the fun of hanging PrintDeck on the refrigerator
while cooking. They also let it attach to a steel tool cabinet or another
magnetic surface.

## The Scuttle

The Scuttle uses the round Waveshare
ESP32-S3-Touch-AMOLED-1.75 board.

### Required

- **1x display board:** Waveshare ESP32-S3-Touch-AMOLED-1.75.
- **1x printed enclosure set:** The Scuttle.
- **5x screws:** M2 x 6 mm, flat head.

### Optional

- **5x case magnets:** 6 mm diameter x 3 mm high.
- **1x speaker:** see [Speaker](#speaker).

## Snapmaker U1 Mini case

The Snapmaker U1 Mini case uses the square Waveshare
ESP32-S3-Touch-LCD-1.54 board.

### Required

- **1x display board:** Waveshare ESP32-S3-Touch-LCD-1.54, touch version.
- **1x printed enclosure set:** Snapmaker U1 Mini case.

### Optional

- **1x LiPo battery:** protected single-cell, 3.7 V nominal, MX1.25 2-pin
  plug; maximum 30 x 40 x 10 mm.
- **1x insulating sheet:** electrically insulating, preferably
  flame-retardant; placed between the battery and PCB.
- **1x speaker:** see [Speaker](#speaker).

## Battery

The battery is optional. Both boards can operate from USB-C without one.

Both supported Waveshare boards use an **MX1.25 2-pin** battery connector for a
single 3.7 V lithium battery. Waveshare calls this connector MX1.25 rather than
JST 1.25. Connector families with the same pitch are not necessarily
interchangeable.

The reference builds use a protected single-cell LiPo battery rated at 3.7 V
nominal (4.2 V when fully charged). Connector fit, wire colour and product
descriptions are not sufficient proof of compatibility. The builder must
verify polarity against the board markings and documentation before connecting
the battery.

- **The Riverstone:** maximum 35 mm wide, 50 mm long and 10 mm high.
- **Snapmaker U1 Mini:** maximum 30 mm wide, 40 mm long and 10 mm high.

Treat these as absolute envelope dimensions, including the protection PCB,
cable exit and any folded cable. A slightly smaller battery will be easier to
install without squeezing or bending the cell.

## Qi wireless charging

Wireless charging is optional and is intended for The Riverstone and The Scuttle.
A battery is required when wireless charging is used. The compact Qi
receiver coils that fit these enclosures do not provide enough stable power to
run the displays directly; they are intended to charge the battery, not replace
it as the device's power source.

- **1x Qi receiver module with receiving coil:** receiver only,
  Qi-compatible, regulated 5 V output and at least 1 A continuous output.
- **1x ferrite shielding layer:** behind the receiver coil, between the coil
  and the battery/electronics; preferably supplied and validated as part of
  the receiver assembly.
- **1x 5 V connection to PrintDeck:** a receiver-specific cable or adapter
  feeding the board's USB-C/5 V input.
- **1x MagSafe-compatible magnetic ring (optional):** at least 45 mm inner
  diameter and no more than 53 mm outer diameter.

The reference configuration uses a regulated **5 V** receiver rated for at
least **1 A**. Receiver selection and integration require verification of its
actual output behavior; voltage modes intended for other devices must not be
passed to PrintDeck.

The receiver must include both the coil and the Qi receiver/regulator
electronics. A bare induction coil is not sufficient. The ferrite layer directs
the magnetic field and separates the coil from the battery and PCB; do not
replace it with an ordinary metal sheet.

The Qi output belongs on the board's 5 V power path, never on its 3.7 V battery
socket. This BOM intentionally does not provide wiring instructions for a
third-party receiver.

## Speaker

The speaker is optional; PrintDeck works without it. Both boards provide an
MX1.25 2-pin speaker connector.

The reference enclosure target is a compact **8 ohm, 0.5-1 W** speaker using
the smallest format that fits. Independently sourced speakers must be checked
for electrical and mechanical compatibility by the builder.
