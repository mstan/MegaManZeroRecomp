#!/usr/bin/env python3
"""Summarize exact pixel differences between two binary PPM captures."""

from __future__ import annotations

import argparse
from collections import Counter, deque
from pathlib import Path


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    raw = path.read_bytes()
    parts = raw.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6" or parts[2] != b"255":
        raise ValueError(f"unsupported PPM header: {path}")
    width, height = map(int, parts[1].split())
    if len(parts[3]) != width * height * 3:
        raise ValueError(f"wrong RGB payload size: {path}")
    return width, height, parts[3]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    args = parser.parse_args()
    width, height, a = read_ppm(args.a)
    width_b, height_b, b = read_ppm(args.b)
    if (width, height) != (width_b, height_b):
        raise ValueError("dimensions differ")

    changed: set[tuple[int, int]] = set()
    pairs: Counter[tuple[tuple[int, int, int], tuple[int, int, int]]] = Counter()
    deltas: Counter[tuple[int, int, int]] = Counter()
    for p in range(width * height):
        x, y = p % width, p // width
        ca = tuple(a[p * 3:p * 3 + 3])
        cb = tuple(b[p * 3:p * 3 + 3])
        if ca != cb:
            changed.add((x, y))
            pairs[(ca, cb)] += 1
            deltas[tuple(cb[i] - ca[i] for i in range(3))] += 1

    print(f"changed_pixels={len(changed)}")
    if not changed:
        return 0
    xs = [p[0] for p in changed]
    ys = [p[1] for p in changed]
    print(f"bbox={min(xs)},{min(ys)}..{max(xs)},{max(ys)}")
    print("top_color_pairs:")
    for (ca, cb), count in pairs.most_common(20):
        print(f"  {count:5d} {ca} -> {cb}")
    print("top_deltas:")
    for delta, count in deltas.most_common(20):
        print(f"  {count:5d} {delta}")

    remaining = set(changed)
    components: list[list[tuple[int, int]]] = []
    while remaining:
        seed = remaining.pop()
        component = [seed]
        queue = deque([seed])
        while queue:
            x, y = queue.popleft()
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    neighbor = (x + dx, y + dy)
                    if neighbor in remaining:
                        remaining.remove(neighbor)
                        component.append(neighbor)
                        queue.append(neighbor)
        components.append(component)
    components.sort(key=len, reverse=True)
    print(f"components={len(components)}")
    for component in components[:20]:
        cx = [p[0] for p in component]
        cy = [p[1] for p in component]
        print(f"  pixels={len(component):5d} "
              f"bbox={min(cx)},{min(cy)}..{max(cx)},{max(cy)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
