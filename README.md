# ps4mac_port

An experimental platform for running PS4 games on Apple Silicon Macs from a dump of your own copy.
The game's own x86-64 code runs under Rosetta 2 on a runtime that reimplements the PS4 system
libraries; its graphics go to a native arm64 process with the shadPS4 video core on Vulkan
(KosmicKrisp on Metal). The aim: **the core plus a dump is all a game needs** — a game's profile
holds only what is its own (patches, exceptions, test scenarios).

**Author:** Iuliia Gailova. Grown out of [SpartanPort](https://github.com/FelipePrado0/SpartanPort)
(God of War III), which grew out of [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc).

> Not affiliated with Sony Interactive Entertainment or any game's publisher. This repository
> contains no game code, data or artwork: every game needs a dump of **your own** copy. The window
> shows the game's title and icon from that dump.

## Games

Tested on a MacBook Pro M1, 8 GB, macOS 27.2.1. FPS at the 720p render resolution (the game scales
to 1080p itself; in-game menu → render resolution).

| Game | Title ID | Status | FPS | Page |
|---|---|---|---|---|
| Ratchet & Clank (2016) | CUSA01047 | played through several planets | level 30 (the game's cap), fights 21–26, Novalis 20–25 | [games/CUSA01047](games/CUSA01047/README.md) |
| God of War III Remastered | CUSA01623 | menus, cutscenes and the first fight | first fight ~24 (1080p: ~15) | [games/CUSA01623](games/CUSA01623/README.md) |
| The Last of Us Remastered | CUSA00552 | title screen, menus, New Game, the prologue, the first playable scene | title screen 35–37, first playable scene ~22 standing | [games/CUSA00552](games/CUSA00552/README.md) |

A game without a profile still starts with the core's defaults (God of War III reached its first
fight that way); a profile adds the game's patches — above all a render resolution patch, the main
lever for FPS on an M1. Some games need a patch to start at all: R&C and The Last of Us ask for
memory at an address macOS reserves in every process, and their profiles move it.

## Requirements

- Apple Silicon Mac, macOS 26 or newer, Rosetta 2. 8 GB works; more is not tested yet.
- A dump of your own copy from your own PS4 (decrypted `eboot.bin`, `sce_sys/param.sfo`).
- For building: Xcode Command Line Tools, Homebrew.

## Build

```bash
bash packaging/macos/build-deps.sh         # x86-64 dependencies (runtime side), once
bash packaging/macos/build-deps-arm64.sh   # arm64 dependencies (KosmicKrisp, SDL3, ...), once
bash build.sh                              # the runtime (out/ps4mac_runtime) and GPU library
bash service/build.sh                      # the arm64 graphics service (out/service/ps4mac_service)
```

## Run

```bash
PS4MAC_SERVICE=$PWD/out/service/ps4mac_service SPARTAN_GAME_DIR=/path/to/CUSA01047 python3 run.py
```

- `SPARTAN_GAME_DIR` — the dump (default: a folder named after a profiled Title ID next to the
  repository or one level further).
- Data (settings `spartanport.ini`, saves, shader cache) go to
  `~/Library/Application Support/ps4mac_port/<Title ID>`; `SPARTAN_DATA_DIR` chooses another folder.
- In game: Insert (or R3 + L2) opens the menu: render resolution, frame rate, overlay.
- Environment variables override the profile and the core's defaults (`docs/architecture.md`).
- The graphics service also keeps its findings for the game there: `host_rewritten.txt` lists guest
  memory the game rewrites while the GPU reads it, so the next start copies it from the beginning.

## Controls

A controller (DualShock 4, DualSense, Xbox and others SDL knows) works as on the PS4. Without one,
the keyboard:

| Key | PS4 |
|---|---|
| W A S D | left stick |
| arrow keys | right stick |
| Space, Left Shift, E, Q | Cross, Circle, Square, Triangle |
| 1, 3 | L1, R1 |
| R, F | L2, R2 |
| Z, C | L3, R3 |
| I K J L | d-pad up, down, left, right |
| Enter | Options |
| Tab, Backspace | touchpad click, left and right side |
| X, V | spin the left stick clockwise, counterclockwise |

X and V are for "rotate the stick" prompts in any game: hold the direction the prompt starts from
(W A S D), then X or V; the stick turns at full tilt, one turn in 0.6 s, without passing through
the centre. `SPARTAN_KEY_MAP` remaps keys, for example `SPARTAN_KEY_MAP="cross=Q,spin_cw=G"`; a key
mapped this way loses its default action.

## Adding a game

1. Dump your copy and run it as above, without a profile: many things already work.
2. `python3 run.py --check-imports` lists the PS4 functions the runtime still lacks; they go into the
   shared runtime, never into a profile.
3. `games/<TITLE_ID>/profile.toml` and a patch file for what is the game's own (a render resolution
   patch, bug fixes), and a page with what was tested. Patches: [docs/PATCHES.md](docs/PATCHES.md).

The rule (`docs/architecture.md`, 4.1): everything that emulates the PS4 goes into the core, even
when only one game shows the problem so far, so each new game improves the others. A profile holds
only what is in the game's own code (an address or timeout it hard-codes, its render resolution)
and its defaults and tests; a patch never hides a bug of the core.

## Documentation

- [docs/architecture.md](docs/architecture.md) — the architecture, the plan, every measurement and
  decision (in Russian).
- [docs/PATCHES.md](docs/PATCHES.md) — game patches: the profile's patch file, signatures, render
  resolution.
- [docs/spartanport](docs/spartanport/README.md) — SpartanPort's documentation (its launcher, mods,
  Windows and Linux), kept for reference and not updated for ps4mac_port. Design notes inherited
  from SpartanPort (`docs/upscaler.md`, `docs/motion_vectors.md`, `docs/DEPTH_ADAPTIVE_TAA.md`,
  `docs/parallel_gpu.md`, `docs/ultrawide.md`) stay next to the code that cites them.

## Credits

- **SpartanPort** — Felipe Prado and contributors; its README is in [docs/spartanport](docs/spartanport/README.md).
- **bloodborne_pc** — deadinside28, Supermedo and contributors.
- **shadPS4** — the video core and reference implementations of PS4 system functions.
- **KosmicKrisp** (Mesa) — Vulkan on Metal.

## License

GPL-2.0, as SpartanPort, bloodborne_pc and shadPS4. See [LICENSE](LICENSE).
