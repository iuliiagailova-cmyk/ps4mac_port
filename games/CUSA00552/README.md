# The Last of Us Remastered — CUSA00552

| | |
|---|---|
| Dump tested | CUSA00552 (US), version 01.00, base game |
| Mac | MacBook Pro M1, 8 GB, macOS 27.2.1 |
| Status | menus, New Game, the prologue cutscene and the first playable scene (Sarah's room) |
| FPS | 720p: Sarah's room ~22 (standing); title screen 35–37 at 480p, 720p and 1080p alike; cutscene and loading screens 60 |
| Not tested | playing beyond the first room; other regions and patches |

## Profile

[`profile.toml`](profile.toml), [`patches.xml`](patches.xml) — three patches; without the first the
game does not start:

- **Bug Fix - Address Space Base** — the engine gives its memory areas 4 GiB slots starting at
  0x1000000000 and checks that each mapping lands exactly there. macOS reserves that range in every
  process, so the start moves to 0x8800000000. The patch finds its site by a code signature.

- **Resolution Patch - 480p / 720p / 900p** — the render settings' default writes four size pairs
  of 1920x1080; the patch writes the same instructions with the new size, as the engine's own
  mode switch does for 1280x720. The profile makes **720p the default** (`[graphics]` in
  `profile.toml`): it goes into `spartanport.ini` until the player picks another resolution in
  the in-game menu.

- **Bug Fix - GPU Frame Watchdog** — the engine halts when a GPU frame takes over 2 seconds; the
  first frames after a level loads take longer here, so the limit becomes 30 seconds.
  This is a safety net for slow first frames in the core, not a property of the game: the goal
  is for the game to run without it (docs/architecture.md, 4.1).

Everything else the game needed (fibers, PNG/JPEG decoding, a few kernel, Rtc and service
functions) is in the core.

## Notes

- New Game asks for a save slot (the system's save list). The list is drawn by the graphics
  service, out of reach of the runtime, so the core answers it as a player would: a new save when
  saving, the newest save when loading.
- Write faults (the game writing memory the GPU tracks, ~220/s in Sarah's room) cost the game's job
  threads ~0.3 s per second in all. The core's "hot pages" (pages left writable) cut them to ~100/s
  but break the particles the GPU simulates (the game stops on "Particle block traversal bug"), so
  they stay off.
