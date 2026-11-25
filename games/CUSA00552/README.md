# The Last of Us Remastered — CUSA00552

| | |
|---|---|
| Dump tested | CUSA00552 (US), version 01.00, base game |
| Mac | MacBook Pro M1, 8 GB |
| Status | boots, loads, reaches the title screen ("PRESS ANY BUTTON"); 4 minutes of the autotest without a crash |
| FPS | loading screen 60; title screen 35–37 at 480p, 720p and 1080p alike (bound by write faults, not by resolution) |
| Not tested | the menus beyond the title screen and gameplay; other regions and patches |

## Profile

[`profile.toml`](profile.toml), [`patches.xml`](patches.xml) — one required patch:

- **Bug Fix - Address Space Base** — the engine gives its memory areas 4 GiB slots starting at
  0x1000000000 and checks that each mapping lands exactly there. macOS reserves that range in every
  process, so the start moves to 0x8800000000. The patch finds its site by a code signature.

- **Resolution Patch - 480p / 720p / 900p** — the render settings' default writes four size pairs
  of 1920x1080; the patch writes the same instructions with the new size, as the engine's own
  mode switch does for 1280x720. The profile makes **720p the default** (`[graphics]` in
  `profile.toml`): it goes into `spartanport.ini` until the player picks another resolution in
  the in-game menu.

Everything else the game needed (fibers, PNG/JPEG decoding, a few kernel, Rtc and service
functions) is in the core.
