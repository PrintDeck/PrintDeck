# PrintDeck

**Your print status, always in view.**

PrintDeck turns a compact touch display into a dedicated local companion for
your 3D printer. It connects directly to Klipper and supported Bambu Lab
printers, keeping progress, tools, temperatures and camera views one glance
away—without requiring a cloud account.

[Project website](https://printdeck.xyz/) ·
[Install firmware](https://printdeck.xyz/firmware/) ·
[Case designs](https://printdeck.xyz/case-designs/) ·
[User manual](https://printdeck.xyz/printdeck-user-manual.pdf)

[![The Riverstone, The Scuttle and Snapmaker U1 Mini PrintDeck devices](docs/assets/marketing/cases/home-hero-v2.png)](https://printdeck.xyz/)

## Built for a glance

- **Live print status:** job phase, progress, temperatures, layers and timing on
  a screen that remains ready while the printer is active.
- **Multi-tool clarity:** automatic discovery of up to twelve Moonraker
  extruders, including live and target temperatures, filament material and
  colour.
- **Local camera views:** support for compatible Bambu Lab and Moonraker camera
  sources, plus dedicated no-printer-modification support for Snapmaker U1 and
  Creality K2 cameras.
- **Local by design:** PrintDeck connects directly to the printer and stores
  printer credentials on the device.
- **Power-aware:** configurable OLED power saving with touch, button and
  print-completion wake-up behavior.
- **Easy maintenance:** guided USB installation, factory reset and
  configuration-preserving USB or over-the-air updates.

## U1 Mini case

For Snapmaker U1, PrintDeck recognizes the stock configuration over the local
network and presents live job, tool and temperature data on a focused companion
display. The square U1 Mini enclosure mirrors the printer's shape, while
dedicated camera support works without modifying the printer.

## Supported display boards

PrintDeck supports three hardware variants with the same navigation, printer
features and settings:

- **Round:** Waveshare ESP32-S3 Touch AMOLED 1.75, 466 × 466;
- **Square:** Waveshare ESP32-S3 Touch LCD 1.54, 240 × 240.
- **KNOMI2:** KNOMI2, 240 × 240.

Each display target has an independent version, immutable release and stable OTA
channel. The round AMOLED remains the default build target.

## Get started

1. Choose one of the supported display boards.
2. Connect it to your computer over USB-C.
3. Open the guided [PrintDeck Web Installer](https://printdeck.xyz/firmware/).
4. Add your printer through PrintDeck Web Config.

The installer can perform a complete first installation or factory reset. It
can also update a configured device over USB without erasing Wi-Fi, printer or
display settings. Updates are additionally available from Web Config.

## Cases and hardware

The Riverstone, The Scuttle and Snapmaker U1 Mini enclosure designs cover the two
supported display formats. Experienced builders can use the project
[bill of materials](docs/BOM.md); it documents compatibility requirements
without recommending individual sellers or unverified components.

For official DIY sets or a complete, tested PrintDeck device, please
[contact PrintDeck Support](https://printdeck.xyz/support/).

## Source and support

The shipping firmware source, required build inputs and reviewed generated
product assets are available in this repository. Installation and updates use
the published target-specific firmware and browser installer.

Security issues should be reported according to [SECURITY.md](SECURITY.md).
General support routes are listed in [SUPPORT.md](SUPPORT.md).

## License

PrintDeck's original software and accompanying documentation are licensed
under the [PolyForm Noncommercial License 1.0.0](LICENSE). The license permits
use, modification and distribution for noncommercial purposes, subject to its
terms and the attribution requirements in [NOTICE](NOTICE).

Commercial use, including selling PrintDeck firmware, modified versions, DIY
sets or devices containing the software, requires a separate written license
from the copyright holder. [Contact PrintDeck Support](https://printdeck.xyz/support/)
to discuss commercial licensing.

Third-party components and assets remain subject to their own licenses, listed
in [THIRD_PARTY.md](THIRD_PARTY.md) and alongside the relevant files. The
PrintDeck name, project branding and product photography may not be used to
imply sponsorship, endorsement or an official PrintDeck product.

## Authors

- [Gulios](https://github.com/gulios)
- [Kitek](https://github.com/kitekkitek)

## Links and resources

- [Project website and product overview](https://printdeck.xyz/)
- [Guided Web Installer](https://printdeck.xyz/firmware/)
- [Case designs](https://printdeck.xyz/case-designs/)
- [User manual](https://printdeck.xyz/printdeck-user-manual.pdf)
- [Support](https://printdeck.xyz/support/)
- [Bill of materials](docs/BOM.md)
