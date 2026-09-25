# PrintDeck product development guide

## Web Config and App/Cloud consistency

The device Web Config and the App/Cloud web panel are two views of the same
PrintDeck product. Keep shared screens, terminology, layout, controls, status
presentation, empty states, thumbnails and fallback images consistent.

For every change to a shared user-facing surface, inspect the equivalent view
in both products before considering the task complete. Implement the matching
change in both repositories when the behavior applies to both; do not assume
that editing one updates the other. Include all maintained translations and
check both desktop and mobile layouts. Preserve unrelated work in each
repository and report which surfaces were changed and verified.

Keep differences only when they are justified by local-device versus cloud
capabilities, permissions, security or unavailable data. Explain intentional
differences explicitly; do not imitate unsupported features or expose device-only
operations through the cloud. Cross-product consistency does not authorize
publishing, deploying or changing unrelated functionality.
