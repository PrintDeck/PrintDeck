# Kenney sound presets

The embedded 16 kHz mono assets come from four Kenney packs, all released under
CC0 1.0:

- Modern: [Interface Sounds 1.0](https://kenney.nl/assets/interface-sounds)
- Arcade: [Digital Audio 1.0](https://kenney.nl/assets/digital-audio)
- Sci-Fi: [Sci-fi Sounds 1.0](https://kenney.nl/assets/sci-fi-sounds)
- Voice Notifications fallback cues: [UI Audio 1.0](https://kenney.nl/assets/ui-audio)

| PrintDeck event | Modern | Arcade | Sci-Fi | Voice Notifications |
| --- | --- | --- | --- | --- |
| Startup | `confirmation_002.ogg` | `powerUp1.ogg` | `forceField_000.ogg` | spoken notification |
| Navigation | `select_001.ogg` | `pepSound3.ogg` | `laserSmall_000.ogg` | `rollover3.ogg` |
| Automatic orientation | `toggle_001.ogg` | `phaseJump2.ogg` | `laserRetro_002.ogg` | `switch14.ogg` |
| Print started | `maximize_005.ogg` | `powerUp8.ogg` | `doorOpen_001.ogg` | spoken notification |
| Progress 25% | generated | `powerUp5.ogg` | `laserSmall_002.ogg` | spoken notification with ascending celebration |
| Progress 50% | generated | `powerUp7.ogg` | `laserRetro_004.ogg` | spoken notification with ascending celebration |
| Progress 75% | generated | `zapThreeToneUp.ogg` | `laserLarge_004.ogg` | spoken notification with ascending celebration |
| Print paused | `minimize_002.ogg` | `twoTone2.ogg` | `forceField_004.ogg` | spoken notification |
| Print finished | `confirmation_004.ogg` | `threeTone1.ogg` | `doorClose_001.ogg` | spoken notification with final celebration |
| Print error | `error_003.ogg` | `zapThreeToneDown.ogg` | `explosionCrunch_000.ogg` | spoken notification |
| Bambu HMS alert | `error_007.ogg` | `laser8.ogg` | `laserLarge_001.ogg` | spoken notification |
| Filament attention | `question_002.ogg` | `threeTone2.ogg` | `slime_000.ogg` | spoken notification |
| Shutdown countdown | `tick_004.ogg` | `tone1.ogg` | `laserRetro_003.ogg` | `click2.ogg` |
| Shutdown | `close_004.ogg` | `highDown.ogg` | `doorClose_000.ogg` | spoken notification |
| Sound test | `confirmation_003.ogg` | `zapTwoTone2.ogg` | `laserRetro_003.ogg` | `switch3.ogg` |

Conversion retains each source duration, mixes stereo sources to mono, removes
DC offset, applies an anti-aliasing low-pass filter, resamples to 16 kHz, adds a
short edge fade and normalizes peak level. Clean also uses bounded transient
shaping so the short Voice Notifications fallback cues remain audible on the
device speaker. The locally synthesized speech assets under `audio/voice` are
not sourced from Kenney and are not covered by the CC0 statement above.

The resulting signed 16-bit PCM is stored as 4-bit IMA ADPCM in PrintDeck's
small `PDIA` container. Its 12-byte little-endian header contains the magic,
decoded sample count, initial predictor, initial step index and one reserved
byte. Firmware validates the header and decodes directly from flash into its
existing 320-sample output buffer; it never loads a complete effect into RAM.
Every converted asset receives a round-trip verification before it is committed.
No network fetch runs on the device. Soft and Oldschool remain deterministic
synthesized presets and do not embed third-party samples.
