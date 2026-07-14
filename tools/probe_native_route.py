#!/usr/bin/env python3
"""Branch deterministic native input experiments from one strict LLE snapshot."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from compare_nba_lockstep import (
    JsonLineClient,
    RELEASED,
    campaign_clear_key,
    campaign_traverse_key,
    write_ppm,
)


KEY_A = 1 << 0
KEY_B = 1 << 1
KEY_RIGHT = 1 << 4
KEY_LEFT = 1 << 5
KEY_UP = 1 << 6
KEY_DOWN = 1 << 7
KEY_R = 1 << 8
KEY_L = 1 << 9


def pressed(*buttons: int) -> int:
    value = 0
    for button in buttons:
        value |= button
    return RELEASED & ~value


def strategy_key(name: str, frame: int) -> int:
    attack = (frame % 10) < 3
    buttons: list[int] = [KEY_B] if attack else []

    if name == "stand_fire":
        pass
    elif name == "retreat_fire":
        if frame < 180:
            buttons.append(KEY_LEFT)
    elif name == "jump_fire":
        if 24 <= (frame % 120) < 42:
            buttons.append(KEY_A)
    elif name == "retreat_jump_fire":
        if frame < 180:
            buttons.append(KEY_LEFT)
        if 24 <= (frame % 120) < 42:
            buttons.append(KEY_A)
    elif name == "dash_retreat_fire":
        if frame < 90:
            buttons.extend((KEY_LEFT, KEY_L))
        if 24 <= (frame % 120) < 42:
            buttons.append(KEY_A)
    elif name == "baseline":
        return campaign_traverse_key(frame + 13_000)
    elif name in ("golem_saber_jump", "golem_saber_forward",
                  "golem_charge_forward"):
        # From the stable pre-trigger Golem room: approach Ciel, advance the
        # scripted encounter, then attack with the Z-Saber subweapon (R).
        buttons = []
        if frame < 54:
            buttons.append(KEY_RIGHT)
        elif frame < 700:
            if ((frame - 54) % 12) < 4:
                buttons.append(KEY_A)
        elif frame < 4200 or name == "golem_saber_jump":
            phase = (frame - 700) % 120
            if 18 <= phase < 50:
                buttons.append(KEY_A)
            if 30 <= phase < 38:
                buttons.append(KEY_R)
        else:
            # The scripted X dialogue has handed over the saber by this point.
            # Close the range so the melee weapon can connect.
            phase = (frame - 4200) % 120
            buttons.append(KEY_RIGHT)
            if 24 <= phase < 60:
                buttons.append(KEY_A)
            if name == "golem_saber_forward":
                if 30 <= phase < 38:
                    buttons.append(KEY_R)
            else:  # golem_charge_forward
                if phase < 72:
                    buttons.append(KEY_R)
    elif name in ("saber_finish_rush", "saber_finish_jump",
                  "saber_finish_dash"):
        # Branch begins on the "You Got the Z Saber" text. Dismiss it, close
        # range immediately, and slash before the scripted low-health Zero can
        # take another hit.
        buttons = []
        if frame < 120:
            if (frame % 12) < 4:
                buttons.append(KEY_A)
        else:
            buttons.append(KEY_RIGHT)
            if ((frame - 120) % 12) < 4:
                buttons.append(KEY_R)
            if name == "saber_finish_jump" and 12 <= ((frame - 120) % 90) < 46:
                buttons.append(KEY_A)
            if name == "saber_finish_dash" and frame < 180:
                buttons.append(KEY_L)
    elif name in ("saber_finish_precise_r", "saber_finish_precise_b",
                  "saber_finish_precise_jump_r", "saber_finish_charge_r"):
        buttons = []
        if frame < 120:
            if (frame % 12) < 4:
                buttons.append(KEY_A)
        else:
            active = frame - 120
            if active < 52:
                buttons.append(KEY_RIGHT)
            if name == "saber_finish_charge_r":
                if active < 48:
                    buttons.append(KEY_R)
            elif 38 <= active < 46:
                buttons.append(KEY_B if name == "saber_finish_precise_b" else KEY_R)
            if name == "saber_finish_precise_jump_r" and active < 44:
                buttons.append(KEY_A)
    elif name in ("saber_finish_repeat_r", "saber_finish_repeat_b"):
        buttons = []
        if frame < 120:
            if (frame % 12) < 4:
                buttons.append(KEY_A)
        else:
            active = frame - 120
            if active < 70:
                buttons.append(KEY_RIGHT)
            if active >= 50 and (active % 12) < 4:
                buttons.append(KEY_R if name.endswith("_r") else KEY_B)
            phase = active % 120
            if 18 <= phase < 50:
                buttons.append(KEY_A)
    elif name in ("saber_finish_wall_dash_b", "saber_finish_wall_dash_r"):
        buttons = []
        if frame < 120:
            if (frame % 12) < 4:
                buttons.append(KEY_A)
        else:
            active = frame - 120
            if active < 42:
                buttons.append(KEY_LEFT)
            elif active < 78:
                buttons.extend((KEY_LEFT, KEY_A))
            elif active < 150:
                buttons.extend((KEY_RIGHT, KEY_L, KEY_A))
                if 30 <= active - 78 < 46:
                    buttons.append(KEY_R if name.endswith("_r") else KEY_B)
            elif (active % 120) < 4:
                buttons.append(KEY_R if name.endswith("_r") else KEY_B)
    elif name == "post_boss_advance":
        buttons = [KEY_A] if (frame % 12) < 4 else []
    elif name.startswith("hub_"):
        # Branches begin beside Ciel in the ruined lab. Depending on the source
        # checkpoint this may be the original trigger or an automatic retry;
        # do not infer mission completion from the absent Golem alone. They use
        # KEYINPUT only and avoid guest-memory/state-dependent steering.
        buttons = []
        if frame < 54:
            buttons.append(KEY_RIGHT)  # approach Ciel
        elif frame < 62:
            buttons.append(KEY_UP)     # initiate conversation
        else:
            phase = (frame - 62) % 48
            if name == "hub_accept_a":
                if phase < 4:
                    buttons.append(KEY_A)
            elif name == "hub_accept_up_a":
                if phase < 4:
                    buttons.append(KEY_UP)
                elif 12 <= phase < 16:
                    buttons.append(KEY_A)
            elif name == "hub_accept_down_a":
                if phase < 4:
                    buttons.append(KEY_DOWN)
                elif 12 <= phase < 16:
                    buttons.append(KEY_A)
            elif name == "hub_accept_mash":
                slot = (frame - 62) // 12
                sequence = (KEY_A, KEY_UP, KEY_A, KEY_DOWN)
                if phase % 12 < 4:
                    buttons.append(sequence[slot % len(sequence)])
            else:
                raise ValueError(f"unknown strategy: {name}")
    else:
        raise ValueError(f"unknown strategy: {name}")
    return pressed(*buttons)


def run_rle(client: JsonLineClient, start: int, count: int, key_fn) -> None:
    current = start
    end = start + count
    while current < end:
        key = key_fn(current)
        run = 1
        while current + run < end and key_fn(current + run) == key:
            run += 1
        reply = client.call({"cmd": "run_frames", "n": run, "keyinput": key})
        if not reply.get("ok", False):
            raise RuntimeError(f"run_frames failed at {current}: {reply}")
        current += run


def capture(client: JsonLineClient, path: Path) -> None:
    shot = client.call({"cmd": "screenshot"})
    write_ppm(path, bytes.fromhex(shot["data"]), shot["w"], shot["h"])


def main() -> int:
    here = Path(__file__).resolve().parent
    repo = here.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", type=int, default=16_000)
    parser.add_argument("--branch-frames", type=int, default=4_000)
    parser.add_argument("--capture-every", type=int, default=500)
    parser.add_argument("--port", type=int, default=19870)
    parser.add_argument("--native", type=Path,
                        default=repo / "build" / "MegaManZeroRecomp.exe")
    parser.add_argument("--load-checkpoint", type=Path,
                        help="reuse a prior TCP savestate instead of replaying from reset")
    parser.add_argument("--load-checkpoint-frame", type=int, default=0,
                        help="global input frame represented by --load-checkpoint")
    parser.add_argument("--base-profile", choices=("campaign-clear", "campaign-traverse"),
                        default="campaign-clear")
    parser.add_argument("--strategies", default="golem_saber_jump")
    args = parser.parse_args()

    strategies = [s.strip() for s in args.strategies.split(",") if s.strip()]
    tag = time.strftime("%Y%m%d-%H%M%S")
    run_dir = repo / "build" / "native_route_probes" / tag
    run_dir.mkdir(parents=True)
    save = run_dir / "blank.sav"
    snapshot = run_dir / "checkpoint.gbs"
    snapshot_request = snapshot.relative_to(repo).as_posix()
    stdout = (run_dir / "native.stdout.log").open("wb")
    stderr = (run_dir / "native.stderr.log").open("wb")

    env = os.environ.copy()
    env.update({
        "GBARECOMP_BIOS_HLE": "0",
        "GBARECOMP_STRICT_STATIC": "1",
        "GBARECOMP_SELFHEAL_RECOMPILE": "0",
        "GBARECOMP_HANG_WATCHDOG": "0",
    })
    env.pop("GBARECOMP_DEMO_INPUT", None)
    creationflags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
    proc = subprocess.Popen(
        [str(args.native), "--tcp", str(args.port), "--save", str(save),
         str(repo / "game.toml")], cwd=repo, env=env,
        stdout=stdout, stderr=stderr, creationflags=creationflags)

    client = None
    report = {"run_dir": str(run_dir), "checkpoint": args.checkpoint,
              "branch_frames": args.branch_frames, "strategies": []}
    try:
        client = JsonLineClient(args.port)
        base_key = (campaign_clear_key if args.base_profile == "campaign-clear"
                    else campaign_traverse_key)
        if args.load_checkpoint:
            source = args.load_checkpoint.resolve()
            try:
                source_request = source.relative_to(repo.resolve()).as_posix()
            except ValueError as exc:
                raise RuntimeError("--load-checkpoint must be inside the game repo") from exc
            loaded = client.call({"cmd": "savestate_load", "path": source_request})
            if not loaded.get("ok", False):
                raise RuntimeError(f"initial savestate_load failed: {loaded}")
            if args.load_checkpoint_frame > args.checkpoint:
                raise RuntimeError("--load-checkpoint-frame exceeds --checkpoint")
            run_rle(client, args.load_checkpoint_frame,
                    args.checkpoint - args.load_checkpoint_frame, base_key)
        else:
            client.call({"cmd": "run_frames", "n": 1, "keyinput": RELEASED})
            run_rle(client, 0, args.checkpoint, base_key)
        capture(client, run_dir / f"checkpoint_f{args.checkpoint}.ppm")
        saved = client.call({"cmd": "savestate_save", "path": snapshot_request})
        if not saved.get("ok", False):
            raise RuntimeError(f"savestate_save failed: {saved}")

        for strategy in strategies:
            loaded = client.call({"cmd": "savestate_load", "path": snapshot_request})
            if not loaded.get("ok", False):
                raise RuntimeError(f"savestate_load failed: {loaded}")
            elapsed = 0
            while elapsed < args.branch_frames:
                batch = min(args.capture_every, args.branch_frames - elapsed)
                run_rle(client, elapsed, batch,
                        lambda frame, name=strategy: strategy_key(name, frame))
                elapsed += batch
                capture(client, run_dir / f"{strategy}_plus{elapsed}.ppm")
            final_snapshot = run_dir / f"{strategy}_final.gbs"
            final_request = final_snapshot.relative_to(repo).as_posix()
            final_saved = client.call({"cmd": "savestate_save", "path": final_request})
            if not final_saved.get("ok", False):
                raise RuntimeError(f"final savestate_save failed: {final_saved}")
            report["strategies"].append({"name": strategy, "frames": elapsed})
            (run_dir / "report.json").write_text(
                json.dumps(report, indent=2) + "\n", encoding="utf-8")
    finally:
        if client is not None:
            try:
                client.close()
            except OSError:
                pass
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        stdout.close()
        stderr.close()
        save.unlink(missing_ok=True)

    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
