# Game patches

A patch changes a game's code or data **in memory** when it starts; the dump is never changed.
In ps4mac_port a patch is for what is in the game's own code and cannot be done by the core: an
address the game hard-codes inside the range macOS reserves, a timeout it hard-codes, its render
resolution. Anything that emulates the PS4 belongs in the core instead, and a patch never hides a
bug of the core (`docs/architecture.md`, 4.1).

## Where patches live

Each game's profile names its patch file:

```toml
# games/<TITLE_ID>/profile.toml
[patches]
file = "patches.xml"
app_ver = "01.00"   # the APP_VER the file's entries are written for
```

The file uses the shadPS4/GoldHEN XML format: one `<Metadata>` per patch, with `Name`, `Note`,
`Author`, `AppVer` and `isEnabled`, and its writes in `<PatchList>`. Addresses are PS4 virtual
addresses (the `eboot.bin` image starts at `0x400000`).

## What is applied

- Every patch with `isEnabled="true"`, such as the address patches R&C and The Last of Us need to
  start at all.
- **One** `Resolution Patch - <size>`: the one that matches `render_resolution` in the game's
  `spartanport.ini` (in-game menu, Insert or R3 + L2; it takes effect at the next start). A profile
  can choose the default:

  ```toml
  [graphics]
  render_resolution = "720p"   # written into spartanport.ini until the player picks another
  ```

- The names in `SPARTAN_PATCHES` (`;`-separated), for experiments.

The log lists what was applied, and a patch whose site was not found in this `eboot.bin` is
skipped with a note instead of writing over the wrong bytes.

## Signatures

A `Line` with only an `Address` writes at that address, which is right only for the build the
file was written for. A `Pattern` finds the site in any build whose code matches:

```xml
<Line Type="bytes" Address="0x00d103b8" Value="..." Pattern="c7 87 84 00 00 00 00 00 00 00 ..." At="0x18"/>
```

- `Pattern`: the original bytes around the site, `??` where builds may differ (rip-relative
  displacements, branch targets). It must match exactly once.
- `At`: the site's offset from the start of the pattern.

Data has no bytes of its own to search for, so a patch for a value in `.data` or `.rodata` finds the
code that uses it, and follows the operand:

```xml
<Line Type="bytes" Address="0x01530d18" Value="0000f041" Target="watchdog_seconds"/>
<Target Name="watchdog_seconds" Address="0x01530d18"
        Pattern="c5 f8 2e 05 ?? ?? ?? ?? 77 5a f3 90 ..." At="4" Ref="1" Tail="0"/>
```

- `Target`: a named site; a `Line` with `Target="name"` writes there.
- `Ref="1"`: the 4 bytes at the site are a rip-relative or rel32 operand; the target is the
  address it points to. `Tail`: the instruction's bytes after the operand (0 when it ends there).

`Type` is `bytes` (hex), `bytes16`/`bytes32`/`bytes64`, `float32`/`float64`, `utf8` or `utf16`.
Mask (search-and-replace) patches are not supported.

## Examples

- [games/CUSA01047/patches.xml](../games/CUSA01047/patches.xml) — Ratchet & Clank: the address
  space base, the resolution patches.
- [games/CUSA00552/patches.xml](../games/CUSA00552/patches.xml) — The Last of Us Remastered: the
  address space base through a `Target`, a GPU frame watchdog constant, the resolution patches
  rewriting a block of instructions. The comment at the top of the file explains each site.
- [games/CUSA01623/patches.xml](../games/CUSA01623/patches.xml) — God of War III Remastered:
  SpartanPort's patch file (resolution, frame rate, video skip), with signatures.

## Third-party patches

shadPS4 and GoldHEN patch files for the game in `<data folder>/patches/*.xml` are read too; the
ones marked `isEnabled="true"` apply, and `<data folder>/patches.json`
(`{"enabled": [...], "disabled": [...]}`, entries `<file name>/<patch name>`) changes that. They are
written for a build's addresses, so they work only on that build. Not tested in ps4mac_port yet.
