#!/usr/bin/env python3
"""AceTeam Beacon host bridge (phase 1, mock).

Sends an approval request to the AceTeam Beacon app running on a Flipper Zero,
waits for a human to press approve/deny on the device, and prints an audit line.

This is the aceteam side of the loop. In this mock, "the agent" is just this
script. To wire it into the real product, replace request_approval()'s framing
with an aceteam approvals / ask_human call and keep the same fail-closed policy.

Honesty note: this is L0 (presence). The recorded "device_port" is only the port
the host observed, not a verified device identity. See the README ladder.

Requires:
  - pyserial            (pip install -r requirements.txt)
  - the AceTeam Beacon app running on the Flipper (it registers the
    `aceteam_approve` CLI command while open).
"""
import argparse
import datetime
import json
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required:  pip install -r bridge/requirements.txt")


def request_approval(port: str, action: str, timeout: int) -> str:
    """Return 'approve', 'deny', 'cancelled', or 'timeout'."""
    with serial.Serial(port, 115200, timeout=1) as ser:
        # opening the port asserts DTR -> a fresh CLI session; nudge + drain banner
        ser.write(b"\r\n")
        time.sleep(0.3)
        ser.reset_input_buffer()

        ser.write(f"aceteam_approve {action}\r\n".encode())

        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            chunk = ser.read(256)
            if not chunk:
                continue
            buf += chunk
            for raw in buf.splitlines():
                line = raw.decode(errors="replace").strip()
                if line.startswith("RESULT "):
                    return line[len("RESULT "):].strip()
        return "timeout"


def main() -> int:
    ap = argparse.ArgumentParser(description="AceTeam Beacon host bridge (mock).")
    ap.add_argument("--port", default="/dev/ttyACM0", help="Flipper serial port")
    ap.add_argument("--action", required=True, help="the action to approve")
    ap.add_argument("--timeout", type=int, default=60, help="seconds to wait for a press")
    args = ap.parse_args()

    print(f"[aceteam] an agent wants to: {args.action}")
    print(f"[aceteam] waiting for a human to approve on the beacon ({args.port}) ...")

    decision = request_approval(args.port, args.action, args.timeout)

    record = {
        "ts": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "action": args.action,
        "decision": decision,
        "device_port": args.port,
    }
    print("[audit] " + json.dumps(record))

    if decision == "approve":
        print("[aceteam] approved. proceeding.")
        return 0
    if decision == "deny":
        print("[aceteam] denied. aborting.")
        return 2
    # timeout / cancelled / anything else -> fail closed
    print(f"[aceteam] no decision ({decision}). aborting (fail-closed).")
    return 3


if __name__ == "__main__":
    raise SystemExit(main())
