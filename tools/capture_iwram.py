#!/usr/bin/env python3
"""Capture live IWRAM over the debug socket and locate ROM code copies."""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import subprocess
import time


ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build" / "MegaManZeroRecomp.exe"
DEFAULT_ROM = ROOT / "roms" / "megaman_zero_usa.gba"
TARGETS = (0x0000, 0x03B8, 0x04AC, 0x0A5C)


class Client:
    def __init__(self, port: int) -> None:
        deadline = time.time() + 15.0
        last_error: OSError | None = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 2.0)
                self.sock.settimeout(None)
                self.buf = b""
                return
            except OSError as error:
                last_error = error
                time.sleep(0.1)
        raise RuntimeError(f"cannot connect to TCP port {port}: {last_error}")

    def call(self, **request: object) -> dict:
        self.sock.sendall(json.dumps(request).encode("utf-8") + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("runtime closed the debug socket")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def read_iwram(self) -> bytes:
        result = bytearray()
        for offset in range(0, 32 * 1024, 4096):
            response = self.call(cmd="read_iwram", addr=offset, len=4096)
            if not response.get("ok"):
                raise RuntimeError(f"read_iwram at 0x{offset:04X}: {response}")
            result += bytes.fromhex(response["data"])
        return bytes(result)

    def close(self) -> None:
        try:
            self.call(cmd="quit")
        except (OSError, RuntimeError):
            pass
        self.sock.close()


def locate_copy(iwram: bytes, rom: bytes, target: int) -> None:
    matches: list[int] = []
    probe_size = 0
    for candidate_size in (64, 48, 32, 24, 16):
        probe = iwram[target : target + candidate_size]
        matches = []
        cursor = 0
        while True:
            found = rom.find(probe, cursor)
            if found < 0:
                break
            matches.append(found)
            cursor = found + 1
        if len(matches) == 1:
            probe_size = candidate_size
            break

    if len(matches) != 1:
        print(f"0x{0x03000000 + target:08X}: no unique ROM match down to 16 bytes")
        return

    source = matches[0]
    left = 0
    while target + left > 0 and source + left > 0:
        if iwram[target + left - 1] != rom[source + left - 1]:
            break
        left -= 1
    right = probe_size
    while target + right < len(iwram) and source + right < len(rom):
        if iwram[target + right] != rom[source + right]:
            break
        right += 1

    print(
        f"0x{0x03000000 + target:08X}: ROM 0x{0x08000000 + source:08X}; "
        f"maximal exact run runtime=0x{0x03000000 + target + left:08X} "
        f"source=0x{0x08000000 + source + left:08X} size=0x{right - left:X}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--port", type=int, default=19862)
    parser.add_argument("--exe", type=pathlib.Path, default=DEFAULT_EXE)
    parser.add_argument("--rom", type=pathlib.Path, default=DEFAULT_ROM)
    args = parser.parse_args()

    process = subprocess.Popen(
        [str(args.exe), "--tcp", str(args.port)],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    client = Client(args.port)
    try:
        frame = client.call(cmd="frame")["frame"]
        while frame < args.frames:
            frame = client.call(cmd="step")["frame"]
        iwram = client.read_iwram()
    finally:
        client.close()
        process.wait(timeout=10)

    output = ROOT / "build" / f"mmz_iwram_f{args.frames}.bin"
    output.write_bytes(iwram)
    print(f"captured {len(iwram)} IWRAM bytes at frame {frame}: {output}")

    rom = args.rom.read_bytes()
    for target in TARGETS:
        locate_copy(iwram, rom, target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
