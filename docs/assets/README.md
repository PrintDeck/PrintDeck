# Browser logos

- `google-chrome.svg` comes from the official Google Chrome website:
  `https://www.google.com/chrome/static/images/chrome-logo-m100.svg`
- `microsoft-edge.png` comes from the official Microsoft Edge website CDN:
  `https://edgecdn-embza6g8cacagcbn.z01.azurefd.net/welcome/static/favicon.png`

The files are stored locally so the unsupported-browser screen does not depend
on third-party image requests at runtime.

Google Chrome and Microsoft Edge are trademarks of their respective owners.
Their marks are used only to identify compatible browsers; PrintDeck is not
affiliated with or endorsed by Google or Microsoft.

## Public website brand assets

- `brand/printdeck-logo-black.svg` and `brand/printdeck-logo-white.svg` are the
  public-site vector logos supplied for light and dark surfaces. Their
  letterforms are stored as compact vector paths; do not embed complete font
  files back into either logo.
- Matching PNG copies remain in `brand/` for raster-only consumers.
- `fonts/Poppins-Regular.ttf` and `fonts/Poppins-SemiBold.ttf` are extracted
  from the supplied logo artwork so the public website uses the exact same two
  Poppins faces. `fonts/OFL-Poppins.txt` contains the font license.

These files belong to the public website under `docs/`. They must not be loaded
from the embedded Web Config or documentation/manual asset trees.

## Integration marks

- `integrations/home-assistant.png` comes from the official Home Assistant
  website: `https://www.home-assistant.io/images/favicon-192x192-full.png`.

Home Assistant and its logo are trademarks of their respective owners. The
mark is used only to identify an integration guide; PrintDeck is not affiliated
with or endorsed by Home Assistant or the Open Home Foundation.

## Printer connection marks

The Works with pages use local copies of the reviewed marks:

- `integrations/printers/bambu.png`, `prusalink.png` and `elegoo.png` retain
  the existing PrintDeck Web Config brand-catalog PNG bytes for Bambu Lab,
  Prusa and ELEGOO.
- `integrations/printers/klipper.svg` is the unchanged upstream Klipper mark
  from `https://github.com/Klipper3d/klipper/blob/master/docs/img/klipper.svg`.
- `integrations/printers/octoprint.png` is the unchanged 500 px official
  OctoPrint logo from `https://octoprint.org/trademark-rules/logo_png_kit.zip`.

These marks identify compatible products and projects. They do not imply
partnership or endorsement. OctoPrint is a registered trademark of Gina Häußge;
the Notices page links to `https://octoprint.org/` alongside that attribution.

## GIF hero rotation

The U1 screen on the GIFs page alternates three Cartoon, three Cookie and three
classic clips, changing style every five seconds. The six
`marketing/gifs/cartoon-*.gif` and `marketing/gifs/cookie-*.gif` files are
unchanged LCD reaction assets from the Cartoon and Cookie themes. The three
classic clips reuse the existing violet printing, green filament loading and
cyan filament changing assets under `marketing/home/`.

The round display reuses `marketing/knomi2/blue-eye.gif`; CSS hue rotation
changes its iris colour without extra eye files. Both animations pause when
the hero or tab is hidden, and reduced-motion preferences show still posters.
