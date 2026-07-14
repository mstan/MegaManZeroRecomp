# MegaManZeroRecomp

Static recompilation experiment for Mega Man Zero (USA) using gbarecomp. The
baseline is hardware-faithful LLE: the real GBA BIOS and cartridge ARM/Thumb
code execute through the static dispatcher. HLE is not used to establish the
baseline.

## Status

The real BIOS reset path now executes deterministic coverage campaigns using
only statically generated native code:

- `campaign` passes 30,000 frames from reset into the opening mission.
- `campaign-combat` passes 60,000 frames and visibly remains in active combat,
  combining movement, attacks, jumps, and dashes through hardware `KEYINPUT`.
- `campaign-traverse` passes 60,000 frames while driving sustained rightward
  traversal with deterministic attacks, dashes, and jumps. It exercises a much
  broader set of player, enemy, effect, object, renderer, and stage callbacks.
- `campaign-safe` passes 30,000 frames with lower-risk full-height traversal and
  reaches the opening Golem sequence without dash-induced contact pressure.
- `campaign-clear` passes 30,000 frames, triggers the scripted Golem encounter,
  receives the Z-Saber, and exercises its main-weapon B action callback. The
  current deterministic finish attempt dies and automatically retries to the
  pre-trigger room; despite the legacy profile name, Golem defeat and mission
  completion are not yet strict coverage claims.

These strict proofs report zero dispatch misses, interpreted instructions,
healed/cache code, unmapped bus accesses, or unhandled I/O accesses. BIOS HLE,
interpreter bridging, cache loading, and self-healing recompilation are all
disabled. The current reviewed corpus contains 10,885 ARM/Thumb functions,
four observed code-copy mappings, 33 bounded callback/jump-table declarations,
and 42 exact interior resume aliases for observed asynchronous or indirect
control-flow boundaries. This is strong path-specific static closure, not a
whole-game claim.

The engine models the cartridge's hardware SRAM path (blank `0xFF` state, byte
writes, mirroring, and atomic persistence). Strict verification creates and
removes a fresh profile-specific SRAM file so repeated runs do not silently
inherit save state.

An independent NanoBoyAdvance run with its MP2K HLE disabled, the real BIOS,
and fresh blank SRAM independently replayed the same hardware inputs. After
correcting native GBA alpha blending and brightness arithmetic in RGB555 space,
strict recomp frame 14,999 and oracle frame 15,000 are pixel-identical
(`changed_pixels=0`). The one-frame index offset is a presentation-boundary
phase difference; same-index frame 15,000 differs only in 14 animation pixels.
The earlier 1,088-pixel sprite/HUD residual is therefore resolved rather than
masked with an HLE replacement.

The combat oracle remains exact through native frame 9,558. VBlank-updated
state then appears on a different nearby frontend callback
(`native 9,559 == oracle 9,561`, pixel-exact), so the comparison harness records
the best captured oracle frame within a configurable neighborhood instead of
assuming same-index presentation. Near frame 20,000, the best observed pair has
an exact world/background and a 1,253-pixel residual confined to animated
sprites, projectiles, and HUD components. That late-combat timing residual is
still open; it is not represented as full visual parity.

The independent oracle also replays the `campaign-clear` inputs through frame
20,000, immediately before the encounter trigger. Both cores visibly reach the
same state; native/oracle screenshots differ by 19 pixels at frame 16,000 and 14
pixels at frame 20,000. Transition-sensitive checkpoints remain non-exact
(5,480 pixels at frame 14,000, 28,708 at 14,500, and 912 at 18,000), so this is
evidence for the shared pre-trigger route rather than a claim of all-frame or
post-trigger parity.

The campaign also exposed a 0x24-byte position-independent routine copied from
ROM `0x080C8900` to overlapping IWRAM/stack placements. `[[extra_func]]` now
accepts an optional `source_addr`, allowing the function finder to decode an
observed runtime entry from its ROM source while preserving the runtime PC and
relocation bias. A regression test covers the overlapping placements. This
supports the observed LLE pattern; it is not yet a claim of arbitrary dynamic
code relocation.

## Local layout

- Engine worktree: `../gbarecomp-wt-mmz-static` (`codex/mmz-static`)
- ROM: `roms/megaman_zero_usa.gba` (ignored)
- Generated C++: `generated/` (ignored)
- Config and reviewed discovery facts: `game.toml`

