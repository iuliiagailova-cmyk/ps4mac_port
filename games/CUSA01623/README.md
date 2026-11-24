# God of War III Remastered — CUSA01623

| | |
|---|---|
| Dump tested | CUSA01623 (US), version 01.00, base game |
| Mac | MacBook Pro M1, 8 GB, macOS 27.2.1 |
| Status | menus, brightness setup, New Game, cutscenes (skippable with X), the first fight on Gaia; ~10 minutes played without a crash |
| FPS | first fight: 720p ~24, 1080p ~15; menus and cutscenes 60 |
| Not tested | beyond the first fight; CUSA01715, CUSA01720, patch 01.02 |

Without any profile the dump already ran to the first fight on the core's defaults.

## Profile

[`profile.toml`](profile.toml), [`patches.xml`](patches.xml) — SpartanPort's patch file. Its entries are
tagged 01.02 but find their sites by signature, so they apply to 01.00 as well. On by default:

- **Skip Any Video With X Button** — cutscenes can be skipped.
- **Bug Fix - Startup Black Screen**.
- **Bug Fix - Texture Corruption Fix** — a larger video arena (replaced by the resolution patches,
  which enlarge it themselves).

In the in-game menu: **render resolution** (the resolution patches; 720p recommended on an M1) and the
frame rate patches.

No settings of its own: the core's defaults apply.

## Known issues

- Only the beginning of the game is tested.

## Autotest

`~/0_Games/GoWIII/autotest/bench.sh` (outside the repository): New Game → God (Normal) → cutscenes
skipped → the first fight; mean FPS after 150 s (if Kratos dies, the death screen at 60 FPS raises it).
