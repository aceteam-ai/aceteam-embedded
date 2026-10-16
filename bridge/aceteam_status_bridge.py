#!/usr/bin/env python3
"""AceTeam Status host bridge (phase 1, mock).

Pushes a snapshot of live status (agents running, PRs awaiting review, CI
red/green, ACET balance) to the AceTeam Status app on a Flipper Zero, which
renders it as a compact scrollable list.

This is the host side of the companion dashboard. In this mock the status is
canned; to wire it into the real product, replace build_mock_status() with an
aceteam read (waves, PRs, CI, balance) and keep the same wire format.

Honesty note: this is a read-only display. The device does not verify the
numbers, authenticate this host, or prove the state is current. Signing pushed
state would ride on the same work as the Beacon's L3 rung (see the README).

Wire protocol (host -> device), line oriented, each line ends with '\\n':
  one item per line:  <kind><space><text>
    kind: h=header  g=good  r=bad  w=warn  i=info
  a line that is a single '.' ends the snapshot and triggers a redraw.

Run:
  python3 bridge/aceteam_status_bridge.py                 # print the wire bytes, no hardware
  python3 bridge/aceteam_status_bridge.py --selftest      # round-trip check, no hardware
  python3 bridge/aceteam_status_bridge.py --port /dev/ttyACM0          # push once
  python3 bridge/aceteam_status_bridge.py --port /dev/ttyACM0 --watch 5  # refresh every 5s
"""
from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass

KINDS = {"h": "header", "g": "good", "r": "bad", "w": "warn", "i": "info"}


@dataclass(frozen=True)
class StatusItem:
    kind: str  # one of KINDS
    text: str

    def __post_init__(self) -> None:
        if self.kind not in KINDS:
            raise ValueError(f"unknown kind {self.kind!r}; expected one of {sorted(KINDS)}")
        if "\n" in self.text or "\r" in self.text:
            raise ValueError("item text may not contain newlines")


def build_mock_status() -> list[StatusItem]:
    """The canned snapshot. Swap this for a real aceteam read later."""
    return [
        StatusItem("h", "Waves"),
        StatusItem("g", "3 agents running"),
        StatusItem("w", "2 PRs awaiting review"),
        StatusItem("h", "CI"),
        StatusItem("g", "main green"),
        StatusItem("r", "staging red (1 job)"),
        StatusItem("h", "Account"),
        StatusItem("i", "ACET balance 142.5"),
    ]


def format_snapshot(items: list[StatusItem]) -> bytes:
    """Encode items into the device wire format, terminated by a '.' line."""
    lines = [f"{it.kind} {it.text}" for it in items]
    lines.append(".")
    return ("\n".join(lines) + "\n").encode()


def parse_snapshot(data: bytes) -> list[StatusItem]:
    """Inverse of format_snapshot (the device-side parser, in Python)."""
    items: list[StatusItem] = []
    for raw in data.decode().splitlines():
        if raw == ".":
            break
        if not raw:
            continue
        kind = raw[0]
        text = raw[2:] if len(raw) > 1 and raw[1] == " " else raw[1:]
        items.append(StatusItem(kind, text))
    return items


def selftest() -> int:
    items = build_mock_status()
    wire = format_snapshot(items)
    assert parse_snapshot(wire) == items, "snapshot did not round-trip"
    assert wire.endswith(b".\n"), "snapshot must end with a '.' line"
    assert all(it.kind in KINDS for it in items)
    print(f"[selftest] ok: {len(items)} items, {len(wire)} bytes, round-trip clean")
    return 0


def push_serial(port: str, items: list[StatusItem], watch: float) -> int:
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("pyserial is required:  pip install -r bridge/requirements.txt")

    wire = format_snapshot(items)
    with serial.Serial(port, 115200, timeout=1) as ser:
        # opening the port asserts DTR -> a fresh CLI session; nudge + drain banner
        ser.write(b"\r\n")
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"aceteam_status\r\n")
        time.sleep(0.3)

        while True:
            ser.write(wire)
            print(f"[status] pushed {len(items)} items to {port}")
            if watch <= 0:
                break
            time.sleep(watch)
            # a real bridge would re-read here; the mock just re-pushes the same
            items = build_mock_status()
            wire = format_snapshot(items)
        # Ctrl-C (ETX) tells the device the host is done
        ser.write(b"\x03")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="AceTeam Status host bridge (mock).")
    ap.add_argument("--port", help="Flipper serial port; omit to just print the wire bytes")
    ap.add_argument("--watch", type=float, default=0, help="re-push every N seconds (0 = once)")
    ap.add_argument("--selftest", action="store_true", help="round-trip check, no hardware")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    items = build_mock_status()
    if not args.port:
        sys.stdout.buffer.write(format_snapshot(items))
        sys.stdout.flush()
        return 0

    return push_serial(args.port, items, args.watch)


if __name__ == "__main__":
    raise SystemExit(main())
