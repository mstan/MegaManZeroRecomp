#!/usr/bin/env python3
"""Classify executed dispatch misses as new roots or interior resume aliases."""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parent.parent
# pathlib under the MSYS Python used by devkitPro can resolve a Windows path
# one directory too high when this script is invoked with backslashes. Anchor
# defaults to the directory that actually owns game.toml when that happens.
if not (ROOT / "game.toml").is_file():
    candidate = pathlib.Path.cwd()
    if (candidate / "game.toml").is_file():
        ROOT = candidate
EXTENT_RE = re.compile(
    r"^/\* 0x([0-9A-Fa-f]{8})  mode=(arm|thumb)  "
    r"end=0x([0-9A-Fa-f]{8})\b",
    re.MULTILINE,
)
LOG_MISS_RE = re.compile(
    r"SELF-HEAL dispatch miss for pc=0x([0-9A-Fa-f]{8}).*?"
    r"\((arm|thumb)\)",
    re.DOTALL,
)


def load_extents(path: pathlib.Path) -> list[tuple[int, int, str]]:
    if path.is_file():
        sources = [path]
    elif path.is_dir():
        sources = sorted(path.glob("recompiled*.cpp"))
    elif path.name == "recompiled.cpp":
        # Stable codegen sharding replaces the legacy monolith with numbered
        # siblings. Preserve the old CLI/default while consuming all shards.
        sources = sorted(path.parent.glob("recompiled_*.cpp"))
    else:
        sources = []
    if not sources:
        raise FileNotFoundError(f"no generated body sources found for {path}")
    extents: list[tuple[int, int, str]] = []
    for source in sources:
        text = source.read_text(encoding="utf-8")
        extents.extend(
            (int(match.group(1), 16), int(match.group(3), 16), match.group(2))
            for match in EXTENT_RE.finditer(text)
        )
    return extents


def containing_extent(
    pc: int, mode: str, extents: list[tuple[int, int, str]]
) -> tuple[int, int, str] | None:
    candidates = [
        extent
        for extent in extents
        if extent[2] == mode and extent[0] < pc < extent[1]
    ]
    return max(candidates, key=lambda extent: extent[0], default=None)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--coverage", type=pathlib.Path, default=ROOT / "recomp_coverage_AZCE.json"
    )
    parser.add_argument(
        "--log",
        type=pathlib.Path,
        help="Parse executed misses from a diagnostic log instead of coverage JSON",
    )
    parser.add_argument(
        "--game-generated",
        type=pathlib.Path,
        default=ROOT / "generated" / "recompiled.cpp",
    )
    parser.add_argument(
        "--bios-generated",
        type=pathlib.Path,
        default=ROOT.parent
        / "gbarecomp"
        / "src"
        / "runtime"
        / "generated_bios"
        / "bios_recompiled.cpp",
    )
    parser.add_argument(
        "--game-out",
        type=pathlib.Path,
        default=ROOT / "build" / "reviewed_game_misses.toml.frag",
    )
    parser.add_argument(
        "--bios-out",
        type=pathlib.Path,
        default=ROOT / "build" / "reviewed_bios_misses.toml.frag",
    )
    args = parser.parse_args()

    game_extents = load_extents(args.game_generated)
    bios_extents = load_extents(args.bios_generated)
    if args.log is not None:
        counts: collections.Counter[tuple[int, str]] = collections.Counter()
        raw_log = args.log.read_bytes()
        log_encoding = (
            "utf-16" if raw_log.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8"
        )
        log_text = raw_log.decode(log_encoding, errors="replace")
        for match in LOG_MISS_RE.finditer(log_text):
            counts[(int(match.group(1), 16), match.group(2))] += 1
        misses = [
            {"pc": f"0x{pc:08X}", "mode": mode, "bridged": count}
            for (pc, mode), count in sorted(counts.items())
        ]
        source_note = f"diagnostic log {args.log.name}"
    else:
        coverage = json.loads(args.coverage.read_text(encoding="utf-8"))
        misses = sorted(
            coverage.get("misses", []), key=lambda item: int(item["pc"], 16)
        )
        source_note = f"coverage run {args.coverage.name}"

    roots = 0
    aliases = 0
    header = [
        f"# Generated review aid; every address was executed in {source_note}.",
        "# Classification is against generated function extents and still requires review.",
        "",
    ]
    game_lines = list(header)
    bios_lines = list(header)
    for miss in misses:
        pc = int(miss["pc"], 16)
        mode = miss["mode"]
        extents = bios_extents if pc < 0x00004000 else game_extents
        host = containing_extent(pc, mode, extents)
        lines = bios_lines if pc < 0x00004000 else game_lines
        lines.extend(("[[extra_func]]", f"addr = 0x{pc:08X}", f'mode = "{mode}"'))
        if host is not None:
            aliases += 1
            lines.append("resume = true")
            lines.append(
                f'note = "Executed interior target; generated host '
                f'0x{host[0]:08X}..0x{host[1]:08X}"'
            )
        else:
            roots += 1
            lines.append('note = "Executed indirect/fall-through target; new static root"')
        lines.extend((f"# bridged_count = {miss.get('bridged', 0)}", ""))

    args.game_out.parent.mkdir(parents=True, exist_ok=True)
    args.game_out.write_text("\n".join(game_lines), encoding="utf-8", newline="\n")
    args.bios_out.write_text("\n".join(bios_lines), encoding="utf-8", newline="\n")
    print(
        f"classified {len(misses)} executed misses: {roots} new roots, "
        f"{aliases} interior aliases\n  game: {args.game_out}\n  bios: {args.bios_out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
