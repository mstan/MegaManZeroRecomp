# MegaManZeroRecomp — Mega Man Zero, recompiled

> _This recompilation is a **byproduct of developing
> [gbarecomp](https://github.com/mstan/gbarecomp)** — the games are the proving ground, the framework is the
> goal, and depth will keep landing over months, not days. My time for any one
> title is limited, so I ask for your patience. Contributions are welcome —
> testing, issues, and PRs to the game or framework all help and will
> accelerate this game's polish. More on the why at:
> [Recomp + AI: 5 Months Later »](https://1379.tech/recomp-ai-5-months-later/)_

Static recompilation of **Mega Man Zero** (Game Boy Advance) to native PC,
built on the [`gbarecomp`](https://github.com/mstan/gbarecomp) framework.

> **Status — playable static-first bring-up (v0.0.2)**
>
> Mega Man Zero boots through the real GBA BIOS, reaches gameplay, has working
> controls, audio, and persistent SRAM, and is comfortable to play in the
> normal windowed runner. Tested routes through the opening mission have full
> static coverage. The entire game has not been exhaustively proven static:
> an uncovered target falls back to the instruction interpreter, is reported,
> and can be folded into a later static corpus.

## Screenshots

| Opening mission | Active gameplay |
|---|---|
| ![Zero and the Resistance in the opening mission](docs/screenshots/mmz-opening.png) | ![Zero fighting through the opening stage](docs/screenshots/mmz-gameplay.png) |

Both images were captured from strict native/LLE verification runs.

## What is recompiled

The original ROM's ARM7TDMI ARM/Thumb code is translated into native C++ ahead
of time. The real GBA BIOS is also recompiled and executed through the LLE path;
BIOS HLE is an optional convenience and is not used to establish correctness.
The `gbarecomp` runtime models the PPU, APU, DMA, timers, interrupts, cartridge
SRAM, input, and other hardware-facing behavior.

This is not a decompilation or a source port. No game source, ROM data, or BIOS
image is included in this repository or its releases.

## ROM identity

| Target | Game | Region/revision | SHA-1 | Debug port |
|---|---|---|---|---|
| `MegaManZeroRecomp` | Mega Man Zero | USA, revision 0 | `193b14120119162518a73c70876f0b8bffdbd96e` | 19862 |

The runtime hash-gates the ROM before execution.

## Quick start

1. Download `MegaManZeroRecomp-windows-x64-v0.0.2.zip` from
   [Releases](../../releases) and extract the whole folder.
2. Run `MegaManZeroRecomp.exe`.
3. Select your own legally obtained Mega Man Zero (USA) ROM and GBA BIOS dump
   when prompted. Their paths are cached for future launches.
4. Play. SRAM saves, save states, and native coverage caches stay beside the
   extracted runner.

## Controls

| GBA input | Keyboard |
|---|---|
| D-Pad | Arrow keys |
| A | Z |
| B | X |
| L | A |
| R | S |
| Start | Enter |
| Select | Right Shift |
| Fast-forward | Hold Tab |

Save states use **Shift+F1–F9** to save and **F1–F9** to load.

## Experimental extended view

The faithful default remains 240x160. Widescreen is deliberately opt-in and
supports two elective policies: a fixed logical width, or an adaptive width
that follows the live window aspect ratio.

For a fixed width, create a `game.toml` beside the executable:

```toml
[video]
view_width = 288
```

For a one-off launch, use `MegaManZeroRecomp.exe --view-width 288`. The
`GBARECOMP_VIEW_WIDTH` environment variable is also supported and has the
highest priority. Set the width back to `240` to restore faithful presentation.

For adaptive resizing, use `MegaManZeroRecomp.exe --resize-view` or set:

```toml
[video]
resize_view = true
```

Without a fixed width, the window opens at the native 3:2 size. Combining
adaptive mode with `view_width` uses that width as the initial window aspect,
then live resizing continuously adjusts the logical framebuffer from 240 up to
480 pixels while retaining the 160-line height. The launcher Aspect ratio row
provides this initial width. Adaptive fullscreen instead follows the display
and ignores the saved fixed aspect for that launch.

Fixed widths from 240 through 480 are accepted; height remains 160 and a
resized window preserves the selected logical aspect ratio. Tile-aligned
288x160 remains the recommended fixed target.
The 384x160 view is a progressive-validation target, while exact-2x 480x160 is
a research mode that can reveal authored scenery and encounter assumptions far
beyond the original camera. Both still require whole-game route coverage.

The enhancement executes Mega Man Zero's original guest full-reload and
incremental background streamers at shifted margin positions and keeps their
generated tilemaps in presentation-only caches. It widens all twelve original
guest OAM clipping paths through reviewed ROM-literal overrides and extends OBJ
X interpretation only in the wide PPU path. The original stage spawn manager
also scans its original spawn table with tile-quantized horizontal activation
bounds extended by the selected view width; entity creation, flags, and culling
remain guest LLE. During active gameplay, Zero's authentic BG0 HUD is anchored
to the extended left content edge and a boss gauge is anchored to the right;
menus and faithful 240x160 presentation remain unchanged. The authentic live
maps, CPU state, timing state, and world-layer center are preserved: synthetic
guest writes are journaled and rolled back, device writes are suppressed, and
stack use is canary-checked.
Margin caches invalidate after save-state loads, authentic full reloads, and
coordinate discontinuities, then re-seed from subsequent guest streamers.
Unsupported PPU layouts, non-uniform window
effects, inactive stage scenes, stage-behavior transitions, camera bounds, and
authored outer-map edges fail closed to black margins.

This remains experimental rather than a whole-game widescreen guarantee.
The common stage activation window is extended, but entity-specific creation
and deletion rules, scripted encounters, and the 128-entry OAM budget remain
authentic and may still expose game-specific pop-in under wider or untested
routes. Use 240x160 for the faithful view and 288x160 for the recommended
enhanced view. Treat 384x160 as progressive validation and 480x160 as an
exact-2x diagnostic, not as whole-game-supported modes.

Known v0.0.2 limitation: loading a save state while widescreen is active can
temporarily leave the extended presentation caches out of sync. A normal room
reload (including dying and choosing Retry) rebuilds them correctly. SRAM saves
and faithful 240x160 presentation are unaffected.

## Static coverage and fallback

The normal player build is static-first. A reviewed generated function runs
natively. If indirect control flow reaches an address absent from that corpus,
the runtime executes only that gap in its ARM/Thumb interpreter and emits a
dispatch miss. Self-healing can compile the observed target to native code and
persist it in `recomp_cache/`; the reviewed proposal can then be folded back
into `game.toml` and regenerated for a later release.

Strict verification is deliberately different. With
`GBARECOMP_STRICT_STATIC=1`, caches, self-healing, and interpreter bridging are
disabled, so the first missing PC aborts. A passing strict run is therefore a
path-specific static-coverage proof, not a whole-game claim.

## Verified bring-up evidence

The committed corpus contains 10,885 ARM/Thumb functions, four observed
code-copy mappings, 33 bounded callback/jump-table declarations, and 92 exact
interior resume aliases (including three bounded, reviewed IRQ-resume spans).
These deterministic LLE campaigns pass with zero
dispatch misses, interpreted instructions, healed/cache code, unmapped bus
accesses, or unhandled I/O accesses:

| Profile | Frames | Route covered |
|---|---:|---|
| `campaign` | 30,000 | Reset through the opening mission |
| `campaign-combat` | 60,000 | Sustained movement, attacks, jumps, and dashes |
| `campaign-traverse` | 60,000 | Broad opening-stage traversal and callbacks |
| `campaign-safe` | 30,000 | Lower-risk route to the Golem sequence |
| `campaign-clear` | 30,000 | Golem trigger, Z-Saber grant, and weapon callback |

The cartridge SRAM model starts blank at `0xFF`, supports byte writes and
mirroring, and persists atomically. Strict verification uses a fresh
profile-specific save so previous play cannot hide missing initialization.

### Independent oracle checks

An isolated NanoBoyAdvance run with MP2K HLE disabled, a real BIOS, and blank
SRAM replays the same hardware input. Native frame 14,999 and oracle frame
15,000 are pixel-identical; the one-frame offset is a presentation-boundary
phase difference. The combat route remains exact through native frame 9,558,
after which nearby-frame matching isolates a late animated-sprite/projectile
timing residual. The world/background remains exact in the measured late
combat pair. Those residuals are documented rather than represented as full
all-frame visual parity.

The campaign also exercises a 0x24-byte position-independent routine copied
from ROM `0x080C8900` to overlapping IWRAM/stack placements. The recompiler's
`source_addr` metadata decodes that observed LLE pattern from its ROM source
while retaining runtime relocation; it is not a claim of arbitrary dynamic
code relocation.

## Building from source

Prerequisites on Windows: CMake, Ninja, and MSYS2's mingw64 GCC/G++ and SDL2.
Clone this repository next to `gbarecomp`:

```powershell
git clone https://github.com/mstan/gbarecomp.git
git clone https://github.com/mstan/MegaManZeroRecomp.git
cd MegaManZeroRecomp
```

Place the verified ROM at `roms/megaman_zero_usa.gba`. ROM-derived generated
translation units are intentionally not committed, so build the engine tool
and regenerate them locally:

```powershell
cmake -S ..\gbarecomp -B ..\gbarecomp\build -G Ninja `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build ..\gbarecomp\build --target gba_recompile

pwsh tools/regen.ps1

cmake -S . -B build -G Ninja `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build build --target MegaManZeroRecomp --parallel
```

Run the normal interactive discovery build:

```powershell
pwsh tools/play-discovery.ps1
```

It uses real BIOS/LLE boot, persistent SRAM, static-native dispatch first, and
an interpreter bridge only for uncovered targets. Each session records logs,
coverage JSON, reviewed-proposal fragments, input, ROM/build hashes, and exact
initial/final SRAM snapshots under `build/discovery_sessions/`. Nothing is
automatically merged into `game.toml`.

After reviewing a session, replay it with strict fallback disabled:

```powershell
pwsh tools/verify-strict.ps1 -Frames <final_ppu_frame> `
  -InputProfile user-replay `
  -InputTrace .\build\discovery_sessions\<session>\keyinput.csv `
  -InitialSave .\build\discovery_sessions\<session>\initial.sav
```

Savestate loads are not encoded in input traces, so avoid them in a coverage
session that must be replayed deterministically.

The deterministic verification campaigns are:

```powershell
pwsh tools/verify-strict.ps1
pwsh tools/verify-strict.ps1 -Frames 30000 -InputProfile campaign
pwsh tools/verify-strict.ps1 -Frames 60000 -InputProfile campaign-combat
pwsh tools/verify-strict.ps1 -Frames 60000 -InputProfile campaign-traverse
pwsh tools/verify-strict.ps1 -Frames 30000 -InputProfile campaign-safe
pwsh tools/verify-strict.ps1 -Frames 30000 -InputProfile campaign-clear
python tools/compare_nba_lockstep.py --profile campaign-clear `
  --checkpoints 14000,14500,16000,18000,20000 --image-only
```

Generated guest code is deterministically split into 16 stable shards.
Unchanged regeneration leaves shards untouched, and a local metadata addition
rebuilds only its address-hashed shard.

## Release packaging

With the generated corpus present, produce the same ROM/BIOS-free Windows zip
used by GitHub Releases:

```powershell
powershell -File tools\make_release.ps1 -Version 0.0.2
```

The archive contains the stripped executable, four runtime DLLs, a local
overlay toolchain for native self-healing, and a player README. It does not
contain the ROM, BIOS, save data, symbols, config, or generated source.

## Legal

Mega Man Zero and related names are trademarks of Capcom. This unaffiliated,
non-commercial preservation and research project contains no copyrighted ROM
or BIOS image. You must supply your own legally obtained dumps. The
`gbarecomp` framework is maintained and licensed separately in its own
repository; third-party components retain their respective licenses.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP">Join the Retro AI Development (R.A.I.D.) Discord</a>
</p>
