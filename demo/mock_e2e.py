#!/usr/bin/env python3
"""AceTeam Beacon - no-hardware end-to-end mock ("hello world" for the concept).

Everything is mocked in-process so you can assess the whole flow with no Flipper:
  - a mock aceteam AGENT proposes gated actions,
  - a mock BEACON stands in for the physical device (scripted or interactive),
  - the GATE enforces deny-by-default + fail-closed and writes an audit trail.

Run:
  python3 demo/mock_e2e.py            # scripted, non-interactive (default)
  python3 demo/mock_e2e.py --live     # you play the device: y=approve n=deny enter=walk away

This mirrors the real host bridge (bridge/aceteam_beacon_bridge.py): same
decision values, same fail-closed policy, same audit-record shape. Swapping the
MockBeacon for a SerialBeacon (a real Flipper) is the only change.
"""
from __future__ import annotations

import argparse
import datetime
import json
from dataclasses import dataclass, field

# what the device can answer (mirrors the RESULT line the .fap prints)
APPROVE, DENY, TIMEOUT, CANCELLED = "approve", "deny", "timeout", "cancelled"


@dataclass
class MockBeacon:
    """Stands in for the physical device: 'shows' an action and 'presses'."""

    device_id: str = "mock-beacon-001"
    scripted: list | None = None  # queue of preset answers, or None => interactive
    _i: int = field(default=0)

    def request_approval(self, action: str) -> str:
        # what a human would see on the 1.4" screen
        print("    +------------------------------+")
        print("    | Approve action?              |")
        print(f"    | {action[:28]:<28} |")
        print("    | OK = approve   Back = deny   |")
        print("    +------------------------------+")

        if self.scripted is not None:
            answer = self.scripted[self._i] if self._i < len(self.scripted) else TIMEOUT
            self._i += 1
            print(f"    (mock press: {answer})")
            return answer

        raw = input("    press [y=approve / n=deny / enter=walk away]: ").strip().lower()
        return APPROVE if raw == "y" else DENY if raw == "n" else TIMEOUT


def emit_audit(action: str, decision: str, device_id: str) -> None:
    record = {
        "ts": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "action": action,
        "decision": decision,
        "device": device_id,
    }
    print("  [audit] " + json.dumps(record))


def gate(beacon: MockBeacon, action: str) -> bool:
    """Ask the beacon; enforce deny-by-default / fail-closed; audit. Return proceed?"""
    print(f"\n[agent] requests: {action}")
    decision = beacon.request_approval(action)
    emit_audit(action, decision, beacon.device_id)
    proceed = decision == APPROVE  # ONLY an explicit approve proceeds
    print("  [gate] ->", "PROCEED" if proceed else f"BLOCKED ({decision})")
    return proceed


def main() -> int:
    ap = argparse.ArgumentParser(description="AceTeam Beacon no-hardware mock.")
    ap.add_argument("--live", action="store_true", help="you play the device")
    args = ap.parse_args()

    # a mock agent's queue of gated actions
    actions = [
        "deploy prod v1.482",
        "rm -rf /var/cache/build",
        "spend $200 on GPU hours",
        "ALTER TABLE users DROP col",
    ]

    if args.live:
        beacon = MockBeacon(scripted=None)
    else:
        # scripted so it runs hands-free: approve, deny, approve, walk-away (timeout)
        beacon = MockBeacon(scripted=[APPROVE, DENY, APPROVE, TIMEOUT])

    print("=== AceTeam Beacon: mock end-to-end (agent + device + transport, all in-process) ===")
    results = [(a, gate(beacon, a)) for a in actions]

    approved = sum(1 for _, ok in results if ok)
    print("\n=== summary ===")
    for a, ok in results:
        print(f"  {'PROCEED' if ok else 'BLOCKED'}   {a}")
    print(
        f"\n{approved}/{len(results)} actions proceeded; the rest were blocked "
        "(deny-by-default, fail-closed)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
