# ps4mac_port

An experimental platform for porting PS4 games to Apple Silicon Macs: the game's own x86-64 code runs under Rosetta 2 on a high-level runtime that reimplements the PS4 system libraries, and the shadPS4 video core draws through Vulkan → KosmicKrisp → Metal.

**Author:** Iuliia Gailova. Grown out of [SpartanPort](https://github.com/FelipePrado0/SpartanPort) (God of War III), which grew out of [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc); see [Credits](#credits). GPL-2.0.

> Not affiliated with Sony Interactive Entertainment or any game's publisher. This repository contains no game code, data or artwork: every game needs a dump of **your own** copy.

## Games

Each game is a profile in [`games/`](games): its Title IDs, patch file, settings, known issues and test scenarios — data, no code.

| Game | Title ID | Status | Page |
|---|---|---|---|
| Ratchet & Clank (2016) | CUSA01047 | first levels playable on an M1 8 GB (7–11 FPS) | [games/CUSA01047](games/CUSA01047/README.md) |

## Build and run

```bash
bash packaging/macos/build-deps.sh   # x86-64 dependencies, once
bash build.sh --test                 # runtime, GPU library and tests
python3 run.py                       # finds a dump named after a profiled Title ID next to the port
```

`SPARTAN_GAME_DIR` and `SPARTAN_DATA_DIR` choose the dump and the data folder; environment variables override the profile's defaults.

## Adding a game

1. A dump of your own copy (`eboot.bin`, `sce_sys/param.sfo`).
2. `games/<TITLE_ID>/profile.toml` (see [CUSA01047](games/CUSA01047/profile.toml)) and its patch file.
3. `python3 run.py --check-imports` lists PS4 functions the runtime still lacks; they go into the shared runtime.

The architecture and the plan (native arm64 graphics process) are in `docs/`.

## Credits

- **SpartanPort** — Felipe Prado and contributors; its README is in [docs/spartanport](docs/spartanport/README.md).
- **bloodborne_pc** — deadinside28, Supermedo and contributors.
- **shadPS4** — the video core and reference implementations of PS4 system functions.
- **KosmicKrisp** (Mesa) — Vulkan on Metal.