## Playable coverage discovery

Run the handoff launcher from this directory:

```powershell
& .\tools\play-discovery.ps1
```

This opens the normal SDL window with real BIOS/LLE boot and persistent SRAM at
`saves/megaman_zero_discovery.sav`. Static native code always runs first. If a
player reaches an uncovered dispatch target, only that gap bridges through the
reference instruction interpreter; native self-healing and cache loading remain
off. Closing the window writes a timestamped directory under
`build/discovery_sessions/` containing the full log, machine-readable coverage,
raw reviewed-proposal fragment, ROM/build identity manifest, frame-indexed
`keyinput.csv`, and exact `initial.sav`/`final.sav` snapshots. The finalized
manifest records the ending PPU frame and hashes the executable, ROM, config,
input trace, and SRAM evidence. Classified game/BIOS review aids are proposals
only; nothing is merged into `game.toml` automatically.

Default keyboard controls are arrows for the D-pad, `Z` for GBA A, `X` for GBA
B, `A` for L, `S` for R, Enter for Start, Right Shift for Select, and hold Tab
for fast-forward. Use `-FreshSave` to discard the persistent discovery SRAM.

Discovery mode is deliberately not an acceptance result when fallback occurs.
After reviewing and folding coverage into static metadata, replay the affected
route with `tools/verify-strict.ps1`; only its zero-miss/zero-interpreter result
counts as fully static coverage.

To replay a player's recorded route after folding its reviewed misses, use the
session manifest's `final_ppu_frame` with the captured trace and initial SRAM:

```powershell
& .\tools\verify-strict.ps1 -Frames <final_ppu_frame> `
  -InputProfile user-replay `
  -InputTrace .\build\discovery_sessions\<session>\keyinput.csv `
  -InitialSave .\build\discovery_sessions\<session>\initial.sav
```

Input traces preserve the last observed host key state at frame granularity and
are intended for a run from their matching initial SRAM. Savestate loads are
not currently encoded in the trace, so avoid using savestates during a coverage
session that must be replayed deterministically.

## Build loop

```powershell
cmake -S ../gbarecomp-wt-mmz-static -B ../gbarecomp-wt-mmz-static/build -G Ninja `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build ../gbarecomp-wt-mmz-static/build --target gba_recompile
pwsh tools/regen.ps1
cmake -S . -B build -G Ninja `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build build --target MegaManZeroRecomp
pwsh tools/verify-strict.ps1
pwsh tools/verify-strict.ps1 -Frames 30000 -InputProfile campaign
pwsh tools/verify-strict.ps1 -Frames 60000 -InputProfile campaign-combat
pwsh tools/verify-strict.ps1 -Frames 60000 -InputProfile campaign-traverse
pwsh tools/verify-strict.ps1 -Frames 30000 -InputProfile campaign-safe
pwsh tools/verify-strict.ps1 -Frames 30000 -InputProfile campaign-clear
python ./tools/compare_nba_lockstep.py --profile campaign-clear `
  --checkpoints 14000,14500,16000,18000,20000 --image-only
```

After every diagnostic run, inspect `recomp_coverage*.json`, the master miss
fragment, and the reviewed seed proposal. "Fully static" requires zero
interpreted PCs and zero healed/cache PCs for that run. Visible-state claims
require a screenshot, and correctness work compares the earliest divergence
against an independent mGBA/NanoBoyAdvance oracle.

`tools/verify-strict.ps1` forces BIOS HLE, cache loading, interpreter bridging,
forced interpretation, and self-healing off. It fails unless the runtime emits the exact LLE and
zero/zero/zero proof markers, and writes a profile-qualified log and PNG under
`build/` so one coverage path cannot overwrite another path's evidence.

`tools/run-nba-oracle.ps1` performs the independent blank-SRAM replay against
the isolated NanoBoyAdvance oracle at `../_nba_oracle`. It captures the target
frame and both neighbors so presentation-boundary differences cannot hide a
visual divergence. The oracle exposes only hardware input and frame stepping;
it does not replace guest logic with HLE.

Generated guest code is deterministically split into 16 stable translation
units. Unchanged regeneration touches no shards; adding one local resume alias
rebuilds only its address-hashed shard. This reduced a full first build from a
terminated five-minute monolith to about one minute, with later reviewed
iterations typically taking tens of seconds.
