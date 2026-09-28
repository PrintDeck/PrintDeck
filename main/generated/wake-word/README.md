# Hey PrintDeck wake-word model

Voice settings offer Hey PrintDeck (default) and Hi ESP. Only the selected
detector is loaded into RAM. Listening is disabled by default. Upgrades retain
an explicitly saved phrase and the user's listening preference.

This 16 kHz mono INT8 streaming model combines synthetic speech and augmented,
consented speech samples. Offline tests do not establish universal accuracy
across speakers, microphones or rooms.

Size: 36,472 bytes.
SHA-256: `064aa8cb075ec551f02d68c5f7170523463d5e17627bd49f49c57af1198e507c`.
Input: three 40-channel feature frames, 10 ms step. Decision: mean of five
outputs at threshold 0.97, after 84 startup inferences are discarded.
