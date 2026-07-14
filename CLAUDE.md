# MegaManZeroRecomp contributor notes

This game repository consumes the sibling `../gbarecomp` engine checkout. Use
`-DGBARECOMP_ROOT=<path>` only when deliberately testing another engine tree.

## Correctness boundary

- LLE is the baseline: execute the real BIOS and original ARM/Thumb guest code.
- HLE is opt-in convenience derived from observed LLE behavior, never the
  correctness oracle.
- Do not edit `generated/*` by hand. Fix metadata or the recompiler, then run
  `tools/regen.ps1`.
- An interpreter bridge is valid for coverage discovery, but not a strict
  static-coverage result.
- Keep ROMs, BIOS images, saves, ROM-derived generated code, release binaries,
  and diagnostic output out of Git.

## Verification

Run `tools/verify-strict.ps1` for the smoke route and the named campaign being
changed. A strict pass must report zero dispatch misses, interpreted
instructions, healed/cache code, unmapped accesses, and unhandled I/O. Visual
or timing claims should cite the independent oracle and the exact checkpoints.

Release archives are built by `tools/make_release.ps1` and must remain free of
ROM, BIOS, save, config, symbol, and generated-source content.
