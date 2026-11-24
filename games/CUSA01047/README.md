# Ratchet & Clank (2016) — CUSA01047

| | |
|---|---|
| Dump tested | CUSA01047 (EU), version 01.00, base game |
| Mac | MacBook Pro M1, 8 GB, macOS 27.2.1 |
| Status | played through several planets (Veldin, Novalis, Kerwan): menus, saves, cutscenes, gameplay, controller, audio |
| FPS (720p) | first level 30 (the game's cap), fights 21–26, Novalis 20–25 |
| Not tested | CUSA01073, CUSA01928, patch 01.09 |

## Profile

[`profile.toml`](profile.toml), [`patches.xml`](patches.xml):

- **Bug Fix - Address Space Base** (always on): the game's own address allocator hands out 2 GiB
  slots from `0x1000000000`, inside the range macOS reserves in every process; the base moves to
  `0x8800000000`. Required on macOS.
- **Resolution Patch - 480p / 720p / 900p** (in-game menu: render resolution): the size the game
  renders its scene at; it scales the picture to its 1920×1080 output itself. 720p is the
  recommended setting on an M1 (1080p: level ~16 FPS, fights ~15).
- `SPARTAN_HOST_BUFFERS_SKIP = 0x8900000000`: a 180 MiB range the game rewrites every frame stays
  copied for the GPU (in place, the GPU read data the game had already overwritten).

## Known issues

- Lamps on Novalis: light spots on the floor sometimes blink when the camera turns (the GPU depth
  the game culls lights with arrives a frame or two late; every fix cost FPS — left as is).
- Rare green vertical bars over fire for half a second.
- Buffer reads past a buffer's range return nearby data instead of 0 (the core's default for
  speed); `SPARTAN_SHADER_BOUNDS=1` checks every access (~13% slower) if a scene shows garbage.

## Autotests

Scripted pad input and frame dumps (scripts outside the repository, `RnC/autotest`): the first level,
a fight replay, Novalis (lamps, a walk), the flight to Kerwan.
