#!/usr/bin/env python3
"""Drive gbarecomp and NanoBoyAdvance with identical frame-indexed KEYINPUT.

Both cores stay alive for the whole run.  At selected frame boundaries this
captures the presented RGB image plus hardware-visible memory regions, making
the earliest divergence search reproducible without repeated boot replays.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time


RELEASED = 0x03FF
BUTTONS = (0x008, 0x001, 0x001, 0x080, 0x001, 0x010,
           0x002, 0x001, 0x020, 0x001, 0x040, 0x002)
DIRECTIONS = (0x010, 0x080, 0x020, 0x040)


def demo_key(frame: int) -> int:
    if (frame // 6) & 1:
        return RELEASED
    return RELEASED & ~BUTTONS[(frame // 12) % len(BUTTONS)]


def walk_key(frame: int) -> int:
    button = DIRECTIONS[(frame // 180) % len(DIRECTIONS)]
    if frame % 60 < 2:
        button |= 0x001
    return RELEASED & ~button


def campaign_key(frame: int) -> int:
    return walk_key(frame - 9000) if frame >= 9000 else demo_key(frame)


def combat_key(frame: int) -> int:
    button = 0x010  # right
    if frame % 12 < 3:
        button |= 0x002  # B / main attack
    motion_phase = frame % 180
    if motion_phase < 50:
        button |= 0x200  # L / dash
    if 30 <= motion_phase < 40:
        button |= 0x001  # A / jump
    return RELEASED & ~button


def campaign_combat_key(frame: int) -> int:
    return combat_key(frame - 9000) if frame >= 9000 else demo_key(frame)


def traverse_key(frame: int) -> int:
    button = 0x010  # right
    if frame % 10 < 3:
        button |= 0x002  # B / main attack
    motion_phase = frame % 120
    if motion_phase < 75:
        button |= 0x200  # L / dash
    if 18 <= motion_phase < 50:
        button |= 0x001  # A / full-height jump
    return RELEASED & ~button


def campaign_traverse_key(frame: int) -> int:
    return traverse_key(frame - 9000) if frame >= 9000 else demo_key(frame)


def safe_key(frame: int) -> int:
    button = 0x010  # right
    if frame % 10 < 3:
        button |= 0x002  # B / main attack
    motion_phase = frame % 120
    if 18 <= motion_phase < 50:
        button |= 0x001  # A / full-height jump
    return RELEASED & ~button


def campaign_safe_key(frame: int) -> int:
    return safe_key(frame - 9000) if frame >= 9000 else demo_key(frame)


def saber_jump_key(frame: int) -> int:
    button = 0
    motion_phase = frame % 120
    if 18 <= motion_phase < 50:
        button |= 0x001  # A / full-height jump
    if 30 <= motion_phase < 38:
        button |= 0x100  # R / Z-Saber in the subweapon slot
    return RELEASED & ~button


def golem_attempt_key(frame: int) -> int:
    if frame < 5500:
        return saber_jump_key(frame)

    encounter = frame - 5500
    button = 0
    if encounter < 54:
        button |= 0x010  # right / trigger encounter
    elif encounter < 700:
        if (encounter - 54) % 12 < 4:
            button |= 0x001  # A / advance scripted sequence
    elif encounter < 4200:
        phase = (encounter - 700) % 120
        if 18 <= phase < 50:
            button |= 0x001
        if 30 <= phase < 38:
            button |= 0x100
    else:
        finish = encounter - 4200
        if finish < 120:
            if finish % 12 < 4:
                button |= 0x001
        else:
            active = finish - 120
            if active < 70:
                button |= 0x010
            if active >= 50 and active % 12 < 4:
                button |= 0x002  # B / Z-Saber main weapon
            phase = active % 120
            if 18 <= phase < 50:
                button |= 0x001
    return RELEASED & ~button


def campaign_clear_key(frame: int) -> int:
    if frame >= 14500:
        return golem_attempt_key(frame - 14500)
    return campaign_safe_key(frame)


class JsonLineClient:
    def __init__(self, port: int, timeout: float = 15.0):
        deadline = time.monotonic() + timeout
        last_error: OSError | None = None
        while time.monotonic() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 0.5)
                self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.sock.settimeout(120.0)
                self.stream = self.sock.makefile("rwb", buffering=0)
                return
            except OSError as exc:
                last_error = exc
                time.sleep(0.05)
        raise RuntimeError(f"port {port} did not accept a connection: {last_error}")

    def call(self, request: dict) -> dict:
        payload = json.dumps(request, separators=(",", ":")).encode() + b"\n"
        self.stream.write(payload)
        line = self.stream.readline()
        if not line:
            raise RuntimeError(f"server closed after {request}")
        reply = json.loads(line)
        if not reply.get("ok", False):
            raise RuntimeError(f"request failed: {request}: {reply}")
        return reply

    def close(self) -> None:
        try:
            self.stream.close()
        finally:
            self.sock.close()


def write_ppm(path: Path, rgb: bytes, width: int, height: int) -> None:
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)


def image_metrics(a: bytes, b: bytes) -> dict:
    if len(a) != len(b):
        return {"same_size": False, "bytes_a": len(a), "bytes_b": len(b)}
    if a == b:
        return {
            "same_size": True,
            "differing_pixels": 0,
            "differing_channels": 0,
            "max_channel_delta": 0,
            "normalized_rmse": 0.0,
            "bbox": None,
        }
    pixels = len(a) // 3
    differing_pixels = 0
    differing_channels = 0
    squared = 0
    max_delta = 0
    bbox = [10**9, 10**9, -1, -1]
    for p in range(pixels):
        changed = False
        for channel in range(3):
            delta = abs(a[p * 3 + channel] - b[p * 3 + channel])
            if delta:
                changed = True
                differing_channels += 1
                max_delta = max(max_delta, delta)
            squared += delta * delta
        if changed:
            differing_pixels += 1
            x, y = p % 240, p // 240
            bbox[0] = min(bbox[0], x)
            bbox[1] = min(bbox[1], y)
            bbox[2] = max(bbox[2], x)
            bbox[3] = max(bbox[3], y)
    return {
        "same_size": True,
        "differing_pixels": differing_pixels,
        "differing_channels": differing_channels,
        "max_channel_delta": max_delta,
        "normalized_rmse": math.sqrt(squared / max(1, len(a))) / 255.0,
        "bbox": bbox if differing_pixels else None,
    }


def region_metrics(a: bytes, b: bytes) -> dict:
    first = next((i for i, pair in enumerate(zip(a, b)) if pair[0] != pair[1]), None)
    different = sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
    return {
        "equal": a == b,
        "different_bytes": different,
        "first_difference": first,
        "native_sha1": hashlib.sha1(a).hexdigest(),
        "oracle_sha1": hashlib.sha1(b).hexdigest(),
    }


def read_region(client: JsonLineClient, native: bool, name: str,
                address: int, length: int) -> bytes:
    if native:
        command = {"cmd": f"read_{name}", "addr": address, "len": length}
    else:
        oracle_name = "pram" if name == "pal" else name
        command = {"cmd": "read", "region": oracle_name,
                   "addr": address, "len": length}
    return bytes.fromhex(client.call(command)["data"])


def parse_checkpoints(text: str) -> list[int]:
    values = sorted({int(value.strip(), 0) for value in text.split(",") if value.strip()})
    if not values or values[0] < 0:
        raise argparse.ArgumentTypeError("checkpoints must be non-negative")
    return values


def decode_le_series(text: str, width: int) -> list[int]:
    raw = bytes.fromhex(text)
    if len(raw) % width:
        raise RuntimeError(f"packed MMIO field has partial {width}-byte value")
    return [int.from_bytes(raw[i:i + width], "little")
            for i in range(0, len(raw), width)]


def decode_oracle_mmio(response: dict) -> list[dict]:
    cycles = decode_le_series(response["cyc"], 8)
    addresses = decode_le_series(response["addr"], 4)
    values = decode_le_series(response["val"], 4)
    pcs = decode_le_series(response["pc"], 4)
    sizes = decode_le_series(response["size"], 1)
    count = response["count"]
    if not all(len(field) == count for field in
               (cycles, addresses, values, pcs, sizes)):
        raise RuntimeError("oracle MMIO fields disagree with reported count")
    return [
        {"cycle": cycles[i], "addr": addresses[i], "value": values[i],
         "size": sizes[i], "pc": pcs[i]}
        for i in range(count)
    ]


def main() -> int:
    here = Path(__file__).resolve().parent
    repo = here.parent
    workspace = repo.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoints", type=parse_checkpoints,
                        default=parse_checkpoints(
                            "0,1,60,600,1200,2400,3600,4800,6000,7200,"
                            "8400,8800,8900,8950,8980,8990,8998,8999,9000,"
                            "9001,9010,9060,9180,9360,9720,10000"))
    parser.add_argument("--native-port", type=int, default=19868)
    parser.add_argument("--oracle-port", type=int, default=19869)
    parser.add_argument("--native-preroll", type=int, default=1)
    # NBA's first fixed-cycle frame call presents its startup buffer one callback
    # later than gbarecomp's first VBlank-start step. Two oracle calls align the
    # first animated BIOS frame; later reports still retain neighboring frames.
    parser.add_argument("--oracle-preroll", type=int, default=2)
    parser.add_argument("--neighbor-radius", type=int, default=3,
                        help="search captured oracle frames within +/- this radius")
    parser.add_argument("--profile", choices=("campaign", "campaign-combat",
                                               "campaign-traverse", "campaign-safe",
                                               "campaign-clear"),
                        default="campaign")
    parser.add_argument("--native", type=Path,
                        default=repo / "build" / "MegaManZeroRecomp.exe")
    parser.add_argument("--oracle", type=Path,
                        default=workspace / "_nba_oracle" / "build" / "nba_oracle.exe")
    parser.add_argument("--bios", type=Path,
                        default=workspace / "gbarecomp-wt-mmz-static" / "bios" / "gba_bios.bin")
    parser.add_argument("--rom", type=Path,
                        default=repo / "roms" / "megaman_zero_usa.gba")
    parser.add_argument("--image-only", action="store_true",
                        help="skip memory/register captures during a dense frame sweep")
    parser.add_argument("--save-regions", action="store_true",
                        help="retain raw native/oracle region bytes for cross-frame diffing")
    parser.add_argument("--trace-mmio-intervals", action="store_true",
                        help="save each core's MMIO writes between adjacent checkpoints")
    args = parser.parse_args()
    if args.neighbor_radius < 0:
        parser.error("--neighbor-radius must be non-negative")

    tag = time.strftime("%Y%m%d-%H%M%S")
    run_dir = repo / "build" / "lockstep_runs" / tag
    run_dir.mkdir(parents=True)
    oracle_rom = run_dir / "megaman_zero_blank.gba"
    shutil.copyfile(args.rom, oracle_rom)
    native_save = run_dir / "native_blank.sav"

    native_out = (run_dir / "native.stdout.log").open("wb")
    native_err = (run_dir / "native.stderr.log").open("wb")
    oracle_out = (run_dir / "oracle.stdout.log").open("wb")
    oracle_err = (run_dir / "oracle.stderr.log").open("wb")
    env = os.environ.copy()
    env.update({
        "GBARECOMP_BIOS_HLE": "0",
        "GBARECOMP_STRICT_STATIC": "1",
        "GBARECOMP_SELFHEAL_RECOMPILE": "0",
        "GBARECOMP_HANG_WATCHDOG": "0",
    })
    env.pop("GBARECOMP_DEMO_INPUT", None)
    creationflags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
    native_proc = subprocess.Popen(
        [str(args.native), "--tcp", str(args.native_port), "--save", str(native_save),
         str(repo / "game.toml")], cwd=repo, env=env,
        stdout=native_out, stderr=native_err, creationflags=creationflags)
    oracle_proc = subprocess.Popen(
        [str(args.oracle), str(args.bios), str(oracle_rom), "--port",
         str(args.oracle_port)], cwd=run_dir,
        stdout=oracle_out, stderr=oracle_err, creationflags=creationflags)

    native = oracle = None
    report = {"run_dir": str(run_dir), "checkpoints": []}
    try:
        native = JsonLineClient(args.native_port)
        oracle = JsonLineClient(args.oracle_port)
        # Establish the same pre-input observation boundary on both cores.
        native.call({"cmd": "run_frames", "n": args.native_preroll,
                     "keyinput": RELEASED})
        oracle.call({"cmd": "run_frames", "n": args.oracle_preroll,
                     "keyinput": RELEASED})
        current = 0
        native_images: dict[int, bytes] = {}
        oracle_images: dict[int, bytes] = {}
        native_mmio_head: int | None = None
        oracle_mmio_head: int | None = None
        previous_checkpoint: int | None = None

        key_for_frame = (
            campaign_combat_key if args.profile == "campaign-combat" else
            campaign_clear_key if args.profile == "campaign-clear" else
            campaign_safe_key if args.profile == "campaign-safe" else
            campaign_traverse_key if args.profile == "campaign-traverse" else
            campaign_key)
        for target in args.checkpoints:
            while current < target:
                key = key_for_frame(current)
                count = 1
                while (current + count < target and
                       key_for_frame(current + count) == key):
                    count += 1
                native.call({"cmd": "run_frames", "n": count, "keyinput": key})
                oracle.call({"cmd": "run_frames", "n": count, "keyinput": key})
                current += count

            native_shot = native.call({"cmd": "screenshot"})
            oracle_shot = oracle.call({"cmd": "screenshot"})
            native_rgb = bytes.fromhex(native_shot["data"])
            oracle_rgb = bytes.fromhex(oracle_shot["data"])
            native_images[target] = native_rgb
            oracle_images[target] = oracle_rgb
            write_ppm(run_dir / f"native_f{target}.ppm", native_rgb,
                      native_shot["w"], native_shot["h"])
            write_ppm(run_dir / f"oracle_f{target}.ppm", oracle_rgb,
                      oracle_shot["w"], oracle_shot["h"])
            item = {"frame": target, "image": image_metrics(native_rgb, oracle_rgb)}
            regions = {}
            if not args.image_only:
                for name, address, length in (
                    ("oam", 0x07000000, 0x400),
                    ("pal", 0x05000000, 0x400),
                    ("vram", 0x06000000, 0x18000),
                    ("iwram", 0x03000000, 0x8000),
                    ("ewram", 0x02000000, 0x40000),
                    ("io", 0x04000000, 0x60),
                ):
                    a = read_region(native, True, name, address, length)
                    b = read_region(oracle, False, name, address, length)
                    if args.save_regions:
                        (run_dir / f"native_{name}_f{target}.bin").write_bytes(a)
                        (run_dir / f"oracle_{name}_f{target}.bin").write_bytes(b)
                    regions[name] = region_metrics(a, b)
                item["regions"] = regions
                item["native_registers"] = native.call({"cmd": "registers"})
                item["oracle_registers"] = oracle.call({"cmd": "registers"})
            if args.trace_mmio_intervals:
                if native_mmio_head is None:
                    native_cap = native.call({"cmd": "mmio_cap", "count": 0})
                    oracle_cap = oracle.call({"cmd": "mmio_cap", "count": 0})
                else:
                    native_cap = native.call({"cmd": "mmio_cap",
                                              "start": native_mmio_head,
                                              "count": 65536})
                    oracle_cap = oracle.call({"cmd": "mmio_cap",
                                              "start": oracle_mmio_head,
                                              "count": 65536})
                    native_entries = native_cap["entries"]
                    oracle_entries = decode_oracle_mmio(oracle_cap)
                    stem = f"mmio_f{previous_checkpoint}_to_f{target}"
                    native_name = f"native_{stem}.json"
                    oracle_name = f"oracle_{stem}.json"
                    (run_dir / native_name).write_text(
                        json.dumps(native_entries, indent=2))
                    (run_dir / oracle_name).write_text(
                        json.dumps(oracle_entries, indent=2))
                    item["mmio_since_previous"] = {
                        "from_frame": previous_checkpoint,
                        "native_count": len(native_entries),
                        "oracle_count": len(oracle_entries),
                        "native_file": native_name,
                        "oracle_file": oracle_name,
                    }
                native_mmio_head = native_cap["total"]
                oracle_mmio_head = oracle_cap["head"]
                previous_checkpoint = target
            report["checkpoints"].append(item)
            image = item["image"]
            memory_summary = ""
            if regions:
                memory_summary = (
                    f" oam={regions['oam']['different_bytes']}"
                    f" vram={regions['vram']['different_bytes']}"
                    f" iwram={regions['iwram']['different_bytes']}"
                    f" ewram={regions['ewram']['different_bytes']}")
            print(f"frame={target} pixels={image.get('differing_pixels')}"
                  f"{memory_summary}", flush=True)
            (run_dir / "comparison.json").write_text(json.dumps(report, indent=2))

        # A frame number is an observation boundary, and the two frontends can
        # expose VBlank-updated state on different nearby callbacks. Report the
        # best captured oracle frame explicitly so exact parity cannot be hidden
        # by presentation phase (or inferred from a same-index pair).
        for item in report["checkpoints"]:
            target = item["frame"]
            candidates = []
            for oracle_frame in range(target - args.neighbor_radius,
                                      target + args.neighbor_radius + 1):
                if oracle_frame not in oracle_images:
                    continue
                metrics = image_metrics(native_images[target],
                                        oracle_images[oracle_frame])
                candidates.append((metrics.get("differing_pixels", 10**9),
                                   metrics.get("normalized_rmse", float("inf")),
                                   oracle_frame, metrics))
            if not candidates:
                continue
            _, _, oracle_frame, metrics = min(candidates)
            item["best_oracle_neighbor"] = {
                "oracle_frame": oracle_frame,
                "image": metrics,
            }
            print(f"native_frame={target} best_oracle_frame={oracle_frame} "
                  f"pixels={metrics.get('differing_pixels')}", flush=True)
        (run_dir / "comparison.json").write_text(json.dumps(report, indent=2))

        native.call({"cmd": "quit"})
        oracle.call({"cmd": "quit"})
        native_proc.wait(10)
        oracle_proc.wait(10)
        print(f"RUN_DIR={run_dir}")
        return 0
    finally:
        for client in (native, oracle):
            if client is not None:
                try:
                    client.close()
                except OSError:
                    pass
        for proc in (native_proc, oracle_proc):
            if proc.poll() is None:
                proc.kill()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                pass
        for stream in (native_out, native_err, oracle_out, oracle_err):
            stream.close()


if __name__ == "__main__":
    raise SystemExit(main())
