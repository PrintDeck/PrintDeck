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
