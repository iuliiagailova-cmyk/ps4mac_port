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

## Module

The profile turns on God of War III's module (`modules = ["god_of_war"]`, code in
[`module/`](module/runtime.c)), with SpartanPort's features that still work in two processes:

- the engine frame rate, changed live (in-game menu, R3 + L1 + R1 or F9);
- **Exit to Desktop** in the game's pause menu.

SpartanPort's cheats, orb and damage multipliers, enemy health bar and Automatic QTE were removed
(25.11.2026): their switches lived in the graphics service's menu, which never reached the game.
Its keyboard answers to the stick-rotation prompts were replaced on 26.11.2026 by the core's spin
keys, which work in any game: hold the direction the prompt starts from (W A S D), then X
(clockwise) or V (counterclockwise). They are checked against the game's rotation check in a test,
not yet in a live prompt.

## Fixed

- **Lightning turned the screen white for 2–4 s** (25.11.2026): the GPU read guest memory the game
  rewrites every frame; the core finds such memory itself and copies it.
- **White flashes, sometimes with coloured specks, on the first hits of a fight** (26.11.2026): the
  same memory, used for the first time when the fight starts, was found only after 2–4 s. It is now
  found in under a second, and remembered in the data folder (`host_rewritten.txt`), so later starts
  copy it from the beginning. Not yet confirmed in play.

## Known issues

- Only the beginning of the game is tested.

## Autotest

`~/0_Games/GoWIII/autotest/bench.sh` (outside the repository): New Game → God (Normal) → cutscenes
skipped → the first fight; mean FPS after 150 s (if Kratos dies, the death screen at 60 FPS raises it).
